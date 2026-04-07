# Systemd Setup for Continuous Fuzzing

Install the timer and service as a **user** systemd unit:

```sh
# Copy units to user systemd directory
mkdir -p ~/.config/systemd/user/
cp tools/fuzzing/systemd/donner-fuzz.{service,timer} ~/.config/systemd/user/

# Edit the service file if paths differ from defaults
# (default assumes repo at /home/jwm/Projects/donner)

# Reload, enable, and start
systemctl --user daemon-reload
systemctl --user enable donner-fuzz.timer
systemctl --user start donner-fuzz.timer

# Verify it's scheduled
systemctl --user list-timers donner-fuzz.timer
```

## Manual trigger

```sh
# Run immediately (bypasses timer)
systemctl --user start donner-fuzz.service

# Or invoke the script directly
tools/fuzzing/trigger_fuzz.sh --force
```

## Monitoring

```sh
# Check timer status
systemctl --user status donner-fuzz.timer

# Check last run
systemctl --user status donner-fuzz.service

# View logs
journalctl --user -u donner-fuzz.service -f

# View trigger logs
ls ~/.donner-fuzz/trigger-logs/
```

## Configuration

Override defaults via environment variables in the service file
(`Environment=` directive) or in `~/.donner-fuzz/config.json`:

| Variable | Default | Description |
|---|---|---|
| `FUZZ_MIN_INTERVAL` | 7200 (2h) | Minimum seconds between runs |
| `FUZZ_WORKERS` | 8 | Parallel fuzzer count |
| `FUZZ_FUZZER_TIME` | 900 (15m) | Max time per fuzzer |
| `FUZZ_PLATEAU` | 120 (2m) | Coverage plateau timeout |
| `FUZZ_MAX_TOTAL` | 3600 (1h) | Max total run time |
| `FUZZ_QUIET_MODE` | reduce | `reduce`, `skip`, or `ignore` |
| `FUZZ_QUIET_WORKERS` | 2 | Worker count in quiet mode |
| `FUZZ_QUIET_MAX_TOTAL` | 1800 (30m) | Max total time in quiet mode |
| `FUZZ_STEAL_THRESHOLD` | 10 | CPU steal % to trigger quiet mode |
| `FUZZ_LOAD_THRESHOLD` | 0 (off) | Load average to trigger quiet mode |

## Quiet hours (shared host detection)

When running on a shared host (physical or VM), the trigger automatically
detects contention and throttles fuzzing:

1. **CPU steal time** (primary signal for VMs) — when the hypervisor gives
   CPU to other VMs or host processes, steal time rises. If steal exceeds
   `FUZZ_STEAL_THRESHOLD` (default: 10%), quiet mode activates.
2. **Logged-in users** — if other users are SSH'd into the VM.
3. **Load average** — if `FUZZ_LOAD_THRESHOLD` is set and exceeded.

In quiet mode, behavior depends on `FUZZ_QUIET_MODE`:
- `reduce` (default): run with fewer workers and shorter time limit
- `skip`: don't fuzz at all, wait for the next timer tick
- `ignore`: always run at full speed regardless
