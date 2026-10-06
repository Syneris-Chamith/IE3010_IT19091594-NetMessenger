# NetMessenger (IT19091594)

IE3010 Network Programming: multi-client chat and file-sharing over TCP/IP in C.

## Personalised values

| Item                | Value                                             |
| ------------------- | ------------------------------------------------- |
| Registration number | IT19091594                                        |
| Port                | 6000 + 1594 = 7594                                |
| NID tag             | NID:0915                                          |
| Source files        | server_1594.c, client_1594.c, Makefile_1594       |
| Log file            | netmsg_IT19091594.log                             |
| Storage path        | ./storage/IT19091594/<sender_username>/<filename> |
| Chat history        | ./storage/IT19091594/history/<username>.log      |
| Presence channel    | UDP port 7595 alongside TCP port 7594            |

## Build

    make -f Makefile_1594

## Run

    ./server_1594
    ./client_1594

## Added features

- **Token authentication (optional):** set the same non-empty `NETMSG_TOKEN`
  in the server and client environments. The client sends it before
  registration; the server then requires successful authentication before
  accepting chat commands. Leaving it unset keeps the original unauthenticated
  behavior for compatibility. This bearer token is sent over the existing
  unencrypted TCP connection, so use it only on a trusted network; it is not a
  substitute for TLS.

  Example:

      NETMSG_TOKEN='choose-a-private-token' ./server_1594
      NETMSG_TOKEN='choose-a-private-token' ./client_1594 amal

- **Persistent chat history:** broadcast, private, and room messages are
  appended to each participating user's history file. On reconnect, that
  user's saved entries are replayed as `MSG HISTORY` lines over the existing
  TCP connection. File transfers and presence notifications are not chat
  history entries.
- **UDP presence heartbeat:** clients send a `HEARTBEAT <username>` datagram
  every five seconds to UDP port 7595. The server accepts heartbeats only for a
  registered user connecting from the same IP address as that user's TCP
  connection. TCP chat and file transfers continue to use port 7594.
- **Flood protection:** the existing server command limiter remains in place
  (30 commands per client per 10-second window).
