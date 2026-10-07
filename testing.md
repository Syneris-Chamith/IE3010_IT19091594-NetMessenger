# NetMessenger Testing Guide

This guide shows how to start the server with the test token `secret123`,
connect five clients, and exercise the chat, room, file-transfer, and reconnect
features.

## 1. Build the server and client

Open a terminal in the project directory and build the programs:

```sh
cd /home/chamith/Documents/IE3010_IT19091594-NetMessenger
make -f Makefile_1594
```

## 2. Start the server

In the first terminal, start the server with token authentication enabled:

```sh
NETMSG_TOKEN='secret123' ./server_1594
```

Keep this terminal open. The server uses TCP port `7594` for client
connections and file transfers, and UDP port `7595` for client presence
heartbeats.

## 3. Connect five clients

Open five more terminals in the project directory. Run one command in each
terminal; leave all five clients running so they can exchange messages.

**Client 1 terminal**

```sh
NETMSG_TOKEN='secret123' ./client_1594 client1
```

**Client 2 terminal**

```sh
NETMSG_TOKEN='secret123' ./client_1594 client2
```

**Client 3 terminal**

```sh
NETMSG_TOKEN='secret123' ./client_1594 client3
```

**Client 4 terminal**

```sh
NETMSG_TOKEN='secret123' ./client_1594 client4
```

**Client 5 terminal**

```sh
NETMSG_TOKEN='secret123' ./client_1594 client5
```

Each client sends the authentication token and registers its username
automatically. Use a different username for each connected client.

## 4. Test client commands

Type each command in the indicated client terminal and press Enter. Commands
are uppercase; do not type the angle brackets used for placeholders.

### Check the connection and online users

In any client terminal:

```text
HELP
LIST
ROOMS
```

`HELP` prints the available commands, `LIST` shows the connected usernames,
and `ROOMS` lists rooms that currently have members.

### Test broadcast messages

In the `client1` terminal:

```text
BCAST Hello everyone, this is client1.
```

The other connected clients should receive the message.

### Test private messages

In the `client2` terminal:

```text
PMSG client4 Hello client4, this is a private message.
```

The message should appear for `client4` and in the sender's and recipient's
message histories.

### Test rooms

In the `client1`, `client2`, and `client3` terminals, join the same room:

```text
JOIN project
```

In any client terminal, list the rooms:

```text
ROOMS
```

In the `client1` terminal, send a room message:

```text
RMSG project Hello project room.
```

The other members of `project` should receive the message. Now, in the
`client3` terminal, leave the room:

```text
LEAVE project
```

Send another `RMSG project ...` from `client1` and confirm `client3` no longer
receives it. `client1` and `client2` can still use the room.

### Test file transfer

In the project directory, create a small test file:

```sh
printf 'File transfer test from client1.\n' > sample.txt
```

In the `client1` terminal, send it to `client2`:

```text
SENDFILE client2 ./sample.txt
```

The receiver should report the incoming file. Received files are saved in
`downloads/<username>/`; for this example, check `downloads/client2/sample.txt`.
The server stores uploaded files under
`storage/IT19091594/client1/sample.txt`. File names must not contain spaces,
and file transfers are limited to 10 MB.

To send a file to members of a room, first have the sender join that room, then
use the room name as the target:

```text
JOIN project
SENDFILE project ./sample.txt
```

### Test disconnect and reconnect

In the `client5` terminal, disconnect:

```text
QUIT
```

Check `LIST` in another client; `client5` should no longer be listed. Start
`client5` again in a terminal using the same token:

```sh
NETMSG_TOKEN='secret123' ./client_1594 client5
```

After reconnecting, previously saved broadcast, private, and room messages for
that user should be replayed in the client.

## 5. Finish testing

Type `QUIT` in each remaining client terminal. Stop the server with **Ctrl+C**
in the server terminal.

For a remote server, include its IPv4 address after the username, for example:

```sh
NETMSG_TOKEN='secret123' ./client_1594 client1 192.0.2.10
```

Replace `192.0.2.10` with the server's actual IPv4 address. Ensure TCP port
`7594` and UDP port `7595` are reachable between the client and server.
