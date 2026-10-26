"""CLI integration checks. Run with python3 company_cli_test.py /path/to/company-story."""
import os
import pathlib
import select
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())


class CompanyCLI(unittest.TestCase):
    def run_game(self, *args, input=""):
        return subprocess.run([BINARY, *args], input=input, text=True, capture_output=True, timeout=10)

    def test_replay_and_transcript(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "chronicle.txt"
            first = self.run_game("--demo", "--seed", "42", "--out", str(output))
            second = self.run_game("--demo", "--seed", "42")
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(first.stdout, second.stdout)
            self.assertEqual(output.read_text(), first.stdout)
            self.assertIn("RENEWED FOR ANOTHER SEASON", first.stdout)
            self.assertIn("Great Regional Abduction", first.stdout)
            self.assertIn("WEEK 52 / 52", first.stdout)
            self.assertIn("LAUNCH", first.stdout)
            self.assertIn("DEVELOPMENT", first.stdout)
            self.assertNotIn("DVD", first.stdout)
            self.assertIn("Monday planning", first.stdout)
            self.assertEqual(first.stdout.count("MONTHLY REVIEW / month"), 13)
            self.assertNotIn("+18 progress", first.stdout)
            self.assertNotIn("Product ", first.stdout)
            self.assertIn("MAINTENANCE", first.stdout)
            self.assertIn("model release", first.stdout)
            self.assertIn("subscription by 20%", first.stdout)

    def test_invalid_arguments(self):
        for args in [("--seed", "-1"), ("--seed", "4294967296"), ("--seed", "12oops"),
                     ("--seed",), ("--path", "music"), ("--unknown",)]:
            self.assertNotEqual(self.run_game(*args).returncode, 0, args)
        self.assertIn("video", self.run_game("--list-paths").stdout)

    def test_plain_full_game_and_eof(self):
        result = self.run_game("--plain", "--seed", "7", "--path", "video", input="bad\n0\n" + "1\n" * 900)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Please choose one", result.stdout)
        self.assertIn("Weeks ", result.stdout)
        self.assertIn("Losing is a story, too", result.stdout)
        self.assertNotIn("progress is not saved", result.stdout)
        self.assertNotIn("\x1b", result.stdout)
        self.assertIn("progress is not saved", self.run_game("--plain").stdout)

    def test_enter_accepts_and_proposals_can_be_overridden(self):
        result = self.run_game("--plain", "--seed", "7", "--path", "video",
                               input="4\n\n\n2\n2\nq\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Ada (AI agent)", result.stdout)
        self.assertIn("Proposed decision:", result.stdout)
        self.assertIn("You chose: Choose another option", result.stdout)
        self.assertIn("You chose: Separate payments compute and invoice storage", result.stdout)
        self.assertNotIn("Please choose one", result.stdout)
        enter = self.run_game("--plain", "--seed", "7", "--path", "video", input="\n" * 900)
        self.assertEqual(enter.returncode, 0, enter.stderr)
        self.assertIn("You chose: Continue with this decision", enter.stdout)
        self.assertIn("Weeks ", enter.stdout)
        self.assertNotIn("Please choose one", enter.stdout)

    def test_management_interrupt_and_stop(self):
        # Hire Ada, interrupt the first week decision, stop the agent, inspect finances,
        # return to the original story question, then quit.
        result = self.run_game("--plain", "--seed", "7", "--path", "video",
                               input="4\nm\n3\n1\n4\n1\nq\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Next week's subscription stops", result.stdout)
        self.assertIn("AI subscriptions $0", result.stdout)
        self.assertIn("committed payroll: $550", result.stdout)
        self.assertIn("Monday planning: What should Founder own", result.stdout)

    def test_labeled_architecture_and_game_path(self):
        # Interrupt the first story prompt using a label, switch S to Deep Archive,
        # inspect S again and ask the same event evaluator for a tabletop result.
        result = self.run_game("--plain", "--seed", "7", "--path", "video",
            input="4\nS\n4\n1\nS\n6\n1\nq\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[S] S3 Glacier Deep Archive", result.stdout)
        self.assertIn("TABLETOP EXERCISE", result.stdout)
        self.assertIn("asynchronous restore", result.stdout)
        game = self.run_game("--demo", "--path", "game", "--seed", "42")
        self.assertEqual(game.returncode, 0, game.stderr)
        self.assertIn("Brightforge", game.stdout)
        self.assertIn("persistent session service", game.stdout)
        self.assertIn("Weeks 52", game.stdout)

    def test_edit_api_and_dataset_topology(self):
        # Start at the architecture shortcut before hiring. Refine a shared
        # server, extract only Payments, and move only invoices into Aurora.
        steps = ["a", "8", "3", "A", "1", "2", "1",
                 "4", "D", "1", "1", "1",
                 "1", "U3", "2", "1",
                 "6", "1", "4", "1",
                 "2", "B3", "1", "3", "1",
                 "4", "D2", "2", "1", "1",
                 "7", "1", "8", "7", "q"]
        result = self.run_game("--plain", "--seed", "7", "--path", "game", input="\n".join(steps)+"\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[U3] Payments -> [C2]", result.stdout)
        self.assertIn("[B1 @ D] [B3 @ D2]", result.stdout)
        self.assertIn("[D2] Aurora PostgreSQL", result.stdout)
        self.assertIn("[C2] Lambda", result.stdout)
        self.assertIn("pass this week's topology rehearsal", result.stdout)
        self.assertIn("[U1] Accounts -> [A]", result.stdout)
        self.assertNotIn("Please choose one", result.stdout)

    def test_architecture_report_from_map(self):
        result = self.run_game("--plain", "--seed", "7", "--path", "video", input="a\n9\n1\n7\nq\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        for section in ["ARCHITECTURE REPORT / week 0", "COST BY NODE", "API DEPENDENCIES",
                        "IDLE CAPACITY", "READINESS", "RECENT DESIGN CHANGES"]:
            self.assertIn(section, result.stdout)
        self.assertIn("[U3] Payments: [G] -> [A]", result.stdout)
        self.assertNotIn("Please choose one", result.stdout)

    def test_external_rewrite_and_narration_assets(self):
        assets = pathlib.Path(__file__).resolve().parents[2] / "assets" / "graphrewrite"
        with tempfile.TemporaryDirectory() as directory:
            rules = pathlib.Path(directory) / "company.graphrewrite"
            programme = (assets / "company.graphrewrite").read_text().replace("archive-8-0|4800|", "archive-8-0|1234|")
            start = programme.index("rule archive-8-0:")
            end = programme.index("\n\n", start)
            programme = programme[:start] + programme[start:end].replace("storage3", "storage0") + programme[end:]
            rules.write_text(programme)
            narration = (assets / "company.narration").read_text()
            narration = narration.replace("{C} is a small streaming startup", "NARRATOR PROBE: {C} is a small streaming startup")
            rules.with_suffix(".narration").write_text(narration)
            result = subprocess.run([BINARY, "--plain", "--seed", "7", "--path", "video"],
                input="4\nS\n6\n1\nq\n", text=True, capture_output=True, timeout=10,
                env={**os.environ, "COMPANY_STORY_RULES": str(rules)})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("NARRATOR PROBE: Moonbeam", result.stdout)
            self.assertIn("Estimated incident: $1234", result.stdout)
            rules.write_text("this is not a rewrite programme")
            failed = subprocess.run([BINARY, "--demo"], text=True, capture_output=True,
                env={**os.environ, "COMPANY_STORY_RULES": str(rules)}, timeout=10)
            self.assertNotEqual(failed.returncode, 0)

    @unittest.skipIf(os.name == "nt", "POSIX terminal test")
    def test_architecture_shortcuts_and_single_header(self):
        import pty
        for shortcut in (b"\x01", b"\x1b[97;9u"):
            master, slave = pty.openpty()
            process = subprocess.Popen([BINARY, "--seed", "7", "--path", "video", "--no-animation"],
                stdin=slave, stdout=slave, stderr=slave, env={**os.environ, "TERM": "xterm"})
            output = b""
            def read_until(marker):
                nonlocal output
                deadline = time.monotonic() + 5
                while marker not in output:
                    if time.monotonic() > deadline:
                        self.fail(repr(output[-1200:]))
                    if select.select([master], [], [], .1)[0]:
                        output += os.read(master, 65536)
            try:
                read_until(b"Selected option 1")
                self.assertNotIn(b"SYSTEM MAP", output)
                os.write(master, shortcut)
                read_until(b"Choose a high-level system")
                os.write(master, b"4\r")
                read_until(b"Select an AWS component")
                self.assertIn(b"AWS DETAIL / system 4", output)
                for page in output.split(b"\x1b[2J\x1b[H")[1:]:
                    self.assertEqual(page.count(b"Moonbeam | WEEK"), 1, page)
                os.write(master, b"q")
                self.assertEqual(process.wait(timeout=5), 0)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                os.close(master)
                os.close(slave)

    @unittest.skipIf(os.name == "nt", "POSIX terminal test")
    def test_arrow_keys_and_terminal_restoration(self):
        import pty
        import termios
        for leave in (b"q", b"\x03"):
            master, slave = pty.openpty()
            before = termios.tcgetattr(slave)
            process = subprocess.Popen([BINARY, "--seed", "7", "--path", "video", "--no-animation"],
                                       stdin=slave, stdout=slave, stderr=slave,
                                       env={**os.environ, "TERM": "xterm-256color"})
            buffer = b""

            def until(marker):
                nonlocal buffer
                deadline = time.monotonic() + 5
                while marker not in buffer:
                    if time.monotonic() > deadline:
                        self.fail(f"Missing {marker!r}: {buffer[-800:]!r}")
                    if select.select([master], [], [], 0.1)[0]:
                        buffer += os.read(master, 65536)
                if marker == b"Selected option 2":
                    self.assertIn(b"\x1b[1;32m  > ", buffer)
                buffer = buffer.split(marker, 1)[1]

            try:
                until(b"Selected option 1")
                os.write(master, b"\x1b[B")
                until(b"Selected option 2")

                os.write(master, b"\r")
                until(b"You chose: ")
                until(b"\x1b[2J\x1b[H")
                until(b"Available $85000 | recurring cost $")
                until(b"customers 0")
                until(b"Selected option 1")
                os.write(master, leave)
                self.assertEqual(process.wait(timeout=5), 0)
                # macOS can set PENDIN while restoring canonical mode. It is a
                # kernel pending-input flag, not a mode changed by the application.
                after = termios.tcgetattr(slave)
                after[3] &= ~getattr(termios, "PENDIN", 0)
                before[3] &= ~getattr(termios, "PENDIN", 0)
                self.assertEqual(after, before)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                os.close(master)
                os.close(slave)

    @unittest.skipIf(os.name == "nt", "POSIX terminal test")
    def test_result_waits_before_next_page(self):
        import pty
        master, slave = pty.openpty()
        process = subprocess.Popen([BINARY, "--seed", "7", "--path", "video", "--no-animation"],
            stdin=slave, stdout=slave, stderr=slave, env={**os.environ, "TERM": "xterm-256color"})
        buffer = b""
        def until(marker):
            nonlocal buffer
            deadline = time.monotonic() + 5
            while marker not in buffer:
                if time.monotonic() > deadline:
                    self.fail(repr(buffer[-1000:]))
                if select.select([master], [], [], .1)[0]:
                    buffer += os.read(master, 65536)
            seen, buffer = buffer.split(marker, 1)
            return seen
        try:
            until(b"Selected option 1")
            os.write(master, b"4\r")
            page = until(b"When you are ready")
            self.assertIn(b"Ada joins the chat", page)
            self.assertNotIn(b"WEEK 1 /", page)
            until(b"Selected option 1")
            os.write(master, b"\r")
            until(b"WEEK 1 / 52")
            until(b"Selected option 1")
            os.write(master, b"q")
            self.assertEqual(process.wait(timeout=5), 0)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    @unittest.skipIf(os.name == "nt", "POSIX terminal test")
    def test_animation_skip_and_interrupt_restore(self):
        import pty
        import termios
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        process = subprocess.Popen([BINARY, "--seed", "42", "--path", "video"],
                                   stdin=slave, stdout=slave, stderr=slave,
                                   env={**os.environ, "TERM": "xterm-256color"})
        buffer = b""

        def until(marker):
            nonlocal buffer
            deadline = time.monotonic() + 5
            while marker not in buffer:
                if time.monotonic() > deadline:
                    self.fail(f"Missing animation marker {marker!r}: {buffer[-500:]!r}")
                if select.select([master], [], [], 0.1)[0]:
                    buffer += os.read(master, 65536)
            buffer = buffer.split(marker, 1)[1]

        try:
            until(b"small streaming startup")
            self.assertNotIn(b"Plan each Monday", buffer)
            os.write(master, b" ")  # Finish this scene without submitting a menu answer.
            until(b"Selected option 1")
            os.write(master, b"\r")
            until(b"puts a favourite film poster")
            os.write(master, b"\x03")  # Interrupt while text is being animated.
            self.assertEqual(process.wait(timeout=5), 0)
            after = termios.tcgetattr(slave)
            after[3] &= ~getattr(termios, "PENDIN", 0)
            before[3] &= ~getattr(termios, "PENDIN", 0)
            self.assertEqual(after, before)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    unittest.main()
