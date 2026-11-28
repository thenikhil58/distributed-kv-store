import os
from pathlib import Path
import tempfile
import unittest

from tools.rotate_credentials import activate_bundle


class CredentialRotationTests(unittest.TestCase):
    @staticmethod
    def create_bundle(path):
        path.mkdir()
        os.chmod(path, 0o700)
        for name in ("ca.pem", "cert.pem", "key.pem", "auth.conf", "node.token"):
            (path / name).write_text("credential", encoding="utf-8")
        for name in ("key.pem", "auth.conf", "node.token"):
            os.chmod(path / name, 0o600)
        (path / "crls").mkdir(mode=0o700)
        os.chmod(path / "crls", 0o700)

    def test_switches_active_bundle_atomically(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current = root / "current"
            first = root / "v1"
            second = root / "v2"
            self.create_bundle(first)
            self.create_bundle(second)
            current.symlink_to(first.name)

            old_target, new_target = activate_bundle(current, second, [])

            self.assertEqual(old_target, first)
            self.assertEqual(new_target, second)
            self.assertEqual(current.resolve(), second)

    def test_rejects_insecure_private_file_without_switching(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current = root / "current"
            first = root / "v1"
            second = root / "v2"
            self.create_bundle(first)
            self.create_bundle(second)
            current.symlink_to(first.name)
            os.chmod(second / "key.pem", 0o644)

            with self.assertRaisesRegex(ValueError, "key.pem"):
                activate_bundle(current, second, [])

            self.assertEqual(current.resolve(), first)


if __name__ == "__main__":
    unittest.main()
