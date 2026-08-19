import pytest

from protocol_client import ProtocolError


def test_move_before_enter_is_rejected(client):
    client.send("MOVE 10 10")
    response = client.read_line()
    assert response == "ERR not in world; use ENTER first"


def test_attack_before_enter_is_rejected(client):
    client.send("ATTACK slash goblin_01")
    response = client.read_line()
    assert response == "ERR not in world; use ENTER first"


def test_enter_move_attack_single_client(client, unique_account):
    client.send(f"ENTER {unique_account} Hero 120 80")
    assert client.read_line() == "WORLD ENTERED Hero x=120 y=80 sector=1,0"

    client.send("MOVE 180 90")
    assert client.read_line() == "WORLD MOVED Hero x=180 y=90 sector=1,0"

    client.send("ATTACK slash goblin_01")
    assert client.read_line() == "WORLD ATTACK skill=slash target=goblin_01"


def test_quit_sends_bye_and_closes_connection(client, unique_account):
    client.send(f"ENTER {unique_account} Hero 0 0")
    client.read_line()

    client.send("QUIT")
    assert client.read_line() == "BYE"

    with pytest.raises(ProtocolError, match="connection closed"):
        client.read_line()


def test_enter_broadcasts_to_players_in_the_same_sector(make_client, unique_account):
    watcher = make_client()
    watcher.send(f"ENTER {unique_account}_a Hero1 0 0")
    assert watcher.read_line() == "WORLD ENTERED Hero1 x=0 y=0 sector=0,0"

    newcomer = make_client()
    newcomer.send(f"ENTER {unique_account}_b Hero2 10 10")
    assert newcomer.read_line() == "WORLD ENTERED Hero2 x=10 y=10 sector=0,0"
    assert (
        newcomer.read_line()
        == f"EVT SNAPSHOT Hero1 account={unique_account}_a x=0 y=0 sector=0,0"
    )

    assert (
        watcher.read_line()
        == f"EVT ENTER Hero2 account={unique_account}_b x=10 y=10 sector=0,0"
    )


def test_enter_does_not_broadcast_to_players_in_a_distant_sector(make_client, unique_account):
    watcher = make_client()
    watcher.send(f"ENTER {unique_account}_a Hero1 0 0")
    assert watcher.read_line() == "WORLD ENTERED Hero1 x=0 y=0 sector=0,0"

    far_away = make_client()
    far_away.send(f"ENTER {unique_account}_b Hero2 1000 1000")
    assert far_away.read_line() == "WORLD ENTERED Hero2 x=1000 y=1000 sector=10,10"

    watcher.expect_no_message()


def test_move_broadcasts_to_watcher_in_range(make_client, unique_account):
    watcher = make_client()
    watcher.send(f"ENTER {unique_account}_a Hero1 0 0")
    watcher.read_line()

    mover = make_client()
    mover.send(f"ENTER {unique_account}_b Hero2 10 10")
    mover.read_line()  # WORLD ENTERED
    mover.read_line()  # EVT SNAPSHOT of Hero1, already covered elsewhere
    watcher.read_line()  # EVT ENTER for Hero2, already covered elsewhere

    mover.send("MOVE 20 20")
    assert mover.read_line() == "WORLD MOVED Hero2 x=20 y=20 sector=0,0"
    assert watcher.read_line() == "EVT MOVE Hero2 x=20 y=20 sector=0,0"


def test_attack_broadcasts_only_to_watchers_in_range(make_client, unique_account):
    watcher = make_client()
    watcher.send(f"ENTER {unique_account}_a Hero1 0 0")
    watcher.read_line()

    far_away = make_client()
    far_away.send(f"ENTER {unique_account}_c HeroFar 1000 1000")
    far_away.read_line()

    attacker = make_client()
    attacker.send(f"ENTER {unique_account}_b Hero2 10 10")
    attacker.read_line()  # WORLD ENTERED
    attacker.read_line()  # EVT SNAPSHOT of Hero1, already covered elsewhere
    watcher.read_line()  # EVT ENTER for Hero2

    attacker.send("ATTACK slash goblin_01")
    assert attacker.read_line() == "WORLD ATTACK skill=slash target=goblin_01"
    assert (
        watcher.read_line()
        == "EVT ATTACK attacker=Hero2 skill=slash target=goblin_01 x=10 y=10"
    )
    far_away.expect_no_message()


def test_leave_broadcasts_despawn_to_watcher_in_range(make_client, unique_account):
    watcher = make_client()
    watcher.send(f"ENTER {unique_account}_a Hero1 0 0")
    watcher.read_line()

    leaver = make_client()
    leaver.send(f"ENTER {unique_account}_b Hero2 10 10")
    leaver.read_line()  # WORLD ENTERED
    leaver.read_line()  # EVT SNAPSHOT of Hero1, already covered elsewhere
    watcher.read_line()  # EVT ENTER for Hero2

    leaver.send("QUIT")
    assert leaver.read_line() == "BYE"

    assert watcher.read_line() == "EVT DESPAWN Hero2 reason=leave sector=0,0"
