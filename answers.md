# Answers to Assignment Questions

---

## Task 1 — Thread-safety / attributes / synchronization

### How will you ensure thread-safe access to shared metrics data?

- Shared metrics are stored in `system_metrics_t` and protected by a `pthread_mutex_t data_lock`. Any reader or writer must acquire `pthread_mutex_lock(&sys_metrics.data_lock)` before accessing or modifying the fields, and release it after the operation.
- A `pthread_cond_t update_cond` is used to notify waiting consumer threads after updates. This lets consumers block efficiently until a new sample arrives.
- The in-memory ring buffer (`ringbuffer_t`) has its own `pthread_mutex_t lock` to serialize push/snapshot operations. All accesses to ring buffer state (`head`, `count`, `buf[]`) occur while holding that mutex.
- An `atomic_int running` flag is used to coordinate shutdown and avoid races when threads check whether to continue running.

### What thread attributes would you set for this monitoring application?

- The implementation uses default (joinable) attributes for portability. For production use you might:
  - Set thread stack size (`pthread_attr_setstacksize`) if a thread needs extra stack.
  - Use real-time scheduling policies (`SCHED_RR` / `SCHED_FIFO`) for very critical threads to reduce latency (requires privileges).
  - Use `pthread_setaffinity_np` to pin latency-critical threads to specific CPU cores to reduce jitter.
  - Create non-joinable (detached) helper threads where joining isn't needed and resources should free automatically.
- Keep defaults for this assignment for portability and simplicity.

### How will threads communicate & synchronize their monitoring activities?

- Producer threads (CPU/memory and disk threads) update `sys_metrics` and push `metric_sample_t` entries into the ring buffer under their respective mutexes.
- After updating, producers call `pthread_cond_broadcast(&sys_metrics.update_cond)` so consumer threads (if any) can wake and process new metrics.
- The `log_monitor_thread` signals alerts immediately when error patterns are found; it does not require the condition variable for its primary function.
- `atomic_int running` provides a lock-free, safe way to signal threads to exit during shutdown.

---

## Task 2 — Signal handling & process management

### Signal handler design

- Signals handled: `SIGTERM` (graceful shutdown), `SIGUSR1` (force metrics dump), `SIGHUP` (reload config).
- Instead of installing async signal handlers, the program **blocks** these signals in all threads using `pthread_sigmask()` and spawns a dedicated signal-handling thread which calls `sigwait()` to synchronously receive signals. This avoids the restrictions of async-signal-safe functions inside signal handlers.

### How will you manipulate signal masks to protect critical sections?

- We block the handled signals in the main thread before creating worker threads so they inherit the blocked mask. The signal thread (created after) calls `sigwait()` on the same set.
- Because signals are handled in a controlled thread, we don't need to block/unblock signals around normal critical sections; mutual exclusion is handled by mutexes. If a rare critical section must avoid interruption by signals, you can temporarily block signals with `pthread_sigmask` around that region — but with the `sigwait` approach this is not typically required.

### What precautions will you take to ensure signal safety in your handlers?

- Do **not** perform non-async-signal-safe operations from a traditional signal handler (no `malloc`, `printf`, `fopen`, etc.). Instead, use `sigwait` in a thread so you can call normal functions safely.
- The `sigwait` thread performs `dump_metrics_to_file()` and `reload_config()` — these functions use regular library calls and are safe because they run in the normal thread context (not in an async signal handler).

### How will you handle signals in a multi-threaded environment?

- Block signals in all threads (inherit blocked mask), use a single dedicated thread to `sigwait` and react. This is the recommended POSIX pattern for safe signal handling in multi-threaded programs.

---

## Task 3 — I/O Multiplexing for Log Monitoring

### Which mechanism will you use for I/O multiplexing, and why?

- The implementation uses `poll()` to monitor multiple log file descriptors. Rationale:
  - `poll()` is portable and avoids `FD_SETSIZE` limits of `select()`.
  - It scales reasonably for the expected number of log files in this assignment.
  - `inotify` could be more efficient on Linux for file change notifications; however `poll()` + non-blocking reads is portable and simpler to reason about for the assignment requirement.

### How will you manage log rotation and ensure monitoring continues correctly?

- On initial open we `lseek(fd, 0, SEEK_END)` so the monitor only reads new appended data.
- We store the file's inode when opened. Periodically (each poll loop) we `stat(path)` and compare the on-disk inode with the open fd's inode:
  - If the inode changed (common with `logrotate` rename/create), we close the old fd and reopen the path, updating the stored inode.
  - If truncation occurs but the inode is unchanged (e.g., a `truncate` to zero), the code should either `lseek(fd, 0, SEEK_SET)` or reopen; reopen-on-inode-change covers the most common `logrotate` behavior where a new inode is created. (The implementation can be extended to detect size decreases and `lseek` to the new end.)
- These steps ensure the log monitor follows the active log file through rotation and truncation.

### File descriptor management

- Keep arrays mapping configured filenames → open fd → inode.
- Use `O_NONBLOCK` when opening files to avoid blocking reads.
- Close and reopen fds on errors or rotation. Limit number of fds to `MAX_LOGFILES`.
- Handle `poll()` returning `EINTR` by continuing the loop.

---

## Task 4 — Shell wrapper capabilities

- `syswatch_ctl.sh` demonstrates:
  - `getopts`-based argument parsing.
  - Use of arrays to manage server lists (`SERVERS`).
  - Remote execution with SSH including error handling and a ConnectTimeout.
  - A simple `insert` path that retrieves the JSON snapshot from the local TCP endpoint and inserts a record into MySQL using the `mysql` CLI (parsing falls back to `jq` if available).
  - Use of `trap` to catch `INT`/`TERM` for cleanup.
  - Basic start/stop/status semantics for local management and a `remote-start`/`remote-stop` that illustrate how one would manage multiple agents.
- Notes / practical caveats:
  - `syswatch_ctl.sh` assumes `nc` (netcat) is available for status/insert operations — on Debian/Raspbian install `netcat-openbsd`.
  - For non-interactive remote control, SSH key-based auth (and `-i` identity option) is recommended.
  - The `insert` functionality assumes a reachable MySQL instance and a correctly formatted DSN; it's a demonstration of integration rather than a production-ready DB ingestion pipeline.

---
