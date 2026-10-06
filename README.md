# RemoteOps — IE3090 Network Programming Assignment

**Remote System Monitoring and Management Tool over TCP/IP**

## Student Information

| Field | Value |
|---|---|
| **Name** | W M S A Jayasena |
| **Registration Number** | IT24100816 |
| **Module** | IE3090 — Network Programming |
| **Year / Semester** | Year 3, Semester 1 |
| **Institution** | Sri Lanka Institute of Information Technology (SLIIT) |
| **Assignment** | RemoteOps — Part 1 (Take-Home Implementation) |

---

## Personalisation Values

All personalised values below are derived from my registration number
**IT24100816** according to section 2.4 of the assignment brief.

| Item | Formula | Value |
|---|---|---|
| Registration number | — | IT24100816 |
| Numeric part | digits only | 24100816 |
| First four digits | — | 2410 |
| Last four digits | — | 0816 |
| Last three digits | — | 816 |
| **Agent listening port** | 7000 + first four digits | **9410** |
| **Session ID (SID) tag** | last four digits reversed | **6180** |
| **Authentication token** | "OPS-" + last four digits | **OPS-0816** |
| **Log file name** | `remoteops_<regno>.log` | **remoteops_IT24100816.log** |
| **File storage path** | `./agentfiles/<regno>/` | **./agentfiles/IT24100816/** |
| **Agent source file** | `agent_<last3>.c` | **agent_816.c** |
| **Controller source file** | `controller_<last3>.c` | **controller_816.c** |
| **Makefile** | `Makefile_<last3>` | **Makefile_816** |
| **Submission archive** | `IE3090_<regno>.zip` | **IE3090_IT24100816.zip** |

---

## Overview

RemoteOps is a remote system monitoring and management tool that consists of
two programs communicating over TCP/IP, with a secondary UDP channel for
periodic monitoring.

### Agent (`agent_816.c`)

The server program, running on the managed machine. It:

- Listens for TCP connections on port **9410**
- Serves multiple simultaneous Controllers using a
  **thread-per-connection** concurrency model
- Requires successful authentication before accepting any other command
- Provides SYSINFO, LISTPROC, and a restricted EXEC whitelist
- Handles file uploads (PUT) and downloads (GET) with exact byte-count framing
- Sends periodic UDP datagrams for monitoring on request
- Logs all activity to `remoteops_IT24100816.log` with timestamps

### Controller (`controller_816.c`)

The interactive client used by an administrator. It:

- Connects to the Agent over TCP
- Reads commands from the user via stdin
- Sends them using the specified protocol
- Handles PUT (uploads a local file) and GET (saves the downloaded file to
  `./downloads/`)
- Spawns a UDP listener thread on `MONITOR START` to display incoming
  system-stats datagrams live

---

## Build Instructions

### Requirements

- Linux environment (tested on CentOS 10)
- gcc compiler
- make
- POSIX threads library (linked via `-pthread`)

### Build

From the project root directory:

```bash
make
