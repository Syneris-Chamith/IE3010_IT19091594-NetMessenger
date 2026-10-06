# NetMessenger Commands and Run Guide

This guide describes how to build and run this project, connect clients, and
use every chat command supported by the current server.

## 1. Open a terminal in the project folder

Run the following command from the repository root:

```sh
cd /home/chamith/Documents/IE3010_IT19091594-NetMessenger
```

If you have cloned the project elsewhere, `cd` to that copy's folder instead.

## 2. Build the server and client

```sh
make -f Makefile_1594
```

This creates the `server_1594` and `client_1594` executables. To remove those
executables and rebuild them, run:

```sh
make -f Makefile_1594 clean
make -f Makefile_1594
```

## 3. Start the server

Open a terminal in the project folder and run:

```sh
./server_1594
```

The server listens for chat and file transfers on TCP port **7594**, and for
client presence heartbeats on UDP port **7595**. Keep the server running while
clients are connected. Press **Ctrl+C** in the server terminal to stop it.

### Start the server with token authentication

Authentication is optional. To enable it, set `NETMSG_TOKEN` when starting the
server. Choose a private token and do not publish or commit it.

```sh
NETMSG_TOKEN='choose-a-private-token' ./server_1594
```

The token must be no longer than 256 characters. A client connecting to this
server must use the same token. The token is transmitted over the existing
unencrypted TCP connection; use token authentication only on a trusted network.

## 4. Connect one or more clients

Leave the server running, then open a **separate terminal** in the project
folder for each client.

### Connect to a server on this computer

```sh
./client_1594 alice
```

Replace `alice` with the username for this client. Usernames and room names
must be 1-31 characters containing only letters, digits, `_`, or `-`.
If no username argument is provided, the client prompts for one:

```sh
./client_1594
```

Each client automatically connects to TCP port 7594, registers the selected
username, and sends UDP presence heartbeats to port 7595. Usernames must be
unique among currently connected clients.

### Connect to a server on another computer

```sh
./client_1594 alice 192.0.2.10
```

Replace `192.0.2.10` with the server's IPv4 address and `alice` with the
desired username. The client uses TCP port 7594 and UDP port 7595. Ensure the
server computer's firewall/network permits both ports.

### Connect when token authentication is enabled

Set the same token in each client's environment. For example:

```sh
NETMSG_TOKEN='choose-a-private-token' ./client_1594 alice
```

For a remote server:

```sh
NETMSG_TOKEN='choose-a-private-token' ./client_1594 alice 192.0.2.10
```

The client automatically sends `AUTH` before `REGISTER`. With authentication
enabled, the username is registered only after the token is accepted.

## 5. Chat commands

Type commands in the client terminal and press **Enter**. Commands are
case-sensitive and should be typed in uppercase as shown. Replace values in
angle brackets with your own values; do not type the angle brackets. Do not
type the `#` prompt shown in some examples.

### `HELP`

- **What this command does:** Displays the command list in the client.
- **How to give parameters and execute it:** Takes no parameters. Type
  `HELP` and press Enter.

```text
HELP
```

### `LIST`

- **What this command does:** Lists usernames of clients currently registered
  with the server.
- **How to give parameters and execute it:** Takes no parameters. Type `LIST`
  and press Enter.

```text
LIST
```

### `BCAST <message>`

- **What this command does:** Sends a message to all other currently connected
  registered clients.
- **How to give parameters and execute it:** Replace `<message>` with the
  message text. Type `BCAST ` followed by the text and press Enter.

```text
BCAST Hello everyone
```

Broadcast messages are saved in the history of participating users and replayed
to each user when they reconnect.

### `PMSG <username> <message>`

- **What this command does:** Sends a private message to one connected user.
- **How to give parameters and execute it:** Give the recipient's username
  first, followed by a space and the message text. The recipient must be
  connected.

```text
PMSG bob Hi Bob, are you available?
```

