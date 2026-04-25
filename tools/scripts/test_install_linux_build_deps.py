import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("install_linux_build_deps.sh")


class InstallLinuxBuildDepsTest(unittest.TestCase):
    def test_linux_빌드_의존성_스크립트는_lf_줄바꿈을_사용한다(self):
        data = SCRIPT_PATH.read_bytes()

        self.assertTrue(data.startswith(b"#!/usr/bin/env bash\n"))
        self.assertNotIn(b"\r\n", data)


if __name__ == "__main__":
    unittest.main()
