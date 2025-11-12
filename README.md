# NVIDIA Fan Control V2 Tooling

This repository packages a curses-based configurator, installer scripts, and a
wrapper CLI around the existing `nvidia_fan_controlV2.c` daemon.  The Python
utilities let you edit the fan curve in a full-screen terminal UI, compile the
C daemon with the saved settings, and manage the accompanying systemd service.

## Features
- Full-screen TUI for editing 3–7 fan curve points with presets and validation.
- Config-driven build step that generates `build/fan_curve_config.h` and compiles
  the daemon without modifying the original C source.
- Installer/uninstaller scripts that deploy the daemon, CLI wrapper, config,
  and service unit into system locations.
- Wrapper CLI (`nvidia_fan_controlV2`) for tailing logs, restarting the service,
  re-running the TUI, or rebuilding from an existing configuration.
- Optional mock NVML backend and smoke test for contributors without NVIDIA
  hardware.

## Repository Layout
```
.
├── bin/                  # CLI wrapper installed to /usr/local/bin/
├── installer.sh          # Main install script (runs TUI, builds, installs)
├── uninstall.sh          # Removes installed files and service
├── scripts/
│   ├── gen_header_from_conf.py   # Writes build/fan_curve_config.h from config
│   ├── patch_source_for_build.py # Copies + injects header include in C source
│   └── smoke_test_mock.sh        # Mock NVML end-to-end smoke test
├── systemd/nvidia-fan-controlV2.service
├── tui/nvfc_tui.py       # curses-based configurator
├── mock_nvml/            # Optional mock NVML headers + library
└── nvidia_fan_controlV2.c # Upstream daemon source (never modified in-place)
```

## Requirements
- Python 3.8+
- `gcc` and the NVIDIA NVML development libraries (`libnvidia-ml.so` and
  `nvml.h`) when building for real hardware
- `make` (only required when building the mock NVML library)
- Root access for installation (scripts will re-invoke with `sudo` when needed)

## Quick Start – Real Hardware
1. Ensure the NVIDIA driver stack is installed so that
   `/usr/lib/x86_64-linux-gnu/libnvidia-ml.so` (or similar) and `nvml.h` are
   available to the compiler.
2. Run the installer (it will elevate with `sudo` if necessary):
   ```sh
   ./installer.sh
   ```
   - If `/etc/nvidia-fan-controlV2.conf` does not exist, the curses TUI opens so
     you can edit the curve before building.
   - The installer generates the configuration header, compiles the daemon, and
     installs:
     - `/usr/local/sbin/nvidia_fan_controlV2d`
     - `/usr/local/bin/nvidia_fan_controlV2`
     - `/etc/nvidia-fan-controlV2.conf`
     - `/etc/systemd/system/nvidia-fan-controlV2.service`
3. Reload systemd (done automatically when available) and enable the service:
   ```sh
   sudo systemctl enable --now nvidia-fan-controlV2
   ```
4. View live logs:
   ```sh
   nvidia_fan_controlV2
   ```

### Reconfiguring / Rebuilding on Real Hardware
- To reopen the TUI, rebuild the daemon, and restart the service:
  ```sh
  sudo nvidia_fan_controlV2 --re-configure
  ```
- If you edit `/etc/nvidia-fan-controlV2.conf` by hand, rebuild and restart with:
  ```sh
  sudo nvidia_fan_controlV2 --apply
  ```
- Check service status:
  ```sh
  nvidia_fan_controlV2 --status
  ```

## Testing Without Hardware (Mock NVML)
A mock NVML implementation in `mock_nvml/` lets you exercise the tooling
end-to-end.

### Automated Smoke Test
Run the scripted smoke test (invokes the mock, builds the daemon, drives
temperature changes, and validates that speeds increase):
```sh
USE_MOCK_NVML=1 scripts/smoke_test_mock.sh
```
The script leaves its build artifacts under `build/`.

### Manual Mock Workflow
1. Build the mock library (if not already built):
   ```sh
   make -C mock_nvml
   ```
2. Launch the TUI against a temporary config and build in-place:
   ```sh
   NVFC_LIBEXEC_DIR=$(pwd) USE_MOCK_NVML=1 \
   sudo -E ./installer.sh
   ```
   - Setting `NVFC_LIBEXEC_DIR` points the CLI to the repo copy of helper
     scripts when testing without installing into `/usr/local/lib`.
3. Run the daemon manually (optional):
   ```sh
   LD_LIBRARY_PATH=./mock_nvml USE_MOCK_NVML=1 build/nvidia_fan_controlV2d
   ```
4. Drive temperatures from another shell:
   ```sh
   NVML_MOCK_DIR=/tmp/nvml-mock ./mock_nvml/mockctl.sh set-temp 40
   NVML_MOCK_DIR=/tmp/nvml-mock ./mock_nvml/mockctl.sh set-temp 70
   NVML_MOCK_DIR=/tmp/nvml-mock ./mock_nvml/mockctl.sh tail-log
   ```

## Wrapper CLI Reference
`nvidia_fan_controlV2` accepts the following options (run with `sudo` for
operations that rebuild or restart the service):

| Command | Description |
|---------|-------------|
| _no args_ | Tail journal logs (`journalctl -u nvidia-fan-controlV2 -f -o cat`). |
| `--re-configure` | Launch TUI, rebuild with current config, restart service. |
| `--apply` | Rebuild from existing config and restart service. |
| `--restart` | Restart the systemd service. |
| `--status` | Show `systemctl status` output. |

The CLI automatically respects `USE_MOCK_NVML=1` when set (exported during
rebuilds so the daemon links against the mock library).

## Configuration File
The installer/TUI writes `/etc/nvidia-fan-controlV2.conf` in INI format:
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
Edit this file carefully: temperature points must be strictly increasing within
0–100°C, and speeds must stay within 0–100%.

## Uninstall
To remove the deployed files and service:
```sh
sudo ./uninstall.sh
```
The script stops/ disables the service, deletes installed binaries, removes the
configuration, and reloads systemd.

## Troubleshooting
- **Compiler cannot find NVML headers/libraries** – Install the NVIDIA driver
  development packages or add the appropriate `-I`/`-L` paths in
  `installer.sh`/`bin/nvidia_fan_controlV2` if your system uses non-standard
  locations.
- **Service fails to start after reconfiguration** – Check the journal with
  `nvidia_fan_controlV2`, inspect `/etc/nvidia-fan-controlV2.conf` for invalid
  values, and rerun the TUI to correct them.
- **Testing without installation** – Export `NVFC_LIBEXEC_DIR=$(pwd)` so the CLI
  resolves helper scripts from the working tree instead of `/usr/local/lib`.