Private messages are saved in the sender's and recipient's histories.

### `JOIN <room>`

- **What this command does:** Joins a room, creating it if it does not already
  exist.
- **How to give parameters and execute it:** Replace `<room>` with a valid
  room name (1-31 letters, digits, `_`, or `-`) and press Enter.

```text
JOIN project
```

### `LEAVE <room>`

- **What this command does:** Leaves a room you previously joined. The room is
  removed when its last member leaves.
- **How to give parameters and execute it:** Replace `<room>` with the exact
  room name and press Enter.

```text
LEAVE project
```

### `ROOMS`

- **What this command does:** Lists rooms that currently exist (rooms exist
  while they have members).
- **How to give parameters and execute it:** Takes no parameters. Type
  `ROOMS` and press Enter.

```text
ROOMS
```

### `RMSG <room> <message>`

- **What this command does:** Sends a message to the other members of a room.
- **How to give parameters and execute it:** Give the room name first,
  followed by a space and the message. You must have joined the room.

```text
RMSG project The meeting starts now
```

Room messages are saved in the history of the sender and the other room
members, and replayed when those users reconnect.

### `SENDFILE <user-or-room> <path-to-file>`

- **What this command does:** Sends a file to a connected user or to the
  members of a room you have joined. The server stores the uploaded file under
  `storage/IT19091594/<sender_username>/` before forwarding it.
- **How to give parameters and execute it:** Provide a connected username or a
  room you belong to, then a space and the path to a regular file readable by
  the client process. File names must not contain spaces; files are limited to
  10 MB.

```text
SENDFILE bob ./test_files/sample.txt
SENDFILE project ./test_files/sample.bin
```

Received files are saved by the client under `downloads/<your_username>/`.
The client command accepts a path containing spaces, but the file's own name
must not contain spaces.

### `QUIT`

- **What this command does:** Closes the client connection politely and
  unregisters the username.
- **How to give parameters and execute it:** Takes no parameters. Type `QUIT`
  and press Enter.

```text
QUIT
```

## 6. Authentication protocol command

### `AUTH <token>`

- **What this command does:** Authenticates a TCP client using the token
  configured in the server's `NETMSG_TOKEN` environment variable. It is not
  needed when authentication is disabled.
- **How to give parameters and execute the command:** Provide the exact
  configured token after `AUTH` and press Enter. Normally, do not type this
  manually: launch the client with the same `NETMSG_TOKEN` and it sends
  `AUTH` automatically before registration.

```text
AUTH choose-a-private-token
```

If manually authenticating from a client started without `NETMSG_TOKEN`, send
`AUTH <token>` first and then `REGISTER <username>`. An `AUTH` failure must be
corrected before registration will succeed.

## 7. Registration protocol command

### `REGISTER <username>`

- **What this command does:** Registers a username on the server so it can
  participate in chat. The username must not already be in use.
- **How to give parameters and execute the command:** Provide a valid
  1-31-character username made of letters, digits, `_`, or `-`, then press
  Enter. The client normally sends this automatically at startup. It is
  available for manual registration after successful `AUTH`.

```text
REGISTER alice
```

## 8. Automatic presence and flood protection

The client sends `HEARTBEAT <username>` datagrams automatically every five
seconds over UDP port 7595; this is not a command to type in the chat prompt.
The existing TCP server command limiter allows up to 30 regular commands per
client in a 10-second window. If the limit is exceeded, the server responds
with `ERR 016 RATE_LIMITED`; wait for the window to reset before sending more
commands.

## 9. Persistent chat history and logs

Broadcast, private, and room messages are stored per user at:

```text
storage/IT19091594/history/<username>.log
```

After registration on a reconnect, the server replays that user's saved
messages as `MSG HISTORY` lines in the client. Server events are appended to
`netmsg_IT19091594.log`. Sent and received files are stored under `storage/`
and `downloads/`, respectively.