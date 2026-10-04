## Step C — AUTH success + session state

- Added `authenticated` flag per connection. Initially 0.
- AUTH with correct token → sets flag, replies OK AUTHENTICATED SID:6180.
- AUTH with wrong token → ERR 001 AUTH_FAILED SID:6180, connection
  stays open, client can retry.
- All non-AUTH commands guarded by a single `if (!authenticated)` block —
  this one check enforces §2.2(2) for every current and future command.
- QUIT closes the connection cleanly (feeds §2.2(9) graceful disconnect).
- Authed but unimplemented command → ERR 002 COMMAND_NOT_ALLOWED.
- Test C4 proves multi-line framing: a single printf|nc sent 4 lines,
  Agent replied with 4 correctly-ordered lines, one per input line.

### Known limitation
- authenticated is a local variable in main(), so it resets per connection.
  This is correct for now (one client at a time). Step E (threads) will
  move this into a per-connection session struct.
