# SysWatch

**Project Description:** System Monitoring Tool in Linux

**SysWatch** is a multi-threaded system monitoring daemon designed to ensure continuous visibility of CPU, memory, disk, & log file health.  
It provides real-time alerts, a TCP JSON status endpoint, & safe concurrent data access using POSIX threads, synchronization primitives, & signal-based lifecycle management.

**Author:** Eshan Sud (Raspberry Pi 5 Implementation)  
**Course:** Linux System & Shell Programming  
**Date:** November 2025

---

## Environment

- **Hardware:** Raspberry Pi 5 (8 GB RAM)
- **OS:** Raspberry Pi OS (64-bit, Debian Bookworm)
- **Compiler:** GCC 12.2 (ARM64)
- **Tools / Utilities:** `make`, `gcc`, `pthread`, `poll`, `netcat-openbsd` (`nc`), `jq` (optional), `mysql-client` (optional for DB insert)

> Note: `netcat` is provided by the package `netcat-openbsd` on Debian/Raspbian. Install it explicitly (see next section).

---

## Dependencies / Install

Install runtime / testing tools used in this project:

```bash
sudo apt update
sudo apt install -y netcat-openbsd jq mysql-client
```

- `jq` is optional but recommended for pretty-printing JSON from the TCP status endpoint.
- `mysql-client` is only required if you plan to use the wrapper's MySQL `insert` functionality.

---

## Features Implemented

| Feature                               | Description                                                                                                                      |
| ------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| 🧵 **Multi-threaded Monitoring Core** | Uses POSIX threads to monitor CPU & memory (every 5 s), disk (every 10 s), logs (via `poll()`), & serve TCP status concurrently. |
| 🔒 **Thread Synchronization**         | Shared metrics protected with `pthread_mutex_t` & condition variables for safe concurrent access.                                |
| 🚦 **Signal Handling**                | Graceful shutdown (`SIGTERM`), metrics dump (`SIGUSR1`), & config reload (`SIGHUP`).                                             |
| 📡 **Network Service**                | TCP server (default port 9999) returns current & historical metrics as JSON.                                                     |
| 📂 **Rolling Metrics Log**            | Maintains a ring buffer of recent samples & appends metrics to `metrics.log`.                                                    |
| 📜 **Log Monitoring with poll()**     | Watches multiple log files for `"error"`/`"fail"` patterns & triggers alerts in real time.                                       |
| 💾 **Configuration Management**       | Parses a simple key=value config file (`syswatch.cfg`) for runtime parameters.                                                   |
| 🧩 **Shell Wrapper**                  | `syswatch_ctl.sh` automates start/stop/status, supports argument parsing (`getopts`), & simulates remote execution.              |
| 🌐 **Remote Execution (Conceptual)**  | Wrapper supports `-r "host1 host2"` via SSH to demonstrate distributed monitoring capability (needs real remote hosts to run).   |

---

## Build Instructions

```bash
# compile with Makefile
make
```

Or compile manually:

```bash
gcc -std=gnu11 -O2 -pthread -Wall -Wextra -D_GNU_SOURCE -o syswatch syswatch.c
```

---

## Run Instructions

### Basic run (non-root)

Use `test.log` in the config for non-root testing (recommended):

```bash
# start in background
./syswatch -c syswatch.cfg &

# find pid
pgrep -a syswatch
```

### View live metrics

```bash
tail -f metrics.log
```

### Query JSON Status (TCP Service)

Prefer this exact command to reliably send an empty request body:

```bash
printf "" | nc localhost 9999 | jq .
# OR
nc localhost 9999 < /dev/null | jq .
```

If you don't have `jq`, omit the `| jq .` part. If `nc` is missing, install `netcat-openbsd` as shown above.

### Send signals

```bash
kill -USR1 $(pgrep -f syswatch)   # Force metrics dump to file
kill -HUP  $(pgrep -f syswatch)   # Reload config
kill -TERM $(pgrep -f syswatch)   # Graceful shutdown
```

> **Permissions note:** If you configure `LOGFILES` to include system logs (e.g., `/var/log/syslog`), SysWatch may need to run as root or a user with read permissions to those files:
>
> - Use `sudo ./syswatch -c syswatch.cfg` or add your user to the `adm` group (where applicable).
> - Binding ports <1024 requires root; default port 9999 does not.

---

## Configuration Example (`syswatch.cfg`)

```ini
# SysWatch configuration
LOGFILES=./test.log,/var/log/syslog,/var/log/auth.log
PORT=9999
METRICS_LOG=./metrics.log
RING_SIZE=200
```

- `test.log` is convenient for local testing without root.
- `RING_SIZE` controls how many samples are kept in the in-memory ring buffer.

---

