#!/usr/bin/env python3
"""Relay a command with delayed stdout, preserving stream order and contents."""

import collections
import os
import selectors
import subprocess
import sys
import time


def write_all(fd: int, data: bytes) -> None:
    while data:
        written = os.write(fd, data)
        data = data[written:]


def main() -> int:
    delay = float(sys.argv[1])
    process = subprocess.Popen(sys.argv[2:], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    pending = collections.deque()
    selector = selectors.DefaultSelector()
    selector.register(sys.stdin, selectors.EVENT_READ)
    selector.register(process.stdout, selectors.EVENT_READ)
    output_open = True
    try:
        while output_open or pending:
            timeout = max(0, pending[0][0] - time.monotonic()) if pending else None
            for key, _ in selector.select(timeout):
                data = os.read(key.fd, 65536)
                if key.fileobj is sys.stdin:
                    if data:
                        write_all(process.stdin.fileno(), data)
                    else:
                        selector.unregister(sys.stdin)
                        process.stdin.close()
                elif data:
                    pending.append((time.monotonic() + delay, data))
                else:
                    selector.unregister(process.stdout)
                    output_open = False
            while pending and pending[0][0] <= time.monotonic():
                write_all(sys.stdout.fileno(), pending.popleft()[1])
    finally:
        selector.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
        process.wait()
    return process.returncode


if __name__ == "__main__":
    raise SystemExit(main())
