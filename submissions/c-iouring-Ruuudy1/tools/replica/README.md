# A replica of the benchmark box (rented, a few hours at a time)

Everything up to now ran on one development machine (WSL2 on a laptop). That is enough for correctness and for
relative comparisons, but the score depends on the box's CPUs, its virtual NIC and separate load machines. This
kit rebuilds the maintainer's setup on DigitalOcean so that `loadgen search` can run exactly as the maintainer
runs it.

## What to rent

| Role | Droplet | Approx. price |
|---|---|---|
| Target | Premium AMD, 8 vCPU / 16 GB (`s-8vcpu-16gb-amd`), Ubuntu 24.04 | about $0.17/h |
| Load boxes (3) | CPU-Optimized 8 vCPU (`c-8`), Ubuntu 24.04, same region and VPC | about $0.25/h each |

About $0.92/h for all four. Setup takes about 30 minutes (most of it the full seed) and a full search about an
hour, so one complete run costs roughly $2. Destroy the droplets afterwards: they bill while they exist, running
or not. Prices are third-party listings as of 2026-10; check DigitalOcean's pricing page.

## Steps

1. Create the four droplets with your SSH key, in one VPC. Note the private IPs.
2. Copy this repository (with the submission) to each droplet, e.g. `rsync -a --exclude .git challenge/ root@<ip>:challenge/`.
3. On the target: `bash challenge/submissions/c-iouring-Ruuudy1/tools/replica/target-setup.sh`.
   It runs `box/setup-box.sh`, builds the full seed, builds the server as user `challenge`, installs
   `/opt/conduit-tools/switch.sh` and `monitor.sh`, stops Nginx and starts the service on port 80.
4. On each load box: `bash challenge/submissions/c-iouring-Ruuudy1/tools/replica/loadbox-setup.sh`.
5. From the first load box, make sure `ssh root@<target>` and `ssh root@<other load boxes>` work without a
   password (copy the first box's key to the others), then:
   `bash challenge/submissions/c-iouring-Ruuudy1/tools/replica/run-search.sh <target ip> <load box 2 ip> <load box 3 ip>`
6. `results/.../SUMMARY.txt` has the score; `merged.json` and `monitor.csv` per phase have the details.

For A/B work (MULTICORE_BUILD_SPEC.md section 8), copy a variant to `/opt/challenge/<name>`, build it as
`challenge`, and run `loadgen search -lang <name>` (or `loadgen run` at a fixed user count) per variant, in ABBA
order, with `switch.sh` giving each trial a fresh database.
