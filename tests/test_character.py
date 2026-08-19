def test_save_requires_registered_account(client, unique_account):
    _, done = client.request(f"SAVE {unique_account} main 1 0")
    assert done.startswith("FAIL ")
    assert "unknown account" in done


def test_save_then_load_round_trip(client, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    _, save_done = client.request(f"SAVE {unique_account} main 110 3000000")
    assert save_done.startswith("DONE ")
    assert save_done.endswith(
        f"OK SAVE account={unique_account} character=main level=110 gold=3000000"
    )

    _, load_done = client.request(f"LOAD {unique_account}")
    assert load_done.startswith("DONE ")
    assert f"OK LOAD account={unique_account} character_count=1" in load_done
    assert "characters=main:110:3000000:0:0" in load_done


def test_load_for_account_with_no_characters_returns_zero_count(client, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    _, load_done = client.request(f"LOAD {unique_account}")
    assert load_done.startswith("DONE ")
    assert load_done.endswith(f"OK LOAD account={unique_account} character_count=0")
    assert "characters=" not in load_done


def test_load_for_never_registered_account_fails(client, unique_account):
    _, load_done = client.request(f"LOAD {unique_account}")
    assert load_done.startswith("FAIL ")
    assert "unknown account" in load_done


def test_save_same_character_twice_upserts(client, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    client.request(f"SAVE {unique_account} main 1 0")
    client.request(f"SAVE {unique_account} main 50 1234")

    _, load_done = client.request(f"LOAD {unique_account}")
    assert f"OK LOAD account={unique_account} character_count=1" in load_done
    assert "characters=main:50:1234:0:0" in load_done


def test_save_rejects_non_numeric_level_and_gold(client, unique_account):
    client.send(f"SAVE {unique_account} main abc 0")
    response = client.read_line()
    assert response == "ERR level and gold must be unsigned integers"


def test_save_wrong_arg_count_is_rejected_synchronously(client):
    client.send("SAVE onlyaccount")
    response = client.read_line()
    assert response == "ERR usage: SAVE <account> <character> <level> <gold>"
