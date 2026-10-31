# Changelog

## 1.0.0
First release under one version number (previously a mix of "V0", "v3" and "V2").

**New**
- **Smart Smooth** (on by default, one switch in the TUI/config). Draws straight
  lines between your curve points instead of a staircase, filters the
  temperature (rises followed fast, falls slowly), ignores speed changes under a
  deadband, and ramps down in small held steps. Result: far fewer fan writes and
  no flapping around a steep point. Bypassed at `emergency_temp`.
- **Adaptive polling** (`sleep_max`): the poll interval doubles while the GPU is
  cool and quiet, and snaps back to fast on any 2 °C move or above
  `high_temp_threshold`.
- `nvfc` command (installed to `/usr/local/bin`), live status (`nvfc status`
  and a live marker in the TUI), `--version` everywhere.
- NVML is loaded at runtime (`dlopen`); building no longer needs `nvml.h`.
- One-step `installer.sh` that builds, installs, enables and starts the service,
  and migrates old "V2" installs (config kept).

**Fixed**
- Classic mode's minimum interval between fan changes never applied (the
  skip test used `&&`, so any 1 °C change forced a write). It is enforced now.
- A single failed temperature read no longer hands the fan back to auto; it is
  retried first.
- `uninstall.sh` keeps your config unless `--purge` is given.
- TUI temperature limit now matches the daemon (120 °C).

**Renamed** (installer migrates automatically)

| Before | Now |
|---|---|
| `nvidia_fan_controlV2` (CLI) | `nvfc` (alias `nvidia-fan-control`) |
| `nvidia_fan_controlV2d` | `nvidia-fan-controld` |
| `nvidia-fan-controlV2.service` | `nvidia-fan-control.service` |
| `/etc/nvidia-fan-controlV2.conf` | `/etc/nvidia-fan-control.conf` |

**Removed**: leftover scripts from the old compile-time configuration.
