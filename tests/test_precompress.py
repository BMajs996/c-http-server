"""Asset publication checks for the offline gzip tool."""
import gzip
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('precompress', Path(__file__).resolve().parents[1] / 'tools/precompress.py')
assert spec is not None and spec.loader is not None
precompress = importlib.util.module_from_spec(spec)
spec.loader.exec_module(precompress)


class PrecompressTests(unittest.TestCase):
    def test_deterministic_verified_output_and_omission(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'asset.txt'
            source.write_bytes(b'consistent asset\n' * 1000)
            self.assertTrue(precompress.compress_asset(source))
            target = source.with_name(source.name + '.gz')
            first = target.read_bytes()
            self.assertEqual(gzip.decompress(first), source.read_bytes())
            self.assertTrue(precompress.compress_asset(source))
            self.assertEqual(first, target.read_bytes())
            source.write_bytes(b'x')
            self.assertFalse(precompress.compress_asset(source))
            self.assertFalse(target.exists())
            self.assertFalse(list(source.parent.glob('.gzip-*')))

    def test_sidecar_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'asset.txt'
            source.write_bytes(b'asset' * 1000)
            source.with_name(source.name + '.gz').symlink_to(source)
            with self.assertRaises(ValueError):
                precompress.compress_asset(source)
            self.assertEqual(source.read_bytes(), b'asset' * 1000)


if __name__ == '__main__':
    unittest.main()
