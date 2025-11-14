# NVIDIA Fan Control V2 Tooling

This repository ships an NVML-based fan control daemon, a curses configurator,
installer scripts, and a helper CLI. The v3 runtime refactor loads fan curves
from an INI file at start-up and on `SIGHUP`, eliminating the old compile-time
configuration step.

## Features
- Reloadable runtime configuration with clamping and validation.
- Low-overhead fan control loop with adaptive sleeps and light hysteresis.
- Full-screen TUI for editing 3–7 curve points plus runtime options.
- Installer and CLI wrappers for systemd-based deployments.
- Mock NVML backend and automated tests for hardware-free verification.

## Repository Layout
```
.
├── bin/                      # CLI wrapper installed to /usr/local/bin/
├── build/                    # Generated binaries (ignored until built)
├── docs/                     # Additional documentation
├── installer.sh              # Install script (runs TUI if config missing)
├── mock_nvml/                # Optional mock NVML headers + shared library
├── src/                      # Daemon and support modules
├── systemd/                  # Service unit file
├── tests/                    # Automated test scripts & configs
├── tui/nvfc_tui.py           # curses-based configurator
└── uninstall.sh              # Removes installed files and service
```

## Requirements
- Python 3.8+
- `gcc` and the NVIDIA NVML development libraries (`libnvidia-ml.so`, `nvml.h`)
  when building for real hardware
- `make`
- Root access for installation (the script will re-invoke via `sudo` when needed)

## Quick Start – Real Hardware
1. Ensure the NVIDIA driver stack is installed so that `libnvidia-ml.so` and
   `nvml.h` are available to the compiler.
2. Run the installer (it elevates with `sudo` if necessary):
   ```sh
   ./installer.sh
   ```
   - If `/etc/nvidia-fan-controlV2.conf` does not exist, the curses TUI opens so
     you can create or edit the curve before the daemon is built.
   - The installer now calls `make build` to compile `src/` into
     `build/nvidia_fan_controlV2d`, then installs:
     - `/usr/local/sbin/nvidia_fan_controlV2d`
     - `/usr/local/bin/nvidia_fan_controlV2`
     - `/etc/nvidia-fan-controlV2.conf`
     - `/etc/systemd/system/nvidia-fan-controlV2.service`
3. Reload systemd (handled automatically when available) and enable the service:
   ```sh
   sudo systemctl enable --now nvidia-fan-controlV2
   ```
4. View live logs:
   ```sh
   nvidia_fan_controlV2
   ```

### Reconfiguring at Runtime
- Launch the TUI and reload without rebuilding:
  ```sh
  sudo nvidia_fan_controlV2 --re-configure
  ```
- Apply changes from an already edited config file:
  ```sh
  sudo nvidia_fan_controlV2 --apply
  ```
- Restart or inspect the service as needed:
  ```sh
  nvidia_fan_controlV2 --restart
  nvidia_fan_controlV2 --status
  ```

## Testing Without Hardware (Mock NVML)
A mock NVML implementation in `mock_nvml/` lets you exercise the daemon and
scripts end-to-end.

- Build and run the automated suite:
  ```sh
  USE_MOCK_NVML=1 make test
  ```
- The mock library logs fan updates under `${NVML_MOCK_DIR:-/tmp/nvml-mock}/fan_speed.log`
  for manual inspection.

### Manual Reload / Reconfigure Drill
You can confirm that editing the config file and signalling the daemon applies
changes without a rebuild by running the helper script:

```sh
./scripts/mock_reconfigure_drill.sh
```

The script performs the following steps:

- Builds the daemon against the mock NVML library (`USE_MOCK_NVML=1 make build`).
- Creates an isolated scratch directory containing the default six-point curve
  from `tests/reload_initial.conf` (the same values the daemon falls back to
  when parsing fails).
- Starts the daemon in the background with stdout/err captured to
  `$TMPDIR/daemon.log` and writes an environment helper to `$TMPDIR/env.sh` so
  additional terminals can join the session (`source "$TMPDIR/env.sh"`).

Once the script prints the session banner, you can:

1. Launch the TUI against the generated config:
   ```sh
   source "$TMPDIR/env.sh"
   python3 tui/nvfc_tui.py --config "$CONFIG_PATH"
   ```
   Edit a point and save to write the updated curve.
2. Drive the mock daemon while the TUI is open:
   ```sh
   mock_nvml/mockctl.sh set-temp 55
   mock_nvml/mockctl.sh tail-log
   ```
3. Signal the running daemon so it reloads the updated curve:
   ```sh
   kill -HUP "$DAEMON_PID"
   ```
4. Inspect `$TMPDIR/daemon.log` or the mock `fan_speed.log` for the
   `Reloaded config` message and new speeds. When finished, stop the process and
   delete the scratch directory:
   ```sh
   kill "$DAEMON_PID"
   rm -rf "$TMPDIR"
   ```

On a real install the helper CLI performs the same steps when you run
`sudo nvidia_fan_controlV2 --re-configure` (it saves via the TUI and issues
`systemctl reload`).

## Wrapper CLI Reference
`nvidia_fan_controlV2` accepts the following options:

| Command | Description |
|---------|-------------|
| _no args_ | Tail journal logs (`journalctl -u nvidia-fan-controlV2 -f -o cat`). |
| `--re-configure` | Launch the TUI, save the config, and reload the service. |
| `--apply` | Reload the service configuration without opening the TUI. |
| `--restart` | Restart the systemd service. |
| `--status` | Show `systemctl status` output. |

## Configuration File
The daemon reads `/etc/nvidia-fan-controlV2.conf` (or the path supplied via
`--config`/`NVFC_CONFIG_PATH`) on start-up and when it receives `SIGHUP`.
Example:
```
[curve]
temps = 0,40,55,67,75,85
speeds = 25,30,45,65,78,99

[run]
gpu_index = 0
sleep_low = 5
sleep_high = 2
high_temp_threshold = 42
```

Fan curves are clamped to 0–120 °C and 0–100 % during parsing. Temperatures must
remain strictly increasing, otherwise the daemon falls back to defaults while
preserving the runtime options from `[run]`.

## Additional Notes
- Runtime changes and smoothing behaviour are summarised in
  `docs/CHANGES_RUNTIME_CONFIG.md`.
- To uninstall everything:
  ```sh
  sudo ./uninstall.sh
  ```

