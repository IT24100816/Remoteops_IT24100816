## Step A — Agent skeleton

- Created the initial `agent_816.c` with the standard TCP server sequence:
  socket() → bind() → listen() → accept() → recv()/send() → close().
- Bound to the personalised port **9410** (7000 + first four digits of my
  registration number, IT24100816 → 2410).
- Set `SO_REUSEADDR` on the listening socket so the Agent can be restarted
  quickly without hitting "address already in use".
- Sent a single greeting line `OK REMOTEOPS AGENT READY SID:6180` to the
  first client, then closed the connection. This proved the personalised
  SID tag was wired in from the start.
- Personalised values hardcoded as macros at the top of the file:
  - `AGENT_PORT  9410`
  - `SID_TAG     "6180"`
  - `AUTH_TOKEN  "OPS-0816"`
  - `LOG_FILE    "remoteops_IT24100816.log"`
  - `STORAGE_DIR "./agentfiles/IT24100816"`
- Created `Makefile_816` with targets for `agent_816` and `controller_816`
  (the controller target was a placeholder until Step H). Compiles cleanly
  with `-Wall -Wextra -pthread`.

### Obstacles

- The first version had no `setsockopt(SO_REUSEADDR)`, so restarting the
  Agent after a test failed with `bind: Address already in use`. Added the
  option and the problem disappeared.
- Tested with `nc 127.0.0.1 9410` and confirmed the greeting was received.

### Why this design

- Single-client, single-threaded to start with. Threading was deliberately
  deferred to Step E so I could get the protocol right first, then scale.
- All personalised values centralised in `#define` macros so that if any
  value changes, only one place needs editing.

## Step B — Framing & command dispatch

- Implemented read_line() reading one byte at a time until '\n'.
  - Reason: never over-reads past the end of a line, so multi-line TCP
    segments are handled correctly without losing data.
  - Trade-off: inefficient (one syscall per byte). Will replace with a
    buffered reader in Step F when PUT/GET require raw byte reads.
- Implemented send_line() to guarantee every response ends with '\n'.
- All responses end with SID:6180 per §2.3.
- AUTH is not yet accepted — Step C will flip authenticated=1 on the
  correct token.

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

## Step D — SYSINFO, LISTPROC, EXEC

- SYSINFO reads /proc/loadavg, /proc/meminfo, /proc/uptime directly.
  Chose /proc over sysinfo() syscall because /proc is the modern
  Linux-standard interface and parsing with fscanf is trivial.
- LISTPROC uses popen("ps -eo comm --no-headers"), joined with commas,
  capped at ~4 KB to keep the response a single line.
- EXEC uses a hardcoded if/else-if chain mapping each whitelisted name
  to a string literal shell command. The user's input string NEVER
  reaches a shell — this is the §2.5 security requirement. Verified by
  sending "EXEC rm -rf /", "EXEC ls", "EXEC cat /etc/passwd" — all
  rejected with ERR 002 COMMAND_NOT_ALLOWED.
- Known limitation: EXEC only returns the FIRST line of the command's
  output, because the protocol requires single-line responses. df -h /
  and uptime normally produce multiple lines; only the first survives.
  This is a protocol constraint, not a bug.

## Step E — Threading + Logging

### Concurrency model chosen: thread-per-connection
- accept() loop in main() spawns a detached pthread per client.
- Reason: simplest model that maps directly to the socket lifecycle.
  Each thread owns its client fd and its own session state (authenticated
  flag for now; UDP monitor state in Step G). No polling overhead, no
  per-process memory cost of fork, no select/poll bookkeeping.
- Justification vs alternatives:
  * select/poll: harder framing logic, single-threaded, more error-prone.
  * fork: heavy per-connection memory, complicates UDP monitor sharing.
- Detach (pthread_detach) chosen so main() never needs to join();
  resources are freed automatically when the thread exits.

### Logging
- Central log_event() with pthread_mutex protecting the FILE*.
- Timestamp format: YYYY-MM-DD HH:MM:SS, local time (localtime_r).
- Logged events: AGENT START, CONNECT, every CMD with source IP,
  AUTH OK/FAILED, QUIT, DISCONNECT (marks authed vs unauthed).
- fflush() after every entry so logs survive a crash.

### Verification
- Ran 5 parallel clients via shell for-loop. All got correct responses.
- Terminal 1 showed interleaved CONNECT/DISCONNECT lines — visual proof
  of concurrency.
- Killed an idle client with Ctrl+C — Agent logged DISCONNECT and
  survived. Confirms §2.2(9) ungraceful disconnect handling.

## Step F — PUT and GET

### Buffered reader
- Replaced byte-by-byte read_line() with a per-session 8 KB buffer.
- sread_line() reads until '\n', leaving any excess bytes in buf.
- sread_exact(n) first drains buf, then recv()s for the rest.
- This is the §2.3 framing requirement: the Agent must transfer exactly
  <filesize> bytes regardless of how many send()/recv() calls the
  client makes, and must not lose bytes between the command line and
  the file content.

### PUT
- Format: PUT <filename> <filesize>\n then exactly filesize raw bytes.
- Rejects unsafe filenames (containing '/' or '..').
- 10 MB cap → ERR 004 FILE_TOO_LARGE; drains the incoming bytes even
  on rejection so the connection stays framed.
