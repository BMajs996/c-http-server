"""Generate deterministic, verified gzip sidecars for publishable text assets."""
import argparse
import gzip
import hashlib
import os
from pathlib import Path
import tempfile

EXTENSIONS = {'.html', '.htm', '.css', '.js', '.json', '.txt', '.svg'}


def compress_asset(source):
    target = source.with_name(source.name + '.gz')
    if target.is_symlink():
        raise ValueError(f'refusing symlink sidecar: {target}')
    before = source.stat()
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=source.parent, prefix='.gzip-', delete=False) as output:
            temporary = Path(output.name)
            digest = hashlib.sha256()
            with source.open('rb') as original, gzip.GzipFile(filename='', mode='wb', fileobj=output, mtime=0) as compressed:
                while chunk := original.read(65536):
                    digest.update(chunk)
                    compressed.write(chunk)
        after = source.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError(f'source changed during compression: {source}')
        verified = hashlib.sha256()
        with gzip.open(temporary, 'rb') as compressed:
            while chunk := compressed.read(65536):
                verified.update(chunk)
        if digest.digest() != verified.digest():
            raise ValueError(f'gzip verification failed: {source}')
        if temporary.stat().st_size >= before.st_size:
            if target.exists():
                target.unlink()
            return False
        os.chmod(temporary, before.st_mode & 0o777)
        timestamp = max(after.st_mtime_ns, temporary.stat().st_mtime_ns)
        os.utime(temporary, ns=(timestamp, timestamp))
        temporary.replace(target)
        return True
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path, nargs='?', default=Path('public'))
    args = parser.parse_args()
    if not args.root.is_dir():
        parser.error('root must be a directory')
    try:
        for source in sorted(args.root.rglob('*')):
            if source.is_symlink() or not source.is_file() or source.suffix.lower() not in EXTENSIONS:
                continue
            if any(part.startswith('.') for part in source.relative_to(args.root).parts):
                continue
            print(f'{source}: {"gzip written" if compress_asset(source) else "not smaller; sidecar omitted"}')
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
