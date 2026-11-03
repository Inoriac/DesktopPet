import os
import sys
import unittest
from unittest import mock


REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPOSITORY_ROOT, "launcher"))

import atomic_write  # noqa: E402


class AtomicWriteTests(unittest.TestCase):
    def test_permission_error_is_raised_without_retrying(self):
        calls = []

        def denied(path, *args, **kwargs):
            calls.append(path)
            raise PermissionError(13, "Permission denied", path)

        with mock.patch.object(atomic_write.os, "open", side_effect=denied):
            with self.assertRaises(PermissionError):
                atomic_write.atomic_write_text(
                    r"C:\denied-launcher-dir\launch_config.json", "{}")

        self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()