- Writes to ./agentfiles/IT24100816/<filename> in 4 KB chunks.

### GET
- Format: GET <filename> → OK FILE_SEND <filename> <size> SID:6180\n
  then exactly size raw bytes.
- Uses send_exact() to loop until all bytes are written.
- Missing file → ERR 005 FILE_NOT_FOUND.
- Same path-traversal guard as PUT.

### Verification
- Test F3 (round-trip): uploaded test_binary.bin (1 MB random), GET'd
  it, compared with cmp + sha256sum → identical. Proves §2.2(7).
- Test F5: GET ../../etc/passwd and PUT ../../tmp/evil.txt both rejected.
- Test F6: 20 MB upload rejected with ERR 004.
- Test F4: GET of a nonexistent file returned ERR 005.

## Step G — UDP monitoring

### Architecture
- Each TCP session may have AT MOST one active UDP monitor.
- MONITOR START <udp_port>:
  * Records controller IP (from accept()) and requested port.
  * Sets monitor_active = 1.
  * Spawns a detached pthread that sends a UDP datagram once per second.
- MONITOR STOP:
  * Sets monitor_active = 0; monitor thread exits within ~100 ms.
  * Idempotent: if not monitoring, replies OK MONITOR_STOPPED anyway.

### Datagram payload
- Format: "SYSINFO <cpu_load> <mem_used_mb> <uptime_sec> SID:6180"
- Same content as the SYSINFO TCP response, minus the leading "OK ".
- Every datagram carries SID:6180 as required by §2.3.
- Interval: 1 second. (Chosen for testability; the brief permits any
  regular interval — documented in the report.)

### Monitor thread lifecycle
- Uses a per-thread UDP socket (socket() on entry, close() on exit).
- Loop checks monitor_active every 100 ms so STOP takes effect fast.
- On session teardown (QUIT or disconnect), the client thread sets
  monitor_active = 0 BEFORE closing the TCP fd.

### Memory note
- We deliberately do NOT free(session_t) at end of client thread.
  Reason: a detached monitor thread may still be reading session fields
  when the client thread exits. Freeing would risk use-after-free.
- Cost: ~8 KB leaked per connection lifetime of the Agent process.
  Acceptable for this assignment; documented.

### Verification
- Test G1: MONITOR START 9999 → receiver on UDP 9999 saw datagrams
  every second, each ending in SID:6180.
- Test G2: MONITOR STOP → datagrams stopped within ~100 ms, TCP reply
  was OK MONITOR_STOPPED SID:6180.
- Test G3: killed the TCP client mid-monitoring → datagrams stopped,
  Agent survived, log recorded MONITOR stopped.
- Test G4: MONITOR START 99999 → ERR 008 BAD_UDP_PORT.
- Test G5: second MONITOR START while active → ERR 007 ALREADY_MONITORING.

## Step H — Interactive Controller

### Architecture
- Single-threaded for TCP; a UDP listener pthread is spawned on
  demand for MONITOR START.
- Reads commands from stdin (interactive), forwards them verbatim
  to the Agent, prints the Agent's response line.
- Handles three command classes specially:
  * AUTH      — sends "AUTH <token>\n", reads one line.
  * PUT       — reads local file, sends "PUT <name> <size>\n" then
                exactly size raw bytes, reads one line response.
  * GET       — sends "GET <name>\n", reads response line, and if
                it starts with "OK FILE_SEND", reads exactly size
                raw bytes and writes to ./downloads/<name>.
  * MONITOR START <port> — after the Agent confirms, spawns a UDP
                listener bound to 0.0.0.0:<port>, prints each
                datagram as [UDP] ... until MONITOR STOP.
  * MONITOR STOP — sets the shared udp_running flag to 0; listener
                exits within 200 ms (socket has SO_RCVTIMEO).

### Design choices
- Same buffered-reader as the Agent — required because GET responses
  intermix a text line and raw bytes; the naive "read line by line"
  approach would corrupt binary files.
- No caching of auth state — the Controller stays a transparent
  protocol client. Simplifies the Viva: I can point at any line and
  say "this is exactly the byte that went on the wire".
- download directory ./downloads is auto-created with mkdir().

### Verification
- Test H3: uploaded upload_test.txt (54 bytes), got it back, all
  three SHA-256 hashes matched (source, agent-stored, downloaded).
- Test H4: MONITOR START 9999 printed [UDP] lines once per second;
  MONITOR STOP stopped them within 200 ms.

## Step I — Final testing and packaging

- Ran full regression sweep (C1–C5, D1–D5, F1–F6, G1–G5, H1–H5):
  all passed.
- Prepared Implementation Report (report.pdf), README, prompt log,
  structured reflection.
- Packaged IE3090_IT24100816.zip with:
  agent_816.c, controller_816.c, Makefile_816, log excerpt,
  Implementation Report.
- Submission ready for CourseWeb by 7 Oct 2026 22:59.

### Known limitations and honest notes
- Client session_t not freed at thread exit (avoids use-after-free
  against the monitor thread). ~8 KB per connection; acceptable.
- EXEC returns only the first line of a command's output (protocol
  requires single-line responses).
- Monitoring interval is 1 second; chosen for test visibility.
- No TLS/TLS extension implemented (optional §2.6 extension).
