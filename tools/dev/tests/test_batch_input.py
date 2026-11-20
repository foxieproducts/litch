from __future__ import annotations

import os
import sys
import unittest
from typing import TextIO
from unittest import mock

from tools.dev import batch_input
from tools.dev.batch_input import BatchInput

if sys.platform != "win32":
    import pty
    import select
    import termios
    import tty


@unittest.skipIf(sys.platform == "win32", "POSIX terminal and pipe behavior")
class PosixBatchInputTests(unittest.TestCase):
    def terminal(self) -> tuple[int, TextIO]:
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        stream = os.fdopen(slave)
        self.addCleanup(stream.close)
        return master, stream

    def assert_terminal_mode(self, fd: int, original: list) -> None:
        # Darwin marks input pending after returning to canonical mode until
        # the next readiness check processes that kernel-owned state.
        select.select([fd], [], [], 0)
        self.assertEqual(termios.tcgetattr(fd), original)

    def test_terminal_quits_without_enter_and_restores_mode(self) -> None:
        master, stream = self.terminal()
        fd = stream.fileno()
        original = termios.tcgetattr(fd)

        with BatchInput(stream) as operator:
            flags = termios.tcgetattr(fd)[3]
            self.assertFalse(flags & (termios.ICANON | termios.ECHO))
            self.assertTrue(flags & termios.ISIG)
            self.assertFalse(operator.quit_requested())
            os.write(master, b"xq")
            self.assertTrue(operator.quit_requested())
            self.assertTrue(operator.quit_requested())

        self.assert_terminal_mode(fd, original)

    def test_terminal_restores_on_failure_and_keyboard_interrupt(self) -> None:
        _, stream = self.terminal()
        fd = stream.fileno()
        original = termios.tcgetattr(fd)
        for error in (RuntimeError, KeyboardInterrupt):
            with self.subTest(error=error), self.assertRaises(error), BatchInput(stream):
                raise error()
            self.assert_terminal_mode(fd, original)

    def test_terminal_restores_when_mode_change_is_interrupted(self) -> None:
        _, stream = self.terminal()
        fd = stream.fileno()
        original = termios.tcgetattr(fd)
        setcbreak = tty.setcbreak

        def change_then_interrupt(fd: int, when: int) -> None:
            setcbreak(fd, when)
            raise KeyboardInterrupt

        with mock.patch.object(tty, "setcbreak", side_effect=change_then_interrupt):
            with self.assertRaises(KeyboardInterrupt), BatchInput(stream):
                self.fail("interrupted context must not enter")
        self.assert_terminal_mode(fd, original)

    def test_confirmation_discards_stale_y_and_survives_quit_polling(self) -> None:
        master, stream = self.terminal()
        with BatchInput(stream) as operator:
            os.write(master, b"yY")
            operator.begin_confirmation()
            self.assertFalse(operator.confirmation_requested())

            for pasted in (b"python3\n", "éy".encode()):
                with self.subTest(pasted=pasted):
                    os.write(master, pasted)
                    self.assertFalse(operator.quit_requested())
                    self.assertFalse(operator.confirmation_requested())

            os.write(master, b"Y\r\n")
            self.assertFalse(operator.quit_requested())
            self.assertFalse(operator.quit_requested())
            self.assertTrue(operator.confirmation_requested())
            os.write(master, b"y")
            self.assertFalse(operator.confirmation_requested())

    def test_new_confirmation_does_not_discard_queued_quit(self) -> None:
        master, stream = self.terminal()
        with BatchInput(stream) as operator:
            os.write(master, b"YQ")
            operator.begin_confirmation()
            self.assertFalse(operator.confirmation_requested())
            self.assertTrue(operator.quit_requested())

    def test_queued_quit_wins_over_pending_confirmation(self) -> None:
        master, stream = self.terminal()
        with BatchInput(stream) as operator:
            operator.begin_confirmation()
            os.write(master, b"y")
            self.assertFalse(operator.quit_requested())
            os.write(master, b"q")
            self.assertFalse(operator.confirmation_requested())
            self.assertTrue(operator.quit_requested())

    def test_disarming_retires_the_pending_confirmation(self) -> None:
        master, stream = self.terminal()
        with BatchInput(stream) as operator:
            operator.begin_confirmation()
            os.write(master, b"y")
            self.assertFalse(operator.quit_requested())
            operator.end_confirmation()
            self.assertFalse(operator.confirmation_requested())

            operator.begin_confirmation()
            self.assertFalse(operator.confirmation_requested())
            os.write(master, b"y")
            self.assertTrue(operator.confirmation_requested())

    def test_pipe_buffers_uppercase_quit_and_retires_eof(self) -> None:
        for keys in (b"xQ", b"x"):
            with self.subTest(keys=keys):
                read_fd, write_fd = os.pipe()
                with os.fdopen(read_fd) as stream, BatchInput(stream) as operator:
                    with os.fdopen(write_fd, "wb") as writer:
                        self.assertFalse(operator.quit_requested())
                        writer.write(keys)
                    expected = b"Q" in keys
                    self.assertEqual(operator.quit_requested(), expected)
                    self.assertEqual(operator.quit_requested(), expected)
                    with mock.patch("tools.dev.batch_input.select.select") as select:
                        self.assertEqual(operator.quit_requested(), expected)
                        select.assert_not_called()


