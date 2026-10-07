# Selected Advanced Implementation Prompts

This file records prompts for the more challenging networking and reliability
work in NetMessenger. It intentionally focuses on the advanced additions, not
every command or feature in the project.

## 1. Add a UDP presence channel alongside the existing TCP service

> Add a UDP-based presence heartbeat channel to the C chat application while
> keeping chat and file transfers on the existing TCP connection and port.
> Configure the server to listen for UDP heartbeats on a separate port and make
> the client send a heartbeat periodically without blocking keyboard input or
> TCP message reception. Validate heartbeat usernames and only associate a
> heartbeat with an active registered TCP client from the same IP address.
> Avoid treating UDP as reliable or allowing arbitrary heartbeat packets to
> modify TCP registration state. Document the ports and test client-to-server
> heartbeat reception.

## 2. Add optional token authentication without breaking existing launches

> Add a lightweight, optional shared-token authentication step to the existing
> line-based TCP protocol. Read the server token from `NETMSG_TOKEN`; when it
> is configured, reject registration and other protected commands until the
> client successfully authenticates. Update the client to send the same token
> before registration when its environment contains `NETMSG_TOKEN`. If no
> token is configured, retain the existing unauthenticated startup behavior.
> Keep the current TCP port and message/file framing unchanged, handle wrong
> tokens explicitly, and document that this token travels over unencrypted TCP
> and is suitable only for trusted networks.

## 3. Persist chat messages and replay them after reconnect

> Add disk-backed chat history without changing existing live message formats
> or delivery behavior. Persist broadcast, private, and room messages for the
> participating users, using safe username-derived paths under the project's
> storage directory. Serialize concurrent history file access and report
> storage/read errors instead of silently pretending persistence succeeded.
> After a user registers on a new connection, replay their saved messages as
> clearly identifiable history lines over the existing TCP connection. Do not
> include file contents, tokens, or unrelated server events in chat history.
> Verify live delivery still works and that messages are replayed after a
> disconnect and reconnect.

## 4. Preserve protocol compatibility while integrating the new features

> Integrate optional authentication, per-user history replay, and UDP presence
> into the existing threaded C server and select-based client. Do not change
> existing TCP ports, command syntax, message delivery, or binary file-transfer
> framing. Pay attention to socket/thread lifetimes, shared client state,
> locking order, partial TCP reads, and heartbeat timing so UDP activity cannot
> interfere with chat or file transfer. Keep the existing command rate limiter
> intact. Build with the project's warning flags and exercise correct and
> incorrect authentication, live broadcast/private/room messages, reconnect
> history replay, and UDP heartbeat reception.

## 5. Document reproducible setup and validation

> Write a concise, step-by-step command guide for building and running the
> server, connecting local and remote clients, and using the configured token
> consistently on both processes. Explain the current chat/file commands,
> which commands are automatic protocol operations, the TCP and UDP ports,
> history and file locations, and the existing flood limit. For every
> documented command, explain what it does and how to supply its parameters.
> Ensure examples match the implemented protocol and do not imply TLS or
> encrypted transport where none is configured.
