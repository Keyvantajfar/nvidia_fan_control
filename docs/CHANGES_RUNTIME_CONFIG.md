# NVIDIA Fan Control V3 Runtime Updates

The v3 release moves fan curve configuration out of the compiled binary and
into a reloadable runtime file while adding safety and quality-of-life
improvements.

## Highlights

- **Runtime configuration** – the daemon reads `/etc/nvidia-fan-controlV2.conf`
  (or the path provided via `--config`/`NVFC_CONFIG_PATH`) on startup and accepts
  `SIGHUP` to reload without rebuilding.
- **Safety improvements** – fan curves are clamped to sensible ranges with clear
  validation warnings, and the control loop applies hysteresis to avoid rapid
  fan changes.
- **TUI refresh** – arrow keys now follow the standard: `↑/↓` select points and
  `←/→` adjust the temperature or speed columns.
- **Systemd integration** – `ExecReload=/bin/kill -HUP $MAINPID` enables
  in-place reconfiguration through `systemctl reload nvidia-fan-controlV2`.
- **Automation support** – new `Makefile` targets simplify builds (`make build`)
  and end-to-end tests with the mock NVML backend (`USE_MOCK_NVML=1 make test`).

## Editing and Applying the Fan Curve

1. Launch the TUI editor:
   ```bash
   nvidia_fan_controlV2 --re-configure
   ```
2. Adjust temperatures/speeds with `←/→` after selecting a point using `↑/↓`.
3. Save with `S`. The CLI automatically issues `systemctl reload
   nvidia-fan-controlV2` to apply changes without interrupting service.

To reload previously edited settings without opening the TUI run:
```bash
nvidia_fan_controlV2 --apply
```

## Building and Testing

- Build the daemon: `make build`
- Run automated tests with the mock NVML backend: `USE_MOCK_NVML=1 make test`

The mock NVML library logs fan updates under `mock_nvml/fan_speed.log`, which is
what the test suite inspects to confirm clamping, reload behaviour, and fan
smoothing.