class WindowsBatchInputTests(unittest.TestCase):
    def test_console_latches_quit_and_keeps_reads_nonblocking(self) -> None:
        stream = mock.Mock()
        stream.fileno.return_value = 0
        console = mock.Mock()
        console.kbhit.side_effect = (False, True, False, True, True)
        console.getwch.side_effect = ("\xe0", "q", "Q")
        with (
            mock.patch.object(sys, "platform", "win32"),
            mock.patch.object(os, "isatty", return_value=True),
            mock.patch.object(batch_input, "msvcrt", console, create=True),
            BatchInput(stream) as operator,
        ):
            self.assertFalse(operator.quit_requested())
            console.getwch.assert_not_called()
            self.assertFalse(operator.quit_requested())
            self.assertTrue(operator.quit_requested())
            calls = console.kbhit.call_count
            self.assertTrue(operator.quit_requested())
            self.assertEqual(console.kbhit.call_count, calls)

    def test_redirected_stdin_does_not_poll_on_windows(self) -> None:
        stream = mock.Mock()
        stream.fileno.return_value = 0
        with (
            mock.patch.object(sys, "platform", "win32"),
            mock.patch.object(os, "isatty", return_value=False),
            mock.patch.object(batch_input, "msvcrt", create=True) as console,
            mock.patch.object(batch_input, "select", create=True) as select,
            BatchInput(stream) as operator,
        ):
            self.assertFalse(operator.quit_requested())
            console.kbhit.assert_not_called()
            select.select.assert_not_called()

    def test_extended_key_scan_code_cannot_confirm_a_windows_prompt(self) -> None:
        stream = mock.Mock()
        stream.fileno.return_value = 0
        console = mock.Mock()
        keys = ["Y", "\xe0"]
        console.kbhit.side_effect = lambda: bool(keys)
        console.getwch.side_effect = lambda: keys.pop(0)
        with (
            mock.patch.object(sys, "platform", "win32"),
            mock.patch.object(os, "isatty", return_value=True),
            mock.patch.object(batch_input, "msvcrt", console, create=True),
            BatchInput(stream) as operator,
        ):
            operator.begin_confirmation()
            keys.append("Y")  # Scan code for the buffered prefix, not a keypress.
            self.assertFalse(operator.confirmation_requested())
            keys.extend("python3\r")
            self.assertFalse(operator.confirmation_requested())
            keys.extend("y\r")
            self.assertFalse(operator.quit_requested())
            self.assertTrue(operator.confirmation_requested())
