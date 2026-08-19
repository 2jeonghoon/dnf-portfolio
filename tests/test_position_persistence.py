"""Position autosave: ENTER and leaving the world (QUIT or disconnect) persist
x/y to the `characters` table via a fire-and-forget DB job. MOVE never does.

Since the save has no client-visible acknowledgement (no PENDING/DONE), these
tests poll the database directly instead of relying on protocol responses.
"""

from conftest import poll_until


def _character_position(db, account, character):
    with db.cursor() as cursor:
        cursor.execute(
            "SELECT c.x, c.y, c.level, c.gold FROM characters c "
            "JOIN players p ON c.player_id = p.id "
            "WHERE p.account = %s AND c.name = %s",
            (account, character),
        )
        return cursor.fetchone()


def test_enter_persists_position_and_creates_character_with_defaults(
    client, db, unique_account
):
    client.request(f"REGISTER {unique_account} BladeMaster")
    client.send(f"ENTER {unique_account} main 120 80")
    assert client.read_line() == "WORLD ENTERED main x=120 y=80 sector=1,0"

    row = poll_until(lambda: _character_position(db, unique_account, "main"))
    x, y, level, gold = row
    assert (x, y) == (120, 80)
    assert (level, gold) == (1, 0)


def test_move_does_not_persist_until_leave(client, db, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    client.send(f"ENTER {unique_account} main 0 0")
    client.read_line()
    poll_until(lambda: _character_position(db, unique_account, "main"))

    client.send("MOVE 99 99")
    assert client.read_line() == "WORLD MOVED main x=99 y=99 sector=0,0"

    x, y, _, _ = _character_position(db, unique_account, "main")
    assert (x, y) == (0, 0)


def test_quit_persists_final_position(client, db, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    client.send(f"ENTER {unique_account} main 0 0")
    client.read_line()
    poll_until(lambda: _character_position(db, unique_account, "main"))

    client.send("MOVE 99 99")
    client.read_line()

    client.send("QUIT")
    assert client.read_line() == "BYE"

    def _moved():
        row = _character_position(db, unique_account, "main")
        return row if row[:2] == (99, 99) else None

    x, y, _, _ = poll_until(_moved)
    assert (x, y) == (99, 99)


def test_disconnect_persists_final_position(make_client, db, unique_account):
    client = make_client()
    client.request(f"REGISTER {unique_account} BladeMaster")
    client.send(f"ENTER {unique_account} main 0 0")
    client.read_line()
    poll_until(lambda: _character_position(db, unique_account, "main"))

    client.send("MOVE 42 43")
    client.read_line()

    client.close()

    def _moved():
        row = _character_position(db, unique_account, "main")
        return row if row[:2] == (42, 43) else None

    x, y, _, _ = poll_until(_moved)
    assert (x, y) == (42, 43)
