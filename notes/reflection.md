# Reflection — IE3090 Assignment

## Which AI tools did I use, and at which stages?

I used [tool name] throughout Part 1. At the start it helped me set up
a Linux VM (CentOS 10 in VMware Fusion) and understand the assignment's
personalisation requirements. During implementation it provided a
step-by-step guide covering the socket lifecycle, buffered readers, and
the thread-per-connection model. Toward the end it gave feedback on my
report structure and helped me phrase design justifications.

## What did the AI do well?

- Explaining the socket lifecycle (socket → bind → listen → accept →
  recv/send → close) with small, concrete code.
- Warning about the specific pitfalls the brief calls out — partial
  recv(), multiple lines in one buffer, and the text-followed-by-raw-bytes
  framing problem for PUT/GET.
- Structuring the work into incremental steps, which naturally produced
  the required commit history.
- Pointing out the security angle on EXEC (never pass user input to a
  shell) — which I had not fully appreciated.

## Where did it get things wrong or mislead me?

- It initially suggested writing a standalone "echo server" before the
  real Agent. I pointed out this wasn't a deliverable and skipped it.
- In Step E it told me I'd see DISCONNECT log lines in the console, but
  log_event only wrote to the file. We diagnosed that and added stdout
  echo.
- In Step G it designed the UDP payload without a trailing newline, which
  caused `nc -u -l` to concatenate datagrams. We added the newline.
- It underestimated how long Step F (PUT/GET) would take.

## What did I change, add, or reject from AI output, and why?

- Rejected the standalone echo server step — not part of the assignment.
- Changed the log_event function to also echo to stdout during development.
- Added the newline to the UDP payload.
- Wrote every comment in the code in my own words.
- Added a path-traversal guard to PUT/GET — the AI suggested it but I
  verified it manually with `GET ../../etc/passwd` and `PUT ../../tmp/evil.txt`.

## What did I learn about my own understanding of network programming?

- TCP is a byte stream, not a message stream. I had to build a buffered
  reader with `read_line` and `read_exact` to handle both text commands
  and raw file bytes over the same connection. This is the single most
  important insight from the assignment.
- Byte-for-byte file transfer is easy to get wrong — I now understand why
  `send()` and `recv()` must be looped until the exact count is met.
- Threads make per-connection state trivial, but shared resources (log
  file) need a mutex. I learned to keep shared state minimal.
- Protocol compliance matters more than clever code: the assignment
  specified the wire format, and my job was to implement it exactly, not
  invent a better one.
