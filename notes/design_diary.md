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
