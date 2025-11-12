#!/bin/bash
# syswatch_ctl.sh
#
# Usage:
#   ./syswatch_ctl.sh -c ./syswatch.cfg -s start
#   ./syswatch_ctl.sh -s status
#   ./syswatch_ctl.sh -r "host1 host2" -s remote-start
#   ./syswatch_ctl.sh -d 'user:pass@tcp(127.0.0.1:3306)/metricsdb' -i ~/.ssh/id_rsa -s insert

CONFIG="./syswatch.cfg"
ACTION=""
SERVERS=()
MYSQL_DSN=""
LOCAL_BINARY="./syswatch"
PORT=9999
SSH_IDENTITY=""

trap_clean() {
    echo "Caught trap, cleaning up..."
    exit 0
}
trap "trap_clean" INT TERM

usage() {
    cat <<EOF
Usage: $0 [-c config] [-p port] [-r "host1 host2"] [-d mysql_dsn] [-i identity_file] -s <action>

Actions (use -s):
  start           start local syswatch (background)
  stop            stop local syswatch (SIGTERM)
  status          query local syswatch status via TCP
  remote-start    start syswatch on remote servers (via ssh)
  remote-stop     stop syswatch on remote servers (via ssh)
  insert          insert last metrics snapshot into MySQL

Options:
  -c config file (default ./syswatch.cfg)
  -p port (default 9999)
  -r "host1 host2" list of remote hosts (space-separated)
  -d mysql_dsn connection string (user:pass@tcp(host:port)/db)
  -i ssh identity file for remote ssh (optional)
EOF
}

while getopts "c:p:r:d:s:hi:" opt; do
  case ${opt} in
    c) CONFIG="$OPTARG" ;;
    p) PORT="$OPTARG" ;;
    r) IFS=' ' read -r -a SERVERS <<< "$OPTARG" ;;
    d) MYSQL_DSN="$OPTARG" ;;
    s) ACTION="$OPTARG" ;;
    i) SSH_IDENTITY="$OPTARG" ;;
    h) usage; exit 0 ;;
    *) usage; exit 1 ;;
  esac
done

if [ -z "$ACTION" ]; then usage; exit 1; fi

# check availability of nc for local status/insert
have_nc() {
  command -v nc >/dev/null 2>&1
}

start_local() {
    if pgrep -f "$LOCAL_BINARY" >/dev/null; then
        echo "syswatch already running"
        return 0
    fi
    nohup "$LOCAL_BINARY" -c "$CONFIG" > syswatch.out 2>&1 &
    sleep 1
    echo "started syswatch (pid $!)"
}

stop_local() {
    pkill -f "$LOCAL_BINARY" || true
    echo "sent termination to syswatch"
}

status_local() {
    if ! have_nc; then
        echo "Error: netcat (nc) not found. Install netcat-openbsd." >&2
        return 2
    fi
    if ! nc -z localhost "$PORT" 2>/dev/null; then
        echo "syswatch not reachable on port $PORT"
        return 1
    fi
    printf "" | nc localhost "$PORT" | sed -n '1,200p'
}

remote_exec() {
    local host="$1"
    local cmd="$2"
    local ssh_cmd=(ssh -o BatchMode=yes -o ConnectTimeout=5)
    if [ -n "$SSH_IDENTITY" ]; then
        ssh_cmd+=(-i "$SSH_IDENTITY")
    fi
    ssh_cmd+=("$host" "$cmd")
    "${ssh_cmd[@]}"
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "Remote command failed on $host (rc=$rc)" >&2
    fi
    return $rc
}

remote_start() {
    if [ "${#SERVERS[@]}" -eq 0 ]; then
        echo "No servers specified (-r)" >&2; exit 1
    fi
    for h in "${SERVERS[@]}"; do
        echo "Starting on $h..."
        remote_exec "$h" "nohup $LOCAL_BINARY -c $CONFIG > /tmp/syswatch.out 2>&1 &"
    done
}

remote_stop() {
    if [ "${#SERVERS[@]}" -eq 0 ]; then
        echo "No servers specified (-r)" >&2; exit 1
    fi
    for h in "${SERVERS[@]}"; do
        echo "Stopping on $h..."
        remote_exec "$h" "pkill -f $LOCAL_BINARY || true"
    done
}

insert_mysql() {
    if [ -z "$MYSQL_DSN" ]; then echo "No MySQL DSN specified (-d)"; exit 1; fi
    if ! have_nc; then
        echo "Error: netcat (nc) not found. Install netcat-openbsd." >&2
        exit 2
    fi
    SNAP=$(printf "" | nc localhost "$PORT")
    if [ -z "$SNAP" ]; then echo "No snapshot"; exit 1; fi

    # If jq is available, use it (safer). Otherwise fall back to regex extraction.
    if command -v jq >/dev/null 2>&1; then
        CPU=$(printf "%s" "$SNAP" | jq -r '.current.cpu')
        MEM=$(printf "%s" "$SNAP" | jq -r '.current.memory')
        DISK=$(printf "%s" "$SNAP" | jq -r '.current.disk')
    else
        CPU=$(printf "%s" "$SNAP" | sed -n 's/.*"cpu":\s*\([0-9.]*\).*/\1/p' | head -n1)
        MEM=$(printf "%s" "$SNAP" | sed -n 's/.*"memory":\s*\([0-9.]*\).*/\1/p' | head -n1)
        DISK=$(printf "%s" "$SNAP" | sed -n 's/.*"disk":\s*\([0-9.]*\).*/\1/p' | head -n1)
    fi

    if [ -z "$CPU" ]; then echo "Failed to parse metrics"; exit 1; fi

    # Build minimal SQL insert
    SQL="CREATE TABLE IF NOT EXISTS metrics (id INT AUTO_INCREMENT PRIMARY KEY, ts DATETIME, cpu DOUBLE, mem DOUBLE, disk DOUBLE); INSERT INTO metrics (ts,cpu,mem,disk) VALUES (NOW(), $CPU, $MEM, $DISK);"

    # Parse DSN: user:pass@tcp(host:port)/db
    USERPASS=$(echo "$MYSQL_DSN" | awk -F'@' '{print $1}')
    DBURL=$(echo "$MYSQL_DSN" | awk -F'@' '{print $2}')
    USER=$(echo "$USERPASS" | awk -F':' '{print $1}')
    PASS=$(echo "$USERPASS" | awk -F':' '{print $2}')
    HOSTPORT=$(echo "$DBURL" | sed 's/tcp(//; s/)//; s/\// /; s/)//g' | awk '{print $1}')
    DBNAME=$(echo "$DBURL" | awk -F'/' '{print $2}')
    mysql -u"$USER" -p"$PASS" -h "${HOSTPORT%%:*}" -P "${HOSTPORT##*:}" "$DBNAME" -e "$SQL"
}

case "$ACTION" in
    start) start_local ;;
    stop) stop_local ;;
    status) status_local ;;
    remote-start) remote_start ;;
    remote-stop) remote_stop ;;
    insert) insert_mysql ;;
    *) echo "Unknown action"; usage; exit 1 ;;
esac