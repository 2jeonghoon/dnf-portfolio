def test_unknown_command_is_rejected(client):
    client.send("TELEPORT 1 2")
    assert client.read_line() == "ERR unknown command: TELEPORT"


def test_empty_line_is_rejected(client):
    client.send("")
    assert client.read_line() == "ERR empty command"


def test_quit_with_extra_args_is_rejected(client):
    client.send("QUIT now")
    assert client.read_line() == "ERR usage: QUIT"


def test_pipelined_commands_are_both_accepted_and_completed(client, unique_account):
    # Both lines are parsed off one recv() before either DB job completes, so the
    # two PENDING acks arrive back-to-back, followed by the two completions. The
    # two REGISTERs target different accounts so the DB worker pool is free to
    # run them concurrently without one depending on the other's result.
    account_a = f"{unique_account}_a"
    account_b = f"{unique_account}_b"
    client._sock.sendall(
        f"REGISTER {account_a} PlayerA\nREGISTER {account_b} PlayerB\n".encode("utf-8")
    )

    first_pending = client.read_line()
    assert first_pending.startswith("PENDING ")
    assert first_pending.endswith("REGISTER")
    first_id = first_pending.split()[1]

    second_pending = client.read_line()
    assert second_pending.startswith("PENDING ")
    assert second_pending.endswith("REGISTER")
    second_id = second_pending.split()[1]
    assert first_id != second_id

    completions = {}
    for _ in range(2):
        line = client.read_line()
        request_id = line.split()[1]
        completions[request_id] = line

    assert f"OK REGISTER account={account_a} display_name=PlayerA" in completions[first_id]
    assert f"OK REGISTER account={account_b} display_name=PlayerB" in completions[second_id]
