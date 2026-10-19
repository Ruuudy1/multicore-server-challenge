#!/usr/bin/env python3
# Local load test on one machine, as close to the 8-vCPU box as one machine gets: Postgres and the server share
# CPUs 0-7 (Postgres through its systemd cgroup, the server through affinity), the load generator gets the rest,
# and think times are scaled down so a few thousand users make a box-sized load. Each trial starts from a fresh
# copy of the seed. Reports req/s, latency percentiles, errors and CPU microseconds per request (server,
# Postgres, and both together), the spec's primary metric.
#
#   python3 tools/localbench.py --repo ~/mc --server c --users 6000 [--think 0.1] [--measure 60] [--warmup 30]
#   python3 tools/localbench.py --repo ~/mc --server go --users 4000
#   python3 tools/localbench.py --repo ~/mc --server cmd --cmd "/path/to/start.sh" --users 6000
#
# One-time setup as root: systemctl set-property --runtime postgresql@18-main.service AllowedCPUs=0-7
import argparse
import json
import os
import shlex
import signal
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SUB = os.path.dirname(HERE)
PG_CG = "/sys/fs/cgroup/system.slice/system-postgresql.slice/postgresql@18-main.service/cpu.stat"
TICK = os.sysconf("SC_CLK_TCK")
SECRET = "multicore-server-challenge"


def psql(sql, db="postgres"):
    subprocess.run(["psql", "-X", "-q", "-v", "ON_ERROR_STOP=1", "-d", db, "-c", sql], check=True)


def proc_cpu_us(pid):
    # (utime, stime) of the whole process (all threads, and children for a start.sh wrapper), in microseconds
    total_u = total_s = 0
    pids = [pid]
    try:
        out = subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True).stdout.split()
        pids += [int(p) for p in out]
    except OSError:
        pass
    for p in pids:
        try:
            with open(f"/proc/{p}/stat") as f:
                parts = f.read().rsplit(")", 1)[1].split()
            total_u += int(parts[11]) * 1_000_000 // TICK
            total_s += int(parts[12]) * 1_000_000 // TICK
        except OSError:
            pass
    return total_u, total_s


def pg_cpu_us():
    try:
        with open(PG_CG) as f:
            for line in f:
                if line.startswith("usage_usec"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def box_cpu():
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:]]
    total = sum(v[:8])
    idle = v[3] + v[4]
    return total, idle


class Sampler(threading.Thread):
    def __init__(self, pid):
        super().__init__(daemon=True)
        self.pid, self.samples, self.stop = pid, [], False

    def run(self):
        while not self.stop:
            t, i = box_cpu()
            u, s = proc_cpu_us(self.pid)
            self.samples.append((time.time(), u + s, pg_cpu_us(), t, i, s))
            time.sleep(0.5)

    def window(self, t0, t1):
        inside = [s for s in self.samples if t0 <= s[0] <= t1]
        if len(inside) < 2:
            return None
        a, b = inside[0], inside[-1]
        secs = b[0] - a[0]
        busy = (b[3] - a[3]) - (b[4] - a[4])
        return {"secs": secs, "app_us": b[1] - a[1], "pg_us": b[2] - a[2], "app_sys_us": b[5] - a[5],
                "box_busy_pct": 100.0 * busy / max(b[3] - a[3], 1)}


