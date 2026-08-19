def test_login_after_register_succeeds(client, unique_account):
    client.request(f"REGISTER {unique_account} BladeMaster")
    _, done = client.request(f"LOGIN {unique_account}")
    assert done.startswith("DONE ")
    assert f"account={unique_account}" in done
    assert "display_name=BladeMaster" in done
    assert "player_id=" in done


def test_login_unknown_account_fails(client, unique_account):
    _, done = client.request(f"LOGIN {unique_account}")
    assert done.startswith("FAIL ")
    assert "unknown account" in done


def test_login_wrong_arg_count_is_rejected_synchronously(client):
    client.send("LOGIN")
    response = client.read_line()
    assert response == "ERR usage: LOGIN <account>"
