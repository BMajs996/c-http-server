"""Repeatable HTTP/1.1 benchmark and saved-result comparison; standard library only."""
import argparse
import concurrent.futures
import hashlib
import http.client
import json
import os
import platform
import ssl
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from auth_client import auth_headers, load_credential


def percentile(values, fraction):
    if not values:
        return None
    index = (len(values) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(values) - 1)
    return values[lower] + (values[upper] - values[lower]) * (index - lower)


def run(host, port, path, concurrency, requests, warmup, reuse, context=None, paths=None, accept_encoding=None, method="GET", body=None, credential=None):
    targets = paths or [path]
    headers = {"Accept-Encoding": accept_encoding} if accept_encoding is not None else {}
    content_type = 'application/json' if method == 'POST' else ''
    if content_type:
        headers['Content-Type'] = content_type
    def request_headers(target):
        result = dict(headers)
        if credential:
            result.update(auth_headers(credential, method, target, body or b'', content_type))
        return result
    def connection():
        cls = http.client.HTTPSConnection if context else http.client.HTTPConnection
        kwargs = {'timeout': 10}
        if context:
            kwargs['context'] = context
        return cls(host, port, **kwargs)

    warmup_errors = []
    for index in range(warmup):
        client = connection()
        try:
            target = targets[index % len(targets)]
            client.request(method, target, body=body, headers={**request_headers(target), 'Connection': 'close'})
            response = client.getresponse()
            response.read()
            if response.status != 200:
                raise RuntimeError(f'HTTP {response.status}')
        except Exception as error:
            warmup_errors.append(str(error))
        finally:
            client.close()

    def worker(work):
        first, count = work
        client = None
        latencies, errors, total_bytes = [], [], 0
        try:
            for offset in range(count):
                start = time.perf_counter()
                try:
                    if client is None:
                        client = connection()
                    target = targets[(first + offset) % len(targets)]
                    client.request(method, target, body=body,
                                   headers=request_headers(target) if reuse else {**request_headers(target), 'Connection': 'close'})
                    response = client.getresponse()
                    received = response.read()
                    if response.status != 200:
                        raise RuntimeError(f'HTTP {response.status}')
                    total_bytes += len(received)
                    latencies.append((time.perf_counter() - start) * 1000)
                    if not reuse:
                        client.close()
                        client = None
                except Exception as error:
                    errors.append(str(error))
                    if client:
                        client.close()
                        client = None
        finally:
            if client:
                client.close()
        return latencies, errors, total_bytes

    start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        counts = [requests // concurrency + (i < requests % concurrency) for i in range(concurrency)]
        starts = [sum(counts[:i]) for i in range(concurrency)]
        results = list(pool.map(worker, zip(starts, counts)))
    duration = time.perf_counter() - start
    latencies = sorted(value for item in results for value in item[0])
    errors = [error for item in results for error in item[1]]
    return {
        'scheme': 'https' if context else 'http', 'host': host, 'port': port,
        'path': path if paths is None else None,
        'paths_sha256': hashlib.sha256(('\n'.join(targets) + '\n').encode()).hexdigest() if paths else None,
        'method': method, 'authentication': credential['kind'] if credential else None,
        'body_sha256': hashlib.sha256(body).hexdigest() if body is not None else None,
        'body_length': len(body) if body is not None else 0,
        'path_count': len(targets), 'concurrency': concurrency, 'requests': requests,
        'keep_alive': reuse, 'warmup': warmup, 'accept_encoding': accept_encoding, 'completed': len(latencies),
        'errors': len(errors), 'error_samples': errors[:5],
        'warmup_errors': len(warmup_errors), 'warmup_error_samples': warmup_errors[:5],
        'elapsed_s': round(duration, 4),
        'successful_requests_per_s': round(len(latencies) / duration, 2),
        'latency_ms': {name: round(percentile(latencies, fraction), 3) if latencies else None
                       for name, fraction in [('p50', .50), ('p95', .95), ('p99', .99)]},
        'body_bytes': sum(item[2] for item in results),
    }


def summary(trials):
    valid = [trial for trial in trials if trial['errors'] == 0 and trial.get('warmup_errors', 0) == 0]
    metrics = {'successful_requests_per_s': lambda trial: trial['successful_requests_per_s']}
    for name in ('p50', 'p95', 'p99'):
        metrics[f'latency_ms.{name}'] = lambda trial, name=name: trial['latency_ms'][name]
    return {
        'trials': len(trials), 'valid_trials': len(valid),
        'completed': sum(trial['completed'] for trial in trials),
        'errors': sum(trial['errors'] + trial.get('warmup_errors', 0) for trial in trials),
        'median': {name: round(statistics.median(getter(trial) for trial in valid), 3)
                   if valid else None for name, getter in metrics.items()},
    }


def workload(result):
    trial = result['trials'][0] if 'trials' in result else result
    return {key: trial.get(key, 'http' if key == 'scheme' else 'GET' if key == 'method' else 1 if key == 'path_count' else 0 if key == 'body_length' else None)
            for key in ('scheme', 'host', 'port', 'path', 'paths_sha256', 'path_count',
                        'concurrency', 'requests', 'keep_alive', 'warmup', 'accept_encoding',
                        'method', 'authentication', 'body_sha256', 'body_length')}


def normalized(result):
    return result['summary'] if 'summary' in result else summary([result])


def compare(left, right):
    a, b = json.loads(left.read_text()), json.loads(right.read_text())
    differing = [key for key in workload(a) if workload(a)[key] != workload(b)[key]]
    if differing:
        raise ValueError('workloads differ: ' + ', '.join(differing))
    sa, sb = normalized(a), normalized(b)
    print(f'{left}: {sa["valid_trials"]}/{sa["trials"]} valid; {sa["errors"]} errors')
    print(f'{right}: {sb["valid_trials"]}/{sb["trials"]} valid; {sb["errors"]} errors')
    if not sa['valid_trials'] or not sb['valid_trials']:
        raise ValueError('comparison requires at least one error-free trial on each side')
    for name, before in sa['median'].items():
        after = sb['median'][name]
        change = (after / before - 1) * 100 if before else float('nan')
        unit = ' req/s' if name == 'successful_requests_per_s' else ' ms'
        print(f'{name}: {before:.3f}{unit} -> {after:.3f}{unit} ({change:+.1f}%)')
    return int(sa['errors'] != 0 or sb['errors'] != 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--method', choices=['GET', 'POST'], default='GET')
    parser.add_argument('--body-file', type=Path, help='request payload (maximum 16 KiB)')
    parser.add_argument('--credential', type=Path, help='private client credential file; secrets are not saved')
    parser.add_argument('--path', default='/health')
    parser.add_argument('--paths-file', type=Path, help='one request path per line, cycled across requests')
    parser.add_argument('--concurrency', type=int, default=16)
    parser.add_argument('--requests', type=int, default=2000)
    parser.add_argument('--warmup', type=int, default=20)
    parser.add_argument('--trials', type=int, default=5)
    parser.add_argument('--https', action='store_true')
    parser.add_argument('--ca-file', type=Path)
    parser.add_argument('--fresh', action='store_true')
    parser.add_argument('--accept-encoding', help='recorded Accept-Encoding request header')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--config', type=Path, help='server configuration to record, including its SHA-256')
    parser.add_argument('--label', help='descriptive run label, such as cache-on')
    parser.add_argument('--compare', nargs=2, type=Path, metavar=('BASELINE', 'CANDIDATE'))
    args = parser.parse_args()
    if args.compare:
        try:
            return compare(*args.compare)
        except (OSError, KeyError, ValueError, TypeError) as error:
            parser.error(str(error))
    if not 1 <= args.port <= 65535 or not args.path.startswith('/'):
        parser.error('port must be 1–65535 and path must start with /')
    if not 1 <= args.concurrency <= 512 or not 1 <= args.requests <= 1000000:
        parser.error('invalid workload bounds')
    if not 0 <= args.warmup <= 10000 or not 1 <= args.trials <= 100:
        parser.error('invalid warmup or trial count')
    if args.ca_file and not args.https:
        parser.error('--ca-file requires --https')
    try:
        paths = ([line.strip() for line in args.paths_file.read_text().splitlines()
                  if line.strip() and not line.lstrip().startswith('#')]
                 if args.paths_file else None)
        if paths is not None and (not paths or len(paths) > 4096 or
                                  any(not item.startswith('/') for item in paths)):
            parser.error('--paths-file must contain 1–4096 paths beginning with /')
        config = ({'path': str(args.config),
                   'sha256': hashlib.sha256(args.config.read_bytes()).hexdigest()}
                  if args.config else None)
        body = args.body_file.read_bytes() if args.body_file else b'{}' if args.method == 'POST' else None
        if body is not None and len(body) > 16384:
            parser.error('body exceeds 16 KiB')
        credential = load_credential(args.credential) if args.credential else None
        context = ssl.create_default_context(cafile=str(args.ca_file) if args.ca_file else None) if args.https else None
    except (OSError, ssl.SSLError, ValueError) as error:
        parser.error(str(error))
    trials = []
    for number in range(args.trials):
        result = run(args.host, args.port, args.path, args.concurrency, args.requests,
                     args.warmup, not args.fresh, context, paths, args.accept_encoding, args.method, body, credential)
        trials.append(result)
        print(f'trial {number + 1}/{args.trials}: {result["completed"]}/{args.requests} completed, '
              f'{result["errors"]} errors, {result["successful_requests_per_s"]} req/s', file=sys.stderr)
    record = {
        'format_version': 2, 'label': args.label, 'timestamp_utc': datetime.now(timezone.utc).isoformat(),
        'environment': {'python': platform.python_version(), 'platform': platform.platform(),
                        'machine': platform.machine(), 'processor_count': os.cpu_count()},
        'config': config, 'trials': trials, 'summary': summary(trials),
    }
    output = json.dumps(record, indent=2) + '\n'
    print(output, end='')
    if args.output:
        args.output.write_text(output)
    return int(record['summary']['errors'] != 0)


if __name__ == '__main__':
    raise SystemExit(main())
