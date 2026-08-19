import os
import socket
import subprocess
import time
import uuid
from pathlib import Path

import pymysql
import pytest

from protocol_client import ProtocolClient

REPO_ROOT = Path(__file__).resolve().parent.parent
SERVER_BINARY = REPO_ROOT / "build" / "dungeon_portfolio_server"
BUILD_DIR = REPO_ROOT / "build"

DB_HOST = os.environ.get("TEST_MYSQL_HOST", "127.0.0.1")
DB_PORT = int(os.environ.get("TEST_MYSQL_PORT", "3306"))
DB_USER = os.environ.get("TEST_MYSQL_USER", "portfolio_test")
DB_PASSWORD = os.environ.get("TEST_MYSQL_PASSWORD", "portfolio_test")
DB_NAME = os.environ.get("TEST_MYSQL_DATABASE", "dungeon_portfolio_test")

SERVER_HOST = "127.0.0.1"
SERVER_PORT = int(os.environ.get("TEST_SERVER_PORT", "19090"))


def _wait_for_port(host: str, port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise RuntimeError(
        f"server did not start listening on {host}:{port} within {timeout}s"
    ) from last_error


@pytest.fixture(scope="session")
def mysql_test_db() -> None:
    """Truncate the test schema once per test session for a clean slate."""
    connection = pymysql.connect(
        host=DB_HOST, port=DB_PORT, user=DB_USER, password=DB_PASSWORD, database=DB_NAME
    )
    try:
        with connection.cursor() as cursor:
            cursor.execute("SET FOREIGN_KEY_CHECKS=0")
            for table in ("inventory_items", "characters", "players"):
                cursor.execute(f"TRUNCATE TABLE {table}")
            cursor.execute("SET FOREIGN_KEY_CHECKS=1")
        connection.commit()
    finally:
        connection.close()


@pytest.fixture(scope="session")
def server(mysql_test_db):
    if not SERVER_BINARY.exists():
        result = subprocess.run(["cmake", "--build", str(BUILD_DIR)], cwd=REPO_ROOT)
        if result.returncode != 0 or not SERVER_BINARY.exists():
            raise RuntimeError("failed to build dungeon_portfolio_server; build it manually first")

    env = os.environ.copy()
    env.update(
        {
            "MYSQL_HOST": DB_HOST,
            "MYSQL_PORT": str(DB_PORT),
            "MYSQL_USER": DB_USER,
            "MYSQL_PASSWORD": DB_PASSWORD,
            "MYSQL_DATABASE": DB_NAME,
            "MYSQL_POOL_SIZE": "4",
            "SERVER_PORT": str(SERVER_PORT),
        }
    )

    process = subprocess.Popen(
        [str(SERVER_BINARY)],
        cwd=REPO_ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        _wait_for_port(SERVER_HOST, SERVER_PORT)
    except RuntimeError:
        process.terminate()
        output = process.stdout.read() if process.stdout else ""
        process.wait(timeout=5)
        raise RuntimeError(f"server failed to start:\n{output}")

    yield {"host": SERVER_HOST, "port": SERVER_PORT}

    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


@pytest.fixture
def make_client(server):
    clients: list[ProtocolClient] = []

    def _make_client() -> ProtocolClient:
        client = ProtocolClient(server["host"], server["port"])
        clients.append(client)
        return client

    yield _make_client

    for client in clients:
        client.close()


@pytest.fixture
def client(make_client) -> ProtocolClient:
    return make_client()


@pytest.fixture
def unique_account() -> str:
    return f"acct_{uuid.uuid4().hex[:12]}"
