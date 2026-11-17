import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEBUGGER = ROOT / "bin" / "minidbg.exe"
DEMO = ROOT / "bin" / "demo.exe"
TARGET = ROOT / "bin" / "test_target.exe"


class Session:
    def __init__(self, target=DEMO, mode=""):
        environment = os.environ.copy()
        environment["MINIDBG_TEST_MODE"] = mode
        self.process = subprocess.Popen(
            [str(DEBUGGER), str(target)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", bufsize=0, env=environment,
        )
        self.queue = queue.Queue()
        self.output = ""
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while character := self.process.stdout.read(1):
            self.queue.put(character)
        self.queue.put(None)

    def until(self, marker="(mdbg) "):
        text = ""
        deadline = time.monotonic() + 20
        while marker not in text:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"Timed out waiting for {marker!r}\n{self.output}")
            try:
                character = self.queue.get(timeout=remaining)
            except queue.Empty:
                raise AssertionError(f"Timed out waiting for {marker!r}\n{self.output}") from None
            if character is None:
                raise AssertionError(f"Unexpected EOF waiting for {marker!r}\n{self.output}")
            text += character
            self.output += character
        return text

    def command(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()
        return self.until()

    def finish(self, command="continue"):
        if command is not None:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()
        text = self.until("Process exited with code ")
        text += self.until("\n")
        self.process.wait(timeout=10)
        return text

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=10)
        self.process.stdin.close()
        self.process.stdout.close()


