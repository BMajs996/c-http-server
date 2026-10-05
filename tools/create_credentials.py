"""Generate separate private client and server credential files without printing secrets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets


def write_private(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, 'w') as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind', choices=['token', 'hmac'], required=True)
    parser.add_argument('--id', required=True)
    parser.add_argument('--client-file', type=Path, required=True)
    parser.add_argument('--server-file', type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', args.id):
        parser.error('id must contain 1–32 letters, digits, underscores, or hyphens')
    secret = secrets.token_bytes(32)
    record = {'kind': args.kind, 'id': args.id, 'secret': secret.hex()}
    server_secret = hashlib.sha256(secret).hexdigest() if args.kind == 'token' else secret.hex()
    created = False
    try:
        write_private(args.client_file, json.dumps(record) + '\n')
        created = True
        write_private(args.server_file, f'{args.kind} {args.id} {server_secret}\n')
    except OSError:
        if created:
            args.client_file.unlink(missing_ok=True)
        parser.exit(1, 'Could not create credential files; check paths and permissions (files must not already exist)\n')
    print('Created private client and server credential files. No secrets printed.')


if __name__ == '__main__':
    main()
