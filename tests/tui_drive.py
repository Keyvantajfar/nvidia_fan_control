#!/usr/bin/env python3
"""Drive the curses TUI in a pseudo-terminal with scripted keys (used by tests).

    tui_drive.py <config> <keys...>

Keys are literal characters or names: UP DOWN LEFT RIGHT ENTER ESC TAB BS.
Prints the TUI's final output and exits with its exit status."""
import fcntl, os, pty, select, struct, sys, termios, time

NAMES = {"UP": "\x1bOA", "DOWN": "\x1bOB", "RIGHT": "\x1bOC", "LEFT": "\x1bOD",
         "ENTER": "\r", "ESC": "\x1b", "TAB": "\t", "BS": "\x7f"}


def main() -> int:
    config, keys = sys.argv[1], sys.argv[2:]
    here = os.path.dirname(os.path.abspath(__file__))
    tui = os.path.join(here, "..", "tui", "nvfc_tui.py")
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(TERM="xterm", NVFC_STATUS_PATH="/nonexistent/status")
        os.execvp(sys.executable, [sys.executable, tui, "--config", config])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))
    out = b""

    def drain(wait: float) -> None:
        nonlocal out
        end = time.time() + wait
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    return
                if not chunk:
                    return
                out += chunk

    drain(1.0)
    for key in keys:
        os.write(fd, NAMES.get(key, key).encode())
        drain(0.4 if key in ("ESC",) else 0.25)
    drain(0.8)
    _, status = os.waitpid(pid, 0)
    sys.stdout.write(out.decode("utf-8", "replace")[-400:])
    return os.waitstatus_to_exitcode(status)


if __name__ == "__main__":
    raise SystemExit(main())