class DebuggerTests(unittest.TestCase):
    def session(self, target=DEMO, mode=""):
        session = Session(target, mode)
        self.addCleanup(session.close)
        self.assertIn("Initial breakpoint", session.until())
        return session

    def demo_pause(self):
        session = self.session()
        output = session.command("continue")
        self.assertIn("Program breakpoint", output)
        checkpoint = int(re.search(r"checkpoint = (0x[0-9A-F]+)", output)[1], 16)
        total = int(re.search(r"total\s+= (0x[0-9A-F]+)", output)[1], 16)
        return session, checkpoint, total

    def test_breakpoint_repeats_step_and_memory(self):
        session, checkpoint, total = self.demo_pause()
        original = session.command(f"mem {checkpoint:X} 1")
        original_byte = re.search(r"0x[0-9A-F]+\s+([0-9A-F]{2})", original)[1]
        self.assertIn("Breakpoint 1 set", session.command(f"break {checkpoint:X}"))
        self.assertIn(" CC ", session.command(f"mem {checkpoint:X} 1"))
        self.assertIn("already exists", session.command(f"break {checkpoint:X}"))
        for iteration, expected_total in enumerate((0, 1, 3), 1):
            self.assertIn("Hit breakpoint 1", session.command("continue"))
            registers = session.command("regs")
            rip = int(re.search(r"RIP=0x([0-9A-F]+)", registers)[1], 16)
            self.assertEqual(rip, checkpoint)
            self.assertIn(f" {original_byte} ", session.command(f"mem {checkpoint:X} 1"))
            self.assertIn(f" {expected_total:02X} 00 00 00", session.command(f"mem {total:X} 4"))
            if iteration == 1:
                self.assertIn("Step at", session.command("step"))
                registers = session.command("regs")
                next_rip = int(re.search(r"RIP=0x([0-9A-F]+)", registers)[1], 16)
                self.assertNotEqual(next_rip, checkpoint)
                self.assertIn(" CC ", session.command(f"mem {checkpoint:X} 1"))
                self.assertIn("Step at", session.command("step"))
        output = session.finish()
        self.assertIn("total = 6", output)
        self.assertIn("code 0 ", output)
        self.assertIn("Breakpoint hits: 3    Steps: 2", output)

    def test_register_change_markers(self):
        session, checkpoint, _ = self.demo_pause()
        session.command(f"break {checkpoint:X}")
        session.command("continue")
        first = session.command("regs")
        self.assertIn("first snapshot", first)
        self.assertNotRegex(first, r"RIP=0x[0-9A-F]+ \*")
        unchanged = session.command("regs")
        self.assertNotRegex(unchanged, r"RIP=0x[0-9A-F]+ \*")
        session.command("step")
        changed = session.command("regs")
        self.assertRegex(changed, r"RIP=0x[0-9A-F]+ \*")
        self.assertIn("TF=0", changed)
        stable = session.command("regs")
        self.assertNotRegex(stable, r"RIP=0x[0-9A-F]+ \*")
        self.assertIn("code 1 ", session.finish("quit"))

    def test_memory_boundary_and_guard_are_noninvasive(self):
        session = self.session(TARGET, "memory")
        output = session.command("continue")
        mixed = re.search(r"mixed = (0x[0-9A-F]+)", output)[1]
        guard = re.search(r"guard = (0x[0-9A-F]+)", output)[1]
        output = session.command(f"mem {mixed} 16")
        self.assertIn("MiniDbg!", output)
        self.assertIn("8/16 bytes readable", output)
        self.assertIn("?? ?? ??", output)
        self.assertIn("guarded", session.command(f"mem {guard} 16"))
        self.assertIn("executable memory", session.command(f"break {guard}"))
        output = session.finish()
        self.assertIn("guard intact = 1", output)
        self.assertIn("code 0 ", output)

    def test_adjacent_breakpoints_preserve_page_protection(self):
        session = self.session(TARGET, "probe")
        output = session.command("continue")
        probe = int(re.search(r"probe = (0x[0-9A-F]+)", output)[1], 16)
        session.command(f"break {probe:X}")
        session.command(f"break {probe + 1:X}")
        for expected in (1, 2, 1, 2):
            self.assertIn(f"Hit breakpoint {expected}", session.command("continue"))
        output = session.finish()
        self.assertIn("code protection = 32", output)
        self.assertIn("Breakpoint hits: 4", output)
        self.assertIn("code 0 ", output)

    def test_step_onto_another_breakpoint(self):
        session = self.session(TARGET, "probe")
        output = session.command("continue")
        probe = int(re.search(r"probe = (0x[0-9A-F]+)", output)[1], 16)
        session.command(f"break {probe:X}")
        session.command(f"break {probe + 1:X}")
        self.assertIn("Hit breakpoint 1", session.command("continue"))
        self.assertIn(f"Step at 0x{probe + 1:016X}", session.command("step"))
        self.assertIn("Hit breakpoint 2", session.command("continue"))
        self.assertIn(f"Step at 0x{probe + 2:016X}", session.command("step"))
        self.assertIn("Hit breakpoint 1", session.command("continue"))
        self.assertIn("Hit breakpoint 2", session.command("continue"))
        self.assertIn("code 0 ", session.finish())

    def test_blank_input_keeps_program_paused(self):
        session = self.session()
        self.assertNotIn("Invalid command", session.command(""))
        self.assertNotIn("Invalid command", session.command(" \t"))
        self.assertIn("RIP=", session.command("regs"))
        session.finish("quit")

    def test_no_extra_commands_or_terminal_escapes_in_pipes(self):
        session = self.session()
        for command in ("locals", "backtrace", "disasm", "break list", "break delete 1"):
            self.assertIn("Invalid command", session.command(command))
        session.command("regs")
        session.finish("quit")
        self.assertNotIn("\x1b", session.output)

    def test_quoted_executable_path_is_one_argument(self):
        directory = ROOT / "bin" / "argument test"
        directory.mkdir(exist_ok=True)
        target = directory / "test target.exe"
        shutil.copy2(TARGET, target)
        self.addCleanup(shutil.rmtree, directory)
        session = self.session(target, "arguments")
        output = session.finish()
        self.assertIn("argc = 1;", output)
        self.assertIn("test target.exe", output)
        self.assertIn("code 0 ", output)

    def test_final_command_without_newline(self):
        session = self.session()
        session.process.stdin.write("regs")
        session.process.stdin.close()
        output = session.finish(None)
        self.assertIn("RIP=", output)
        self.assertIn("code 1 ", output)

    def test_entry_point_breakpoint(self):
        session = Session()
        self.addCleanup(session.close)
        initial = session.until()
        address = re.search(r"Entry point: (0x[0-9A-F]+)", initial)[1]
        self.assertIn("Breakpoint 1 set", session.command(f"break {address}"))
        self.assertIn("Hit breakpoint 1", session.command("continue"))
        self.assertIn("Program breakpoint", session.command("continue"))
        self.assertIn("code 0 ", session.finish())

    def test_address_aliases(self):
        session = Session()
        self.addCleanup(session.close)
        session.until()
        self.assertIn("Breakpoint 1 set", session.command("break entry"))
        self.assertIn("Hit breakpoint 1", session.command("continue"))
        regs = session.command("regs")
        rip = re.search(r"RIP=(0x[0-9A-F]+)", regs)[1]
        memory = session.command("mem rip 4")
        self.assertIn(rip[2:].lstrip("0"), memory.upper())
        self.assertIn("Invalid command", session.command("break base+xyz"))
        self.assertIn("Invalid command", session.command("mem rzz 4"))
        self.assertIn("code 1 ", session.finish("quit"))

    def test_invalid_commands_and_addresses(self):
        session = self.session()
        for command in ("step extra", "mem 0 257", "break -1", "break 0xXYZ", "quit now"):
            self.assertIn("Invalid command", session.command(command))
        self.assertIn("Command is too long", session.command("r" * 600))
        self.assertIn("failed", session.command("mem 1 16"))
        self.assertIn("executable memory", session.command("break 1"))
        self.assertIn("RIP=", session.command("regs"))
        self.assertIn("code 1 ", session.finish("quit"))

    def test_quit_while_breakpoint_is_disarmed(self):
        session, checkpoint, _ = self.demo_pause()
        session.command(f"break {checkpoint:X}")
        session.command("continue")
        self.assertIn("code 1 ", session.finish("quit"))

    def test_reject_data_and_existing_int3(self):
        session, _, total = self.demo_pause()
        self.assertIn("executable memory", session.command(f"break {total:X}"))
        address = re.search(r"Program breakpoint at (0x[0-9A-F]+)", session.output)[1]
        self.assertIn("already contains INT3", session.command(f"break {address}"))
        self.assertIn("code 0 ", session.finish())

    def test_breakpoint_limit(self):
        session, checkpoint, _ = self.demo_pause()
        for index in range(16):
            self.assertIn(f"Breakpoint {index + 1} set",
                          session.command(f"break {checkpoint + index:X}"))
        self.assertIn("Breakpoint limit: 16", session.command(f"break {checkpoint + 16:X}"))
        self.assertIn("code 1 ", session.finish("quit"))

    def test_eof_terminates_target(self):
        session = self.session()
        session.process.stdin.close()
        self.assertIn("code 1 ", session.finish(None))

    def test_multithreaded_breakpoint(self):
        for _ in range(20):
            self.check_multithreaded_breakpoint()

    def check_multithreaded_breakpoint(self):
        session = self.session(TARGET)
        output = session.command("continue")
        checkpoint = re.search(r"checkpoint = (0x[0-9A-F]+)", output)[1]
        session.command(f"break {checkpoint}")
        thread_ids = set()
        for _ in range(6):
            output = session.command("continue")
            self.assertIn("Hit breakpoint 1", output)
            thread_ids.add(re.search(r"thread (\d+)", output)[1])
        self.assertEqual(len(thread_ids), 2)
        output = session.finish()
        self.assertIn("visits = 6", output)
        self.assertIn("code 0 ", output)

    def test_exception_is_delivered_to_target_handler(self):
        session = self.session(TARGET, "handled")
        output = session.command("continue")
        self.assertIn("Exception 0xE0000042", output)
        self.assertIn("first chance", output)
        self.assertIn("Cannot step an exception", session.command("step"))
        output = session.finish()
        self.assertIn("Exception handled by target", output)
        self.assertIn("code 7 ", output)

    def test_unhandled_crash(self):
        session = self.session(TARGET, "crash")
        first = session.command("continue")
        self.assertIn("Exception 0xC0000005", first)
        self.assertIn("first chance", first)
        self.assertIn("RIP=", session.command("regs"))
        second = session.command("continue")
        self.assertIn("second chance", second)
        self.assertIn("0xC0000005", session.finish())

    def test_nonzero_exit_code(self):
        session = self.session(TARGET, "exit")
        self.assertIn("code 7 ", session.finish())

    def test_path_with_spaces_and_unicode(self):
        directory = ROOT / "bin" / "test path кирилиця"
        directory.mkdir(exist_ok=True)
        target = directory / "demo program.exe"
        shutil.copy2(DEMO, target)
        self.addCleanup(shutil.rmtree, directory)
        session = self.session(target)
        session.command("continue")
        self.assertIn("code 0 ", session.finish())

    def test_missing_target(self):
        result = subprocess.run([str(DEBUGGER), str(ROOT / "missing.exe")],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Program not found", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
