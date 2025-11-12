# UNDER HEAVY DEVELOPEMENT

---
# NVIDIA Fan Control Guide

## Overview
This project provides a dynamic fan control utility for NVIDIA GPUs using NVML. It includes:
- A custom fan curve based on GPU temperature.
- A systemd service for automatic startup and logging.
- ~~A terminal-based GUI for configuring the fan curve interactively.~~ #TODO

## Steps to Set Up

### 1. Compile the C Code
First, locate the NVML header file:
```sh
find /usr -name "nvml.h"
```
Then, compile the program:
```sh
gcc -o nvidia_fan_controlV2 nvidia_fan_controlV2.c -I/usr/the/directory/that/includes/nvml.h/file/ -lnvidia-ml
```

### 2. Install the Systemd Service
Create a systemd service file:
```sh
echo "[Unit]
Description=NVIDIA Fan Control Service
After=multi-user.target

[Service]
Type=simple
ExecStart=/usr/local/bin/nvidia_fan_controlV2
Restart=always
User=root

[Install]
WantedBy=multi-user.target" | sudo tee /etc/systemd/system/nvidia-fan-controlV2.service
```

Enable and start the service:
```sh
sudo systemctl enable nvidia-fan-controlV2
sudo systemctl start nvidia-fan-controlV2
```

### 3. View Logs and Reconfigure
To view real-time logs:
```sh
journalctl -u nvidia-fan-controlV2 -f
```
### TO BE IMPLEMENTED
To reconfigure the fan curve:
```sh
nvidia_fan_controlV2 --re-configure
```

## Future Enhancements
### (please contribute if you have any 2-Fan or 3-Fan architecture and you need a more complex fan_control design)
- Support for multi-fan GPUs.
- Improved adaptive fan curve options.
- A full-screen terminal-based GUI for configuring fan curves interactively.


# DEVELOPMENT GUIDE
## How to build & run with the mock

1. Build the mock:

```bash
make -C mock_nvml
```

2. Build your app **against the mock header+lib** (no changes to your C file):

```bash
gcc -o nvidia_fan_controlV2 nvidia_fan_controlV2.c \
  -Imock_nvml -Lmock_nvml -lnvidia-ml \
  -Wl,-rpath,'$ORIGIN/mock_nvml'
```

3. Run with the mock library (and set an optional state dir):

```bash
export NVML_MOCK_DIR=/tmp/nvml-mock   # optional; defaults to /tmp/nvml-mock
LD_LIBRARY_PATH=./mock_nvml ./nvidia_fan_controlV2
```

4. Drive temperature / fans from another shell:

Make the `mockctl.sh` script executable:

```bash
chmod 755 mock_nvml/mockctl.sh
```
then:

```bash
./mock_nvml/mockctl.sh set-temp 38
./mock_nvml/mockctl.sh set-temp 60
./mock_nvml/mockctl.sh set-temp 80
./mock_nvml/mockctl.sh set-fans 1
./mock_nvml/mockctl.sh tail-log     # watch the speeds your app requests
```

This lets anyone run the binary and see realistic behavior:

* Your loop still calls `nvmlDeviceGetTemperature` and picks a fan speed from your curve. 
* The mock writes every requested speed to `fan_speed.log`, so tests/TUI can assert on it.
* If no `temperature` file is present, the mock auto-ramps 35→85°C so the app still “moves”.
