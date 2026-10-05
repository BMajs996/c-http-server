"""Client credentials and versioned request signing; Python standard library only."""
import argparse
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import ssl
import stat
import sys
import time


def load_credential(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        stamp = os.fstat(fd)
        if not stat.S_ISREG(stamp.st_mode) or stamp.st_mode & 0o077 or stamp.st_uid != os.geteuid() or stamp.st_size > 4096:
            raise ValueError('client credential file must be private, regular, and owned by this user')
        with os.fdopen(fd, 'r', closefd=False) as source:
            credential = json.loads(source.read(4097))
    finally:
        os.close(fd)
    if (not isinstance(credential, dict) or credential.get('kind') not in ('token', 'hmac')
            or not isinstance(credential.get('id'), str)
            or not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', credential['id'])
            or not isinstance(credential.get('secret'), str)
            or not re.fullmatch(r'[0-9a-f]{64}', credential['secret'])):
        raise ValueError('invalid client credential record')
    return credential


def canonical_request(key_id, timestamp, nonce, method, target, content_type, content_encoding, body):
    fields = ['C-HTTP-HMAC-V1', key_id, str(timestamp), nonce, method, target,
              content_type.strip(' \t'), content_encoding.strip(' \t'), str(len(body)), hashlib.sha256(body).hexdigest()]
    if any('\r' in field or '\n' in field for field in fields):
        raise ValueError('canonical fields cannot contain line breaks')
    return ('\n'.join(fields) + '\n').encode('ascii')


def auth_headers(credential, method, target, body=b'', content_type='', content_encoding='', timestamp=None, nonce=None):
    if credential['kind'] == 'token':
        return {'Authorization': f'Bearer {credential["id"]}.{credential["secret"]}'}
    timestamp = int(time.time()) if timestamp is None else timestamp
    nonce = secrets.token_hex(16) if nonce is None else nonce
    canonical = canonical_request(credential['id'], timestamp, nonce, method, target, content_type, content_encoding, body)
    signature = hmac.new(bytes.fromhex(credential['secret']), canonical, hashlib.sha256).hexdigest()
    return {'X-Auth-Key-Id': credential['id'], 'X-Auth-Timestamp': str(timestamp),
            'X-Auth-Nonce': nonce, 'X-Auth-Signature': signature}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--credential', type=Path, required=True)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--method', choices=['GET', 'HEAD', 'POST'])
    parser.add_argument('--path')
    parser.add_argument('--body-file', type=Path)
    parser.add_argument('--https', action='store_true')
    parser.add_argument('--ca-file', type=Path)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or (args.ca_file and not args.https):
        parser.error('invalid port or CA file without HTTPS')
    client = None
    try:
        credential = load_credential(args.credential)
        method = args.method or ('GET' if credential['kind'] == 'token' else 'POST')
        target = args.path or ('/api/private/status' if credential['kind'] == 'token' else '/api/private/echo')
        if not target.startswith('/') or len(target) >= 2048:
            raise ValueError('invalid request target')
        body = args.body_file.read_bytes() if args.body_file else (b'{}' if method == 'POST' else b'')
        if len(body) > 16384:
            raise ValueError('body exceeds 16 KiB')
        content_type = 'application/json' if method == 'POST' else ''
        headers = auth_headers(credential, method, target, body, content_type)
        if content_type:
            headers['Content-Type'] = content_type
        headers['Connection'] = 'close'
        if args.https:
            context = ssl.create_default_context(cafile=args.ca_file)
            client = http.client.HTTPSConnection(args.host, args.port, context=context, timeout=10)
        else:
            client = http.client.HTTPConnection(args.host, args.port, timeout=10)
        client.request(method, target, body=body, headers=headers)
        response = client.getresponse()
        data = response.read()
        print(f'HTTP {response.status}')
        sys.stdout.flush()
        sys.stdout.buffer.write(data)
        return int(response.status != 200)
    except (OSError, ValueError, http.client.HTTPException):
        print('Authenticated request failed; check credential file, connection, and request settings', file=sys.stderr)
        return 1
    finally:
        if client:
            client.close()


if __name__ == '__main__':
    raise SystemExit(main())
