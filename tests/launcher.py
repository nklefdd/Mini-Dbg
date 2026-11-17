import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEBUGGER = ROOT / "bin" / "minidbg.exe"
TARGET = ROOT / "bin" / "test_target.exe"
DEMO = ROOT / "bin" / "demo.exe"


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory(prefix="launcher_tests_", dir=ROOT / "bin")
        self.addCleanup(self.workspace.cleanup)
        self.directory = Path(self.workspace.name)
        self.environment = os.environ.copy()
        self.environment["MINIDBG_TEST_MODE"] = "exit"

    def run_debugger(self, commands, arguments=(), path=None):
        environment = self.environment.copy()
        if path is not None:
            environment["PATH"] = str(path) + os.pathsep + environment.get("PATH", "")
        result = subprocess.run(
            [str(DEBUGGER), *map(str, arguments)], input=commands,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            cwd=self.directory, env=environment, text=True,
            encoding="utf-8", errors="replace", timeout=25,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def assert_target(self, output, expected):
        match = next((line for line in output.splitlines() if "Target: " in line), None)
        self.assertIsNotNone(match, output)
        actual = match.split("Target: ", 1)[1].strip()
        self.assertEqual(os.path.normcase(os.path.normpath(actual)),
                         os.path.normcase(os.path.normpath(str(expected))), output)

    def test_executable_directory_and_optional_extension(self):
        output = self.run_debugger("test_target\ncontinue\n")
        self.assertIn("Program> ", output)
        self.assert_target(output, TARGET)
        self.assertIn("Initial breakpoint", output)
        self.assertIn("Process exited with code 7 ", output)

    def test_current_directory_has_priority(self):
        selected = self.directory / "test_target.exe"
        shutil.copy2(DEMO, selected)
        output = self.run_debugger("test_target.exe\ncontinue\ncontinue\n")
        self.assert_target(output, selected)
        self.assertIn("total = 6", output)
        self.assertIn("Process exited with code 0 ", output)

    def test_path_lookup_and_executable_directory_priority(self):
        path_directory = self.directory / "path"
        path_directory.mkdir()
        selected = path_directory / "path_probe.exe"
        shutil.copy2(TARGET, selected)
        output = self.run_debugger("path_probe\ncontinue\n", path=path_directory)
        self.assert_target(output, selected)
        self.assertIn("Process exited with code 7 ", output)
        shutil.copy2(DEMO, path_directory / "test_target.exe")
        output = self.run_debugger("test_target\ncontinue\n", path=path_directory)
        self.assert_target(output, TARGET)
        self.assertIn("Process exited with code 7 ", output)

    def test_unicode_path_with_and_without_quotes(self):
        selected = self.directory / "приклад з пробілами.exe"
        shutil.copy2(TARGET, selected)
        for entered in (str(selected), f'"{selected}"'):
            with self.subTest(entered=entered):
                output = self.run_debugger(entered + "\ncontinue\n")
                self.assert_target(output, selected)
                self.assertIn("Process exited with code 7 ", output)

    def test_explicit_missing_path_does_not_fall_back(self):
        output = self.run_debugger("missing\\test_target.exe\nquit\n")
        self.assertIn("Program not found", output)
        self.assertNotIn("Initial breakpoint", output)
        self.assertNotIn("Target: ", output)

    def test_missing_and_nonexecutable_then_retry(self):
        invalid = self.directory / "document.txt"
        invalid.write_text("This is not an executable.", encoding="utf-8")
        output = self.run_debugger("absent_74382.exe\ndocument.txt\ntest_target\ncontinue\n")
        self.assertIn("Program not found", output)
        self.assertIn("GetBinaryTypeW failed", output)
        self.assertGreaterEqual(output.count("Program> "), 3)
        self.assert_target(output, TARGET)
        self.assertIn("Process exited with code 7 ", output)

    def test_eof_quit_and_blank_input(self):
        for commands in ("", "quit\n", "\n \t\nquit\n"):
            with self.subTest(commands=commands):
                output = self.run_debugger(commands)
                self.assertIn("Program> ", output)
                self.assertNotIn("Initial breakpoint", output)
                self.assertNotIn("Process exited with code", output)

    def test_piped_target_preserves_following_debugger_commands(self):
        output = self.run_debugger("test_target\nregs\nquit\n")
        self.assertIn("RIP=0x", output)
        self.assertIn("Process exited with code 1 ", output)
        self.assertNotIn("Invalid command", output)

    def test_overlong_input_is_drained_before_retry(self):
        output = self.run_debugger("x" * 131073 + "\ntest_target\ncontinue\n")
        self.assertIn("Program name is too long", output)
        self.assert_target(output, TARGET)
        self.assertIn("Process exited with code 7 ", output)

    def test_command_line_uses_the_same_resolver(self):
        output = self.run_debugger("continue\n", arguments=("test_target",))
        self.assertNotIn("Program> ", output)
        self.assert_target(output, TARGET)
        self.assertIn("Initial breakpoint", output)
        self.assertIn("Process exited with code 7 ", output)

    def test_extended_path_prefix(self):
        extended = "\\\\?\\" + str(TARGET)
        output = self.run_debugger(extended + "\ncontinue\n")
        self.assert_target(output, extended)
        self.assertIn("Process exited with code 7 ", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
