def test_save_item_requires_registered_account(client, unique_account):
    _, done = client.request(f"SAVE_ITEM {unique_account} 0 1002001 3")
    assert done.startswith("FAIL ")
    assert "unknown account" in done


def test_save_item_then_load_inventory_round_trip(client, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    _, save_done = client.request(f"SAVE_ITEM {unique_account} 0 1002001 3")
    assert save_done.startswith("DONE ")
    assert save_done.endswith(
        f"OK SAVE_ITEM account={unique_account} slot=0 item_id=1002001 count=3"
    )

    _, load_done = client.request(f"LOAD_INVENTORY {unique_account}")
    assert f"OK LOAD_INVENTORY account={unique_account} item_count=1" in load_done
    assert "items=0:1002001:3" in load_done


def test_load_inventory_for_account_with_no_items_returns_zero_count(client, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    _, load_done = client.request(f"LOAD_INVENTORY {unique_account}")
    assert load_done.endswith(
        f"OK LOAD_INVENTORY account={unique_account} item_count=0"
    )
    assert "items=" not in load_done


def test_load_inventory_for_never_registered_account_fails(client, unique_account):
    _, load_done = client.request(f"LOAD_INVENTORY {unique_account}")
    assert load_done.startswith("FAIL ")
    assert "unknown account" in load_done


def test_save_item_same_slot_overwrites(client, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    client.request(f"SAVE_ITEM {unique_account} 0 1002001 3")
    client.request(f"SAVE_ITEM {unique_account} 0 5005005 9")

    _, load_done = client.request(f"LOAD_INVENTORY {unique_account}")
    assert "OK LOAD_INVENTORY" in load_done
    assert "item_count=1" in load_done
    assert "items=0:5005005:9" in load_done


def test_save_item_with_zero_count_removes_the_item(client, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    client.request(f"SAVE_ITEM {unique_account} 0 1002001 3")
    _, save_done = client.request(f"SAVE_ITEM {unique_account} 0 1002001 0")
    assert save_done.startswith("DONE ")

    _, load_done = client.request(f"LOAD_INVENTORY {unique_account}")
    assert load_done.endswith(
        f"OK LOAD_INVENTORY account={unique_account} item_count=0"
    )


def test_save_item_rejects_non_numeric_args(client, unique_account):
    client.send(f"SAVE_ITEM {unique_account} abc 1 1")
    response = client.read_line()
    assert response == "ERR slot, item_id, and count must be unsigned integers"


def test_save_item_wrong_arg_count_is_rejected_synchronously(client):
    client.send("SAVE_ITEM onlyaccount 0")
    response = client.read_line()
    assert response == "ERR usage: SAVE_ITEM <account> <slot> <item_id> <count>"