## Testing & Results

### Metrics Collection

Confirmed CPU, memory, & disk values are logged every 5 seconds:

```
2025-11-12 17:42:17 cpu=0.05 mem=6.12 disk=44.94
2025-11-12 17:42:22 cpu=0.00 mem=6.13 disk=44.94
...
```

### TCP Status

Command:

```bash
printf "" | nc localhost 9999
```

Example output (truncated):

```json
{
  "current": { "cpu": 0.05, "memory": 6.47, "disk": 44.99 },
  "samples": [ { "timestamp": "2025-11-12 17:42:12", "cpu": 0.00, "memory": 6.17, "disk": 44.94 }, ... ]
}
```

### Signal Handling

- `SIGUSR1` → triggered immediate dump to `metrics.log`.
- `SIGHUP` → printed `"Reloading config"` & re-parsed `syswatch.cfg`.
- `SIGTERM` → stopped gracefully; threads joined cleanly.

### Log Monitoring

With `LOGFILES=./test.log`:

```bash
echo "ERROR: test failure" >> test.log
```

SysWatch produced an alert & wrote to `metrics.log`:

```
[ALERT 2025-11-12 18:00:21] Log ./test.log contains error pattern
```

### Remote Execution (Simulated)

`syswatch_ctl.sh` supports remote operations via SSH:

```bash
./syswatch_ctl.sh -r "host1 host2" -s remote-start
```

- For a real multi-host deployment, replace `host1 host2` with reachable IPs/usernames & ensure SSH is configured (keys/permissions).
- In this single-device test environment, remote hosts were placeholders & SSH attempts failed as expected — the wrapper demonstrates the capability & how to run it.

---

## Shell Wrapper Usage (`syswatch_ctl.sh`)

```bash
# Start locally
./syswatch_ctl.sh -s start

# Stop locally
./syswatch_ctl.sh -s stop

# Query status (uses nc)
./syswatch_ctl.sh -p 9999 -s status

# Remote start (conceptual)
./syswatch_ctl.sh -r "user@192.168.1.10 user@192.168.1.11" -i ~/.ssh/id_rsa_syswatch -s remote-start
```

---

## Optional: `systemd` service (suggested for production / auto-start)

Create `/etc/systemd/system/syswatch.service` (adjust `User` & paths):

```ini
[Unit]
Description=SysWatch monitoring daemon
After=network.target

[Service]
Type=simple
User=minor-project
WorkingDirectory=/home/minor-project/linux-proj/repo-transfer
ExecStart=/home/minor-project/linux-proj/repo-transfer/syswatch -c /home/minor-project/linux-proj/repo-transfer/syswatch.cfg
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable & start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now syswatch.service
sudo journalctl -u syswatch.service -f
```

---

## Implementation Summary per Task

| Task                                       | Description                                                                | Completed |
| ------------------------------------------ | -------------------------------------------------------------------------- | :-------: |
| **Task 1:** Multi-threaded Core            | Threads for CPU/mem, disk, logs, network; mutex + condvar + signal masking |    ✅     |
| **Task 2:** Signal Handling & Process Mgmt | Handled SIGTERM/SIGUSR1/SIGHUP via dedicated sigwait thread                |    ✅     |
| **Task 3:** I/O Multiplexing               | Used `poll()` for multi-file log monitoring with rotation handling         |    ✅     |
| **Task 4:** Shell Wrapper & Remote Exec    | Bash `getopts` parser, MySQL placeholders, remote SSH execution simulation |    ✅     |

---

## Conclusion

SysWatch demonstrates:

- Proper thread synchronization & safe data sharing.
- Real-time monitoring & alerting using POSIX primitives.
- Robust signal-driven daemon behavior.
- Extensible design for distributed monitoring.

---

## Appendix: Quick test script

Create `test_logs.sh` to automate a basic test (restart, append error, query JSON, dump):

```bash
#!/bin/bash
CFG="./syswatch.cfg"
BIN="./syswatch"
LOG="./test.log"

pkill -f syswatch || true
sleep 1
$BIN -c $CFG &
PID=$!
printf "Started syswatch pid=%d\n" "$PID"
sleep 2

echo "OK $(date)" >> $LOG
sleep 1
echo "CRITICAL: disk failure simulated at $(date)" >> $LOG
sleep 1

printf "----- metrics.log tail -----\n"
tail -n 30 metrics.log

printf "----- tcp status -----\n"
printf "" | nc localhost 9999

kill -USR1 $PID
sleep 1
printf "----- after dump -----\n"
tail -n 40 metrics.log

kill -TERM $PID
printf "Stopped syswatch\n"
```

Make it executable:

```bash
chmod +x test_logs.sh
./test_logs.sh
```

---