def wait_healthy(port, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/tags", timeout=2) as r:
                if r.status == 200:
                    return True
        except OSError:
            time.sleep(0.3)
    return False


def loadgen(args, users, ramp, measure, run_id, out, think):
    cmd = ["taskset", "-c", args.lg_cpus, args.loadgen, "run", "-target", f"http://127.0.0.1:{args.port}", "-users", str(users),
           "-ramp", f"{ramp}s", "-measure", f"{measure}s", "-tail", "5s", "-secret", SECRET, "-articles", "500000",
           "-think-scale", str(think), "-run-id", run_id, "-out", out, "-q"]
    start = time.time() + 2
    cmd += ["-start-at", str(int(start * 1000))]
    subprocess.run(cmd, check=True)
    return start


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--server", choices=["c", "go", "cmd"], default="c")
    ap.add_argument("--cmd", help="start command for --server cmd")
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for the server")
    ap.add_argument("--users", type=int, required=True)
    ap.add_argument("--think", type=float, default=0.1)
    ap.add_argument("--ramp", type=int, default=20)
    ap.add_argument("--measure", type=int, default=60)
    ap.add_argument("--warmup", type=int, default=30)
    ap.add_argument("--port", type=int, default=3110)
    ap.add_argument("--cpus", default="0-7", help="CPUs for the server (Postgres is confined separately)")
    ap.add_argument("--lg-cpus", default="8-21")
    ap.add_argument("--loadgen", default=os.path.expanduser("~/loadgen"))
    ap.add_argument("--template", default="conduit_seed")
    ap.add_argument("--keep-db", action="store_true")
    ap.add_argument("--db", help="use this existing database (no copy, kept afterwards): an aged database for A/B work")
    ap.add_argument("--pgss", action="store_true", help="profile statements with pg_stat_statements (must be preloaded)")
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    if args.db:
        db = args.db
        args.keep_db = True
    else:
        db = f"bench_{os.getpid()}"
        psql(f"CREATE DATABASE {db} TEMPLATE {args.template} STRATEGY FILE_COPY")
    if args.pgss:
        psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements", db)
    env = dict(os.environ, DATABASE_URL=f"postgres://conduit:conduit@127.0.0.1:5432/{db}", JWT_SECRET=SECRET,
               HOST="127.0.0.1", PORT=str(args.port))
    for kv in args.env:
        k, v = kv.split("=", 1)
        env[k] = v
    if args.server == "c":
        cmd = [os.path.join(SUB, "bin", "conduit")]
    elif args.server == "go":
        env.setdefault("DB_POOL_SIZE", "80")
        cmd = [os.path.join(args.repo, "examples", "go", "bin", "conduit")]
    else:
        cmd = shlex.split(args.cmd)
    log = open(f"/tmp/localbench-{db}.log", "w")
    srv = subprocess.Popen(["taskset", "-c", args.cpus] + cmd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        if not wait_healthy(args.port):
            sys.exit(f"server not healthy; see /tmp/localbench-{db}.log")
        sampler = Sampler(srv.pid)
        sampler.start()
        if args.warmup:
            loadgen(args, max(args.users // 2, 1), 10, args.warmup, f"warm{os.getpid()}t{int(time.time())}", f"/tmp/{db}-warm.json", args.think)
        out = f"/tmp/{db}.json"
        if args.pgss:
            psql("SELECT pg_stat_statements_reset()", db)
        start = loadgen(args, args.users, args.ramp, args.measure, f"run{os.getpid()}t{int(time.time())}", out, args.think)
        sampler.stop = True
        if args.pgss:
            q = ("SELECT calls, round(total_exec_time::numeric, 0) AS total_ms, round(mean_exec_time::numeric * 1000, 1) AS mean_us, "
                 "round(100 * total_exec_time / sum(total_exec_time) OVER (), 1) AS pct, rows, "
                 "shared_blks_hit / greatest(calls, 1) AS blks_per_call, left(regexp_replace(query, '\\s+', ' ', 'g'), 90) AS query "
                 "FROM pg_stat_statements WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database()) "
                 "ORDER BY total_exec_time DESC LIMIT 25")
            subprocess.run(["psql", "-X", "-d", db, "-c", q])
        merged = f"/tmp/{db}-merged.json"
        subprocess.run([args.loadgen, "merge", "-o", merged, out], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(merged) as f:
            m = json.load(f)
        ov = m["overall"]
        w = sampler.window(start + args.ramp, start + args.ramp + args.measure)
        req = ov.get("count", 0)
        rps = ov.get("rps") or (req / args.measure if args.measure else 0)
        line = {
            "label": args.label or args.server, "users": args.users, "think": args.think, "rps": round(rps),
            "p50": ov.get("p50_ms"), "p95": ov.get("p95_ms"), "p99": ov.get("p99_ms"), "errors": ov.get("errors"),
            "err_rate": ov.get("error_rate"), "pass": m["verdict"]["pass"],
        }
        if w and req:
            line.update({"app_us_req": round(w["app_us"] / (rps * w["secs"]), 1), "app_sys_us_req": round(w["app_sys_us"] / (rps * w["secs"]), 1),
                         "pg_us_req": round(w["pg_us"] / (rps * w["secs"]), 1),
                         "server_box_us_req": round((w["app_us"] + w["pg_us"]) / (rps * w["secs"]), 1),
                         "app_cores": round(w["app_us"] / w["secs"] / 1e6, 2), "pg_cores": round(w["pg_us"] / w["secs"] / 1e6, 2),
                         "machine_busy_pct": round(w["box_busy_pct"], 1)})
        print(json.dumps(line))
        with open(os.path.join(HERE, "..", "tests", "bench-results.jsonl"), "a") as f:
            f.write(json.dumps(dict(line, time=time.strftime("%Y-%m-%dT%H:%M:%S"))) + "\n")
    finally:
        os.killpg(srv.pid, signal.SIGTERM)
        try:
            srv.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(srv.pid, signal.SIGKILL)
        if not args.keep_db:
            psql(f"DROP DATABASE IF EXISTS {db} WITH (FORCE)")


if __name__ == "__main__":
    main()
