#!/usr/bin/env python3
"""Run a command under a real pseudo-terminal and answer a password prompt.

Usage:
    pty-run.py <prompt-substring> <password> -- <cmd> [args...]

Spawns <cmd> via pty.fork(), so the child becomes a session leader with a
fresh pty as its *controlling terminal* (login_tty: setsid + TIOCSCTTY +
slave dup'd to fd 0/1/2). All pty output is relayed to this process's own
stdout (so a launching test can capture it). The first time that output
contains <prompt-substring>, this driver writes "<password>\\n" into the
pty — exactly as if a user typed the password at the terminal.

It exists to test the kmux --rsh + ssh path the FIFO-based
test-rsh-interactive.sh deliberately does not: that an ssh-style wrapper's
prompt actually reaches the controlling terminal, and that the password
typed there is delivered back to the wrapper. ssh reads/writes /dev/tty
directly (not stdin), which is why a real pty — not a pipe — is required.

The driver keeps relaying until the child exits or it is killed by the
launching test once that test's own assertions (tmux came up, etc.) pass.
"""

import os
import pty
import select
import sys


def split_args(argv):
    if "--" in argv:
        i = argv.index("--")
        return argv[:i], argv[i + 1:]
    return argv[:2], argv[2:]


def main():
    head, cmd = split_args(sys.argv[1:])
    if len(head) < 2 or not cmd:
        sys.stderr.write("pty-run: usage: pty-run.py <prompt> <password> -- <cmd> [args...]\n")
        return 2
    prompt = head[0].encode()
    password = head[1].encode()

    pid, master = pty.fork()
    if pid == 0:
        # Child: pty.fork() already ran login_tty(); just exec the command.
        try:
            os.execvp(cmd[0], cmd)
        except OSError as exc:
            sys.stderr.write("pty-run: exec %r failed: %s\n" % (cmd[0], exc))
            os._exit(127)

    sent = False
    seen = b""
    out = sys.stdout.buffer
    while True:
        try:
            ready, _, _ = select.select([master], [], [], 1.0)
        except (OSError, ValueError):
            break
        if master in ready:
            try:
                data = os.read(master, 4096)
            except OSError:
                data = b""
            if not data:
                break  # EOF — child closed the pty (exited)
            out.write(data)
            out.flush()
            if not sent:
                # Keep only enough tail to span a prompt split across reads.
                seen = (seen + data)[-4096:]
                if prompt in seen:
                    os.write(master, password + b"\n")
                    sent = True

    try:
        _, status = os.waitpid(pid, 0)
    except ChildProcessError:
        return 0
    if os.WIFEXITED(status):
        return os.WEXITSTATUS(status)
    if os.WIFSIGNALED(status):
        return 128 + os.WTERMSIG(status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
