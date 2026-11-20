"""Read batch operator keys at safe points between board writes."""

from __future__ import annotations

import os
import sys
from typing import TextIO

if sys.platform == "win32":
    import msvcrt
else:
    import select
    import termios
    import tty

_MAX_READS_PER_POLL = 4096 if sys.platform == "win32" else 64


class BatchInput:
    """Poll for quit and confirmation. Windows key input needs a console."""

    def __init__(self, stream: TextIO | None = None) -> None:
        self._stream = stream
        self._fd: int | None = None
        self._terminal_mode: tuple[int, list] | None = None
        self._quit = False
        self._confirmation_armed = False
        self._discarding_confirmations = False
        self._confirmation_yes = False
        self._confirmation_input = ""
        self._windows_extended_key = False

    def __enter__(self) -> BatchInput:
        self._quit = False
        self.end_confirmation()
        self._windows_extended_key = False
        stream = sys.stdin if self._stream is None else self._stream
        if stream is None:
            return self
        try:
            fd = stream.fileno()
        except (OSError, ValueError):
            return self

        terminal = os.isatty(fd)
        if sys.platform == "win32":
            if terminal:
                self._fd = fd
            return self

        if terminal:
            self._terminal_mode = (fd, termios.tcgetattr(fd))
            try:
                # Keep terminal signals enabled and preserve already typed input.
                tty.setcbreak(fd, termios.TCSANOW)
            except BaseException:
                self.__exit__()
                raise
        self._fd = fd
        return self

    def __exit__(self, *_exc: object) -> None:
        self._fd = None
        self.end_confirmation()
        if self._terminal_mode is not None:
            fd, mode = self._terminal_mode
            self._terminal_mode = None
            termios.tcsetattr(fd, termios.TCSANOW, mode)

    def quit_requested(self) -> bool:
        self._poll_keys()
        return self._quit

    def begin_confirmation(self) -> None:
        """Start a new prompt and ignore Y keys queued before it."""
        self._confirmation_armed = True
        self._confirmation_yes = False
        self._confirmation_input = ""
        self._discarding_confirmations = True
        self._poll_keys()

    def confirmation_requested(self) -> bool:
        """Consume one fresh Y only after checking queued input for quit."""
        if not self._poll_keys() or not self._confirmation_yes:
            return False
        self.end_confirmation()
        return True

    def end_confirmation(self) -> None:
        self._confirmation_armed = False
        self._confirmation_yes = False
        self._confirmation_input = ""
        self._discarding_confirmations = False

    def _poll_keys(self) -> bool:
        """Report a clear input queue. Keep draining on the next bounded poll."""
        if self._quit:
            return False
        for _ in range(_MAX_READS_PER_POLL):
            try:
                keys = self._read_keys(self._fd) if self._fd is not None else None
            except BlockingIOError:
                keys = None
            except (OSError, ValueError):
                self._fd = None
                keys = None
            if keys is None:
                self._discarding_confirmations = False
                if self._confirmation_input:
                    self._confirmation_yes = self._confirmation_input in ("y", "Y")
                    self._confirmation_input = ""
                return True
            if "q" in keys or "Q" in keys:
                self._quit = True
                return False
            if self._confirmation_armed and not self._discarding_confirmations:
                # Retain only enough input to distinguish one Y from a word
                # or paste, even if it arrives across several reads.
                self._confirmation_input = (
                    self._confirmation_input + keys.strip(" \t\r\n")
                )[:2]
        return False

    def _read_keys(self, fd: int) -> str | None:
        if sys.platform == "win32":
            if not msvcrt.kbhit():
                return None
            key = msvcrt.getwch()
            if self._windows_extended_key:
                self._windows_extended_key = False
                return ""
            if key in ("\x00", "\xe0"):
                # Special keys use a prefix followed by a scan code.
                self._windows_extended_key = True
                return ""
            return key

        readable, _, _ = select.select([fd], [], [], 0)
        if not readable:
            return None
        keys = os.read(fd, 4096)
        if not keys:
            # Pipes and files remain readable at EOF. Stop polling them.
            self._fd = None
            return None
        return keys.decode("latin-1")
