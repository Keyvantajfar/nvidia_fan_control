# Changelog

## 1.1.0
**New**
- **Profiles.** Five slots: **Quiet**, **Balanced**, **Performance** (built in,
  read-only) and **custom1** / **custom2** (yours; give them any name). Each
  profile is a curve plus its Smart Smooth settings; GPU and polling settings stay
  shared. Quiet ramps down quickly, Balanced uses the balanced level, Performance
  holds speed longest (`slow`). Switch with `nvfc profile <name>` (also accepts
  `cool` and a custom profile's name) or `P` in the configurator, which also gets
  `C` (copy a profile into a custom slot) and `N` (rename). `nvfc status` and the
  status file show the active profile. The configurator's old `T` presets are
  replaced by the built-in profiles + copy.
- **Easy updates.** `nvfc update` downloads the newest release and installs it
  (`--check`, `--yes`, `--force`, `--channel nightly`, `--from <folder|.tar.gz>`).
  `./installer.sh` is now an incremental updater too: it replaces only what
  changed and restarts the service only when the daemon, its unit or the config
  format changed, so a configurator-only release never touches the running fan
  control. Nothing to uninstall first; your config is kept.
- **Config schema versions.** The config file records `[nvfc] config_version`.
  Older files are upgraded in place when you update (the previous file is kept as
  `<config>.v1.bak`), and future releases can chain further upgrades.

**Compatibility**
- Configs from every earlier release keep working untouched: the daemon reads
  them as before, and updating or saving moves your old curve and Smart Smooth
  settings into profile **custom1** ("Previous config"), kept active, so the fan
  behaves exactly as it did.
- Downgrading to 1.0.x after the config was upgraded: restore
  `/etc/nvidia-fan-control.conf.v1.bak`.

**Changed**
- `./installer.sh` only needs gcc/make when the daemon has to be rebuilt.
- `uninstall.sh --purge` also removes the `.vN.bak` backups.
- Tests run through `bash` (no exec-bit needed).

## 1.0.2
**Fixed**
- The configurator (TUI) no longer flickers/refreshes while you look at it. It
  repainted the whole screen every second to update the live marker; it now
  redraws only when a key is pressed or the live temperature/fan values change,
  and no longer forces a full-terminal repaint.

## 1.0.1
First release under one version number (previously a mix of "V0", "v3" and "V2").

**New**
- **Smart Smooth** (on by default, one switch in the TUI/config). Draws straight
  lines between your curve points instead of a staircase, filters the
  temperature (rises followed fast, falls slowly), ignores speed changes under a
  deadband, and ramps down in held steps. `ramp_down = quick | balanced | slow`
  sets how fast the fan comes down after a sudden cool-down (default `balanced`;
  `slow` is the original gentle behaviour). Numbers can still be set directly with
  `down_hold`, `down_step` and `down_interval`. Result: far fewer fan writes and
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
