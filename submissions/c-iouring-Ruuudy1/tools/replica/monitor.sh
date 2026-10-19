#!/usr/bin/env bash
# /opt/conduit-tools/monitor.sh <interval_s> <label>: what `loadgen search` runs on the target during each phase.
# One CSV line per interval: box CPU, steal, and per-service CPU from cgroup v2 (MULTICORE_BUILD_SPEC.md 10.2).
set -u
cg() { find /sys/fs/cgroup -type d -name "$1" | head -1; }
APP=$(cg challenge.service); DB=$(cg 'postgresql@18-main.service'); NG=$(cg nginx.service)
usec() { if [ -n "$1" ] && [ -f "$1/cpu.stat" ]; then awk '/^usage_usec/ {print $2}' "$1/cpu.stat"; else echo 0; fi; }
sample() { read -r _ u n s i w q sq st _ < /proc/stat; echo "$((u+n+s+i+w+q+sq+st)) $((i+w)) $st $(usec "$APP") $(usec "$DB") $(usec "$NG")"; }
echo "time,cpu%,steal%,app%,db%,nginx%" > "$2.csv"
read -ra prev <<< "$(sample)"; wall=$(awk -v i="$1" -v c="$(nproc)" 'BEGIN {print i * c * 1000000}')
while sleep "$1"; do
  read -ra cur <<< "$(sample)"
  awk -v t="$(date +%s)" -v a="${cur[*]}" -v b="${prev[*]}" -v w="$wall" 'BEGIN {
    split(a, x); split(b, y); d = x[1] - y[1]; if (d <= 0) d = 1
    printf "%s,%.1f,%.1f,%.1f,%.1f,%.1f\n", t, 100 * (d - (x[2] - y[2])) / d, 100 * (x[3] - y[3]) / d,
      100 * (x[4] - y[4]) / w, 100 * (x[5] - y[5]) / w, 100 * (x[6] - y[6]) / w }' >> "$2.csv"
  prev=("${cur[@]}")
done
