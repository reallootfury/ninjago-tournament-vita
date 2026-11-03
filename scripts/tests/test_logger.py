#!/usr/bin/env python3
"""Compile and run the real logger in standard and optional-logging modes.

Run from the repository root: python3 scripts/tests/test_logger.py
Only Vita platform calls are replaced; all file effects stay in temporary dirs.
These host checks do not establish Vita threading or filesystem correctness.
"""

from pathlib import Path
import subprocess
import tempfile
import unittest


class LoggerModesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[2]
        cls.build = tempfile.TemporaryDirectory(prefix="ninjago-logger-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.executables = {}
        for mode in ("off", "on"):
            executable = Path(cls.build.name) / f"logger-{mode}"
            command = [
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                '-DDATA_PATH="ux0:data/ninjago/"',
                "-I", str(cls.root / "scripts/tests/logger-stubs"),
                "-I", str(cls.root / "source"),
            ]
            if mode == "on":
                command.append("-DNINJAGO_RUNTIME_LOGGING")
            command.extend([
                str(cls.root / "scripts/tests/logger_test.c"),
                str(cls.root / "source/utils/logger.c"),
                "-o", str(executable),
            ])
            subprocess.run(command, check=True)
            cls.executables[mode] = executable

    def run_mode(self, mode):
        with tempfile.TemporaryDirectory(prefix=f"ninjago-logger-{mode}-") as work:
            result = subprocess.run(
                [str(self.executables[mode])], cwd=work,
                capture_output=True, text=True, check=False,
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")

    def test_logging_off_preserves_existing_log_without_file_io(self):
        self.run_mode("off")

    def test_logging_on_replaces_old_log_and_persists_messages(self):
        self.run_mode("on")


if __name__ == "__main__":
    unittest.main()
