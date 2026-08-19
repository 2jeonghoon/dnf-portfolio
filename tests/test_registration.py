def test_hello_banner_on_connect(client):
    assert client.hello.startswith("HELLO commands=")
    assert "REGISTER" in client.hello


def test_register_success(client, unique_account):
    _, done = client.request(f"REGISTER {unique_account} BladeMaster")
    assert done.endswith(
        f"OK REGISTER account={unique_account} display_name=BladeMaster"
    )
    assert done.startswith("DONE ")


def test_register_fails_on_duplicate_account(client, unique_account):
    client.request(f"REGISTER {unique_account} FirstName")
    _, done = client.request(f"REGISTER {unique_account} SecondName")
    assert done.startswith("FAIL ")
    assert "already exists" in done

    # confirm the original registration was left untouched
    _, login_done = client.request(f"LOGIN {unique_account}")
    assert "display_name=FirstName" in login_done


def test_register_wrong_arg_count_is_rejected_synchronously(client):
    client.send("REGISTER onlyaccount")
    response = client.read_line()
    assert response == "ERR usage: REGISTER <account> <display_name>"
