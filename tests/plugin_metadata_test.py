import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "cmake"))
from plugin_metadata import c_bytes, validate


class PluginMetadataTest(unittest.TestCase):
    def test_full_semver_and_utf8_name(self):
        for version in ("0.0.0", "1.2.3", "1.2.3-rc.1+build.001", "1.2.3-0.alpha-2"):
            validate("com.example.counter", "计数器", version)

    def test_invalid_inputs_fail_before_code_generation(self):
        for version in ("1.2", "01.2.3", "1.2.3-01", "1.2.3-", "1.2.3+", "1.2.3\n", "v1.2.3"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                validate("com.example.counter", "Counter", version)
        for identity in ("", "../escape", "Bad.Name", "a\0b", "x" * 256):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                validate(identity, "Counter", "1.0.0")
        for name in ("", "x\0y", "中" * 86):
            with self.subTest(name=name), self.assertRaises(ValueError):
                validate("com.example.counter", name, "1.0.0")

    def test_c_literals_escape_quotes_backslashes_and_utf8(self):
        self.assertEqual(c_bytes('"\\\n'), '"\\042\\134\\012"')
        self.assertEqual(c_bytes("é"), '"\\303\\251"')


if __name__ == "__main__":
    unittest.main()
