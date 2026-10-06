# Prompt Log — AI Interactions During Part 1

**Student:** W M S A Jayasena
**Registration Number:** IT24100816
**Module:** IE3090 — Network Programming
**Assignment:** RemoteOps — Part 1 (Take-Home Implementation)
**AI Tool Used:** Claude (Anthropic) — web chat interface

This log records every substantive AI interaction used while building Part 1
of the RemoteOps assignment. Entries are in chronological order. For each
entry I record the prompt I used, a summary of the response, how I used the
output, and what I changed or rejected.

---

## Entry 1 — Understanding the assignment and setting up the environment

**Prompt:**
"I have to build a RemoteOps client/server tool over TCP/IP in C using BSD
sockets for my IE3090 assignment. I don't know anything about this. Guide me
step by step. My environment is CentOS 10 in a VMware Fusion VM on a MacBook."

**Response summary:**
The AI explained the socket lifecycle (socket → bind → listen → accept →
recv/send → close) and proposed a phased plan (Steps A–I) starting with a
minimal Agent skeleton.

**What I did with it:**
Used the phased plan as my working structure. Set up the VM, verified gcc
and make were installed, and confirmed the project folder layout.

**What I changed / rejected:**
Rejected the suggestion to first build a standalone echo server exercise —
it was not a deliverable and I wanted to spend time on the real assignment.
Confirmed with the AI that skipping it was fine.

---

## Entry 2 — Personalisation values from my registration number

**Prompt:**
"My registration number is IT24100816. Calculate my personalised Agent port,
SID tag, auth token, log filename, and storage path from §2.4 of the brief."

**Response summary:**
Walked through the formula and produced:
- Port = 7000 + 2410 = 9410
- SID = 0816 reversed = 6180
- Token = OPS-0816
- Log = remoteops_IT24100816.log
- Storage = ./agentfiles/IT24100816/

**What I did with it:**
Verified each value by hand against the worked example in §2.4 (registration
IT21123456 → port 9112, SID 6543, token OPS-3456). Saved the values to
notes/personalisation.txt.

**What I changed / rejected:**
Nothing on the calculation — I verified it independently and it was correct.

---

## Entry 3 — Step A: The first Agent skeleton

**Prompt:**
"Show me the minimal code for an Agent that opens a TCP socket on port 9410,
accepts one client, and sends a greeting. I need to understand every line."

**Response summary:**
Provided a ~90-line agent_816.c with socket(), bind(), listen(), accept(),
recv(), send(), close(). Explained each call.

**What I did with it:**
Typed the file manually (not copy-pasted). Compiled with `make agent_816`
and tested with `nc 127.0.0.1 9410`. It printed
"OK REMOTEOPS AGENT READY SID:6180".

**What I changed / rejected:**
Renamed variables to my own style (e.g. listen_fd, client_fd). Added my own
personalisation comments at the top. Had to add setsockopt(SO_REUSEADDR)
after a bind error on restart — the AI had not included it initially.

---

## Entry 4 — Step B: Line-based framing

**Prompt:**
"The brief says the Agent must handle a single recv() returning a partial
line, or multiple lines in one buffer. How do I implement this correctly?"

**Response summary:**
Explained that TCP is a byte stream, not a message stream, and proposed a
read_line() that reads one byte at a time until '\n', and a send_line() that
always appends '\n'.

**What I did with it:**
Implemented both helpers. Tested with
`printf 'SYSINFO\nAUTH WRONG\n' | nc 127.0.0.1 9410` — got two correctly
ordered responses, proving multi-line framing.

**What I changed / rejected:**
Wrote my own variable names and comments. Kept the one-byte-at-a-time version
for correctness; noted that a buffered version came later in Step F.

---

## Entry 5 — Step C: Authentication state per connection

**Prompt:**
"How should I implement the AUTH command so that all other commands are
rejected until it succeeds, as required by §2.2(2)?"

**Response summary:**
Suggested a local int authenticated flag per connection, set to 1 only when
the token matches exactly. All non-AUTH commands guarded by a single
`if (!authenticated)` check.

**What I did with it:**
Implemented the flag and the AUTH branch. Verified with three tests: AUTH
with correct token → "OK AUTHENTICATED SID:6180"; wrong token → "ERR 001
AUTH_FAILED"; command before AUTH → "ERR 001 AUTH_REQUIRED". All passed.

