# RemoteOps — IE3090 Network Programming Assignment

## Student
- Name: [Your Full Name]
- Registration number: **IT24100816**
- Module: IE3090 Network Programming (Year 3, Semester 1)

## Personalisation values (derived from registration number IT24100816)

| Item | Value |
|---|---|
| Numeric part | 24100816 |
| First 4 digits | 2410 |
| Last 4 digits | 0816 |
| Last 3 digits | 816 |
| **Agent listening port** | 7000 + 2410 = **9410** |
| **Session ID (SID)** | 0816 reversed = **6180** |
| **Auth token** | **OPS-0816** |
| **Log file** | **remoteops_IT24100816.log** |
| **File storage path** | **./agentfiles/IT24100816/** |
| **Source file names** | **agent_816.c**, **controller_816.c**, **Makefile_816** |
| **Submission ZIP** | **IE3090_IT24100816.zip** |

## Overview

RemoteOps is a remote system monitoring and management tool over TCP/IP. Two programs:

- **Agent** (`agent_816.c`) — server, listens on TCP port 9410. Serves multiple Controllers using a thread-per-connection model. Provides authentication, system info, process listing, restricted command execution, file upload/download, and periodic UDP monitoring. Logs all activity to `remoteops_IT24100816.log`.
- **Controller** (`controller_816.c`) — interactive client. Connects to the Agent, sends commands over TCP, uploads/downloads files, and receives periodic system-stats datagrams over UDP.

## Build

```bash
make
