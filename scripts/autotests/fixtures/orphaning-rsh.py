#!/usr/bin/env python3
"""Proxy an rsh command while leaving its child alive if the proxy is killed."""

import os
import signal
import socket
import subprocess
import sys
import threading


def write_all(fd: int, data: bytes) -> None:
    while data:
        written = os.write(fd, data)
        data = data[written:]


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} CHILDREN_FILE COMMAND [ARG ...]", file=sys.stderr)
        return 2

    children_file = sys.argv[1]
    command = sys.argv[2:]
    proxy_socket, remote_socket = socket.socketpair()

    # The keeper retains the proxy end when this process is SIGKILLed. That
    # leaves the command's stdin/stdout connected but unread, matching a remote
    # tmux client orphaned after its local ssh transport disappears.
    keeper_pid = os.fork()
    if keeper_pid == 0:
        remote_socket.close()
        while True:
            signal.pause()

    process = subprocess.Popen(
        command,
        stdin=remote_socket.fileno(),
        stdout=remote_socket.fileno(),
        close_fds=True,
    )
    remote_socket.close()

    with open(children_file, "a", encoding="utf-8") as children:
        children.write(f"{keeper_pid}\n{process.pid}\n")

    def forward_input() -> None:
        try:
            while data := os.read(sys.stdin.fileno(), 65536):
                proxy_socket.sendall(data)
        except (BrokenPipeError, OSError):
            pass

    threading.Thread(target=forward_input, daemon=True).start()

    try:
        while data := proxy_socket.recv(65536):
            write_all(sys.stdout.fileno(), data)
    except (BrokenPipeError, OSError):
        pass
    finally:
        proxy_socket.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        try:
            os.kill(keeper_pid, signal.SIGTERM)
        except ProcessLookupError:
            pass

    return process.returncode or 0


if __name__ == "__main__":
    raise SystemExit(main())