**What I changed / rejected:**
Chose to allow re-AUTH after failure (the AI's initial version didn't specify).
Added QUIT handling with "OK BYE SID:6180" at the same time.

---

## Entry 6 — Step D: SYSINFO, LISTPROC, and EXEC whitelist

**Prompt:**
"Implement SYSINFO (reading CPU/mem/uptime from /proc), LISTPROC (via
popen ps), and EXEC with the fixed whitelist DATE/UPTIME/DISKFREE/HOSTNAME/
WHOAMI. Emphasise why the EXEC whitelist must never pass user input to a
shell."

**Response summary:**
Gave three functions:
- build_sysinfo() reading /proc/loadavg, /proc/meminfo, /proc/uptime.
- build_listproc() using popen("ps -eo comm --no-headers").
- build_exec() as a hardcoded if/else if chain mapping each whitelisted name
  to a string-literal shell command — never the user's raw input.

**What I did with it:**
Implemented all three. Tested EXEC rm -rf /, EXEC ls, EXEC cat /etc/passwd —
all correctly rejected with "ERR 002 COMMAND_NOT_ALLOWED". Confirmed the
security property: user input never reaches system() or popen().

**What I changed / rejected:**
Capped LISTPROC output with ",..." truncation because the process list was
very long. Capped EXEC output to the first line only (protocol requires
single-line responses).

---

## Entry 7 — Step E: Thread-per-connection and mutex-protected logging

**Prompt:**
"The Agent needs to serve at least 5 simultaneous Controllers. Recommend a
concurrency model and justify it. Also implement logging to
remoteops_IT24100816.log with timestamps, protected by a mutex."

**Response summary:**
Recommended thread-per-connection with detached pthreads. Explained the
trade-offs versus fork and versus select/poll. Provided a log_event()
function using pthread_mutex_t and localtime_r.

**What I did with it:**
Implemented threading with pthread_create + pthread_detach. Tested with
`for i in 1 2 3 4 5; do (printf 'AUTH OPS-0816\nSYSINFO\nQUIT\n' | nc
127.0.0.1 9410 &) done; wait` — all 5 succeeded. Also tested an abrupt
Ctrl+C mid-connection; the Agent detected the disconnect and logged
"DISCONNECT ... [unauthed]" without crashing.

**What I changed / rejected:**
Initially log_event only wrote to the file. I asked the AI to also echo to
stdout for easier live debugging — it agreed and I applied that change.
Committed as a separate "Step E-fix" commit.

---

## Entry 8 — Step F: PUT and GET with exact byte-count framing

**Prompt:**
"The protocol says PUT and GET must transfer exactly <filesize> bytes
regardless of how many recv()/send() calls it takes. My current read_line()
reads one byte at a time, which will not work for binary data. How do I fix
this?"

**Response summary:**
Proposed a per-session 8 KB buffer with two helpers: sread_line() (reads up
to '\n', leaves excess bytes in buffer) and sread_exact(n) (drains buffer
first, then recv()s remaining). Also proposed send_exact() for GET.

**What I did with it:**
Rewrote the reader infrastructure. Tested with a 1 MB random file: uploaded
via PUT, downloaded via GET, extracted the raw bytes from the GET response
with a Python script, then compared with cmp and sha256sum. All three hashes
matched — byte-for-byte identical.

**What I changed / rejected:**
Added is_safe_filename() to reject filenames containing '/' or '..' after
asking about security. Added draining logic in handle_put so the socket
stays framed on ERR 004 rejection. Capped uploads at 10 MB.

---

## Entry 9 — Step G: UDP monitoring thread

**Prompt:**
"Implement MONITOR START <udp_port> and MONITOR STOP. When START is issued,
the Agent should send periodic SYSINFO datagrams over UDP to the Controller's
IP at the given port. The datagram must include SID:6180. STOP must stop
the stream. Also handle disconnect while monitoring."

**Response summary:**
Proposed a per-session monitor_active flag, a monitor_thread spawned on
START, and a 100 ms polling loop so STOP takes effect quickly.

**What I did with it:**
Implemented monitor_thread with a 1 Hz send loop and 100 ms polling. Tested
with `nc -u -l 9999` and `MONITOR START 9999`.

