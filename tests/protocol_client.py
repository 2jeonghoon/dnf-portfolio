"""Socket client for the dungeon_portfolio_server line protocol."""

from __future__ import annotations

import socket


class ProtocolError(AssertionError):
    pass


class ProtocolClient:
    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._buffer = b""
        self._timeout = timeout
        self.hello = self.read_line()

    def send(self, line: str) -> None:
        self._sock.sendall((line + "\n").encode("utf-8"))

    def read_line(self) -> str:
        while b"\n" not in self._buffer:
            try:
                chunk = self._sock.recv(4096)
            except socket.timeout as exc:
                raise ProtocolError("timed out waiting for a line") from exc
            if not chunk:
                raise ProtocolError("connection closed before a full line was received")
            self._buffer += chunk
        line, self._buffer = self._buffer.split(b"\n", 1)
        return line.decode("utf-8").rstrip("\r")

    def request(self, line: str) -> tuple[str, str]:
        """Send a DB-backed command and return (PENDING line, DONE/FAIL line)."""
        self.send(line)
        pending = self.read_line()
        if not pending.startswith("PENDING "):
            raise ProtocolError(f"expected PENDING line, got: {pending!r}")
        request_id = pending.split()[1]

        completion = self.read_line()
        if not (
            completion.startswith(f"DONE {request_id} ")
            or completion.startswith(f"FAIL {request_id} ")
        ):
            raise ProtocolError(
                f"expected DONE/FAIL {request_id} line, got: {completion!r}"
            )
        return pending, completion

    def expect_no_message(self, timeout: float = 0.3) -> None:
        if b"\n" in self._buffer:
            raise ProtocolError(f"expected no message, buffer has: {self._buffer!r}")

        self._sock.settimeout(timeout)
        try:
            chunk = self._sock.recv(4096)
        except socket.timeout:
            return
        finally:
            self._sock.settimeout(self._timeout)

        if chunk:
            self._buffer += chunk
            raise ProtocolError(f"expected no message, got: {self._buffer!r}")

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass
