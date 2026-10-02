"""Repeatable HTTP/1.1 benchmark and saved-result comparison; standard library only."""
import argparse
import concurrent.futures
import hashlib
import http.client
import json
import platform
import ssl
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


def percentile(values, fraction):
    if not values:
        return None
    index = (len(values) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(values) - 1)
    return values[lower] + (values[upper] - values[lower]) * (index - lower)


def run(host, port, path, concurrency, requests, warmup, reuse, context=None):
    def connection():
        cls = http.client.HTTPSConnection if context else http.client.HTTPConnection
        kwargs = {'timeout': 10}
        if context:
            kwargs['context'] = context
        return cls(host, port, **kwargs)

    warmup_errors = []
    for _ in range(warmup):
        client = connection()
        try:
            client.request('GET', path, headers={'Connection': 'close'})
            response = client.getresponse()
            response.read()
            if response.status != 200:
                raise RuntimeError(f'HTTP {response.status}')
        except Exception as error:
            warmup_errors.append(str(error))
        finally:
            client.close()

    def worker(count):
        client = None
        latencies, errors, total_bytes = [], [], 0
        try:
            for _ in range(count):
                start = time.perf_counter()
                try:
                    if client is None:
                        client = connection()
                    client.request('GET', path, headers={} if reuse else {'Connection': 'close'})
                    response = client.getresponse()
                    body = response.read()
                    if response.status != 200:
                        raise RuntimeError(f'HTTP {response.status}')
                    total_bytes += len(body)
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
        results = list(pool.map(worker, counts))
    duration = time.perf_counter() - start
    latencies = sorted(value for item in results for value in item[0])
    errors = [error for item in results for error in item[1]]
    return {
        'scheme': 'https' if context else 'http', 'host': host, 'port': port,
        'path': path, 'concurrency': concurrency, 'requests': requests,
        'keep_alive': reuse, 'warmup': warmup, 'completed': len(latencies),
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
    return {key: trial.get(key, 'http' if key == 'scheme' else None)
            for key in ('scheme', 'host', 'port', 'path', 'concurrency', 'requests', 'keep_alive', 'warmup')}


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
    parser.add_argument('--path', default='/health')
    parser.add_argument('--concurrency', type=int, default=16)
    parser.add_argument('--requests', type=int, default=2000)
    parser.add_argument('--warmup', type=int, default=20)
    parser.add_argument('--trials', type=int, default=5)
    parser.add_argument('--https', action='store_true')
    parser.add_argument('--ca-file', type=Path)
    parser.add_argument('--fresh', action='store_true')
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
        config = ({'path': str(args.config),
                   'sha256': hashlib.sha256(args.config.read_bytes()).hexdigest()}
                  if args.config else None)
        context = ssl.create_default_context(cafile=str(args.ca_file) if args.ca_file else None) if args.https else None
    except (OSError, ssl.SSLError) as error:
        parser.error(str(error))
    trials = []
    for number in range(args.trials):
        result = run(args.host, args.port, args.path, args.concurrency, args.requests,
                     args.warmup, not args.fresh, context)
        trials.append(result)
        print(f'trial {number + 1}/{args.trials}: {result["completed"]}/{args.requests} completed, '
              f'{result["errors"]} errors, {result["successful_requests_per_s"]} req/s', file=sys.stderr)
    record = {
        'format_version': 2, 'label': args.label, 'timestamp_utc': datetime.now(timezone.utc).isoformat(),
        'environment': {'python': platform.python_version(), 'platform': platform.platform(),
                        'machine': platform.machine(), 'processor_count': __import__('os').cpu_count()},
        'config': config, 'trials': trials, 'summary': summary(trials),
    }
    output = json.dumps(record, indent=2) + '\n'
    print(output, end='')
    if args.output:
        args.output.write_text(output)
    return int(record['summary']['errors'] != 0)


if __name__ == '__main__':
    raise SystemExit(main())