**What I changed / rejected:**
The AI's first version produced datagrams without a trailing newline, so
`nc -u -l 9999` concatenated them into a wall of text. I asked for a fix and
we added '\n' to the payload. Committed as "Step G-fix". Also chose 1 second
as the interval (documented in the report).

---

## Entry 10 — Step H: Interactive Controller program

**Prompt:**
"Write the interactive Controller (controller_816.c) that connects to the
Agent, reads commands from stdin, sends them over TCP, and prints responses.
For MONITOR START, it should also spawn a UDP listener thread that prints
incoming datagrams."

**Response summary:**
Provided a full controller_816.c with:
- TCP connection to 127.0.0.1:9410
- Interactive prompt reading from stdin
- Special handling for AUTH, PUT (read local file, send bytes), GET (read
  header, extract N bytes to ./downloads/)
- UDP listener thread spawned on MONITOR START

**What I did with it:**
Implemented the full file. Tested every command interactively. Confirmed the
[UDP] lines appeared live while the prompt stayed ready.

**What I changed / rejected:**
Reused the same buffered reader design as the Agent for consistency. Did not
cache auth state — kept the Controller a "transparent protocol client" so I
could point at any line in the Viva and say "this is exactly what went on
the wire".

---

## Entry 11 — Report structure and content

**Prompt:**
"Help me structure the Implementation Report for §2.7 of the brief. Give me
the full content ready to paste into Word, with markers showing where each
screenshot should go."

**Response summary:**
Provided a 9-section report structure (Personalisation, Architecture,
Concurrency, Protocol evidence, Personalisation proof, Annotated code,
Execution screenshots, Testing summary, Design rationale) with figure markers
indicating where each screenshot belongs.

**What I did with it:**
Wrote the report in Word on my MacBook. Inserted my own screenshots at each
marker. Adjusted wording to match my own voice.

**What I changed / rejected:**
Rewrote the rationale paragraphs in my own words. Expanded the testing table
based on the actual tests I ran. Rejected suggested screenshots that did not
match my real evidence.

---

## Entry 12 — Report review

**Prompt:**
"Review my Implementation Report and point out issues."

**Response summary:**
Flagged figure/caption mismatches in Section 6, missing screenshots, TOC
formatting issues, and a few typos.

**What I did with it:**
Fixed the caption mismatches, added the missing screenshots, updated the
Word TOC, and corrected typos.

**What I changed / rejected:**
Reviewed each suggested fix against the brief and my report. Applied only
the fixes that were correct.

---

## Entry 13 — Screenshot strategy

**Prompt:**
"Should I use the interactive Controller or printf | nc for the protocol
screenshots? Which gives stronger evidence?"

**Response summary:**
Explained that both are acceptable, but that using the real Controller
demonstrates the Controller program working — a required deliverable per
§2.9. Recommended the Controller for protocol screenshots and shell commands
for OS-level proofs (ss, ls, sha256sum).

**What I did with it:**
Re-took the AUTH and other protocol screenshots using the interactive
Controller. Kept the shell-based screenshots for personalisation proof.

**What I changed / rejected:**
Used `printf | nc` for the multi-line framing test (Figure 7) because the
interactive Controller sends one command per Enter, so it cannot easily
demonstrate "4 commands in one TCP write".

---

## Entry 14 — Prompt log itself

**Prompt:**
"What does §3 of the assignment require for the prompt log? Give me a
template."

**Response summary:**
Explained the format (tool / prompt / usage / changes) and emphasised
honesty, specificity, and critical engagement with AI output.

**What I did with it:**
Wrote this prompt log using that structure. Amended each entry to accurately
reflect how I actually used the AI's output.

**What I changed / rejected:**
Rewrote entries in my own words. Added details specific to my environment
and tests. Removed any wording that did not reflect real interactions.

---

## Summary

- Total substantive AI interactions logged: 14
- Purpose of AI use: learning socket programming, structured implementation
  guidance, protocol design advice, debugging, and report feedback.
- All AI-generated code was typed manually, tested against the assignment
  specification, and modified where necessary.
- Sections of AI output that I rejected or changed are noted in each entry.
- I am prepared to explain every part of my submission in the Viva, including
  any part that was influenced by AI output.
