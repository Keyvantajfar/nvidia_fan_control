# UNDER HEAVY DEVELOPEMENT

---
# NVIDIA Fan Control Guide

## Overview
This project provides a dynamic fan control utility for NVIDIA GPUs using NVML. It includes:
- A custom fan curve based on GPU temperature.
- A systemd service for automatic startup and logging.
- A Python based terminal interface (`fan_control.py`) for configuring fan curves.

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

Launch the terminal interface to change profiles or edit the fan curve:
```sh
python3 fan_control.py
```

### 4. Using the Python Interface
Install the Python NVML bindings first:
```sh
pip install pynvml
```
Then launch the interface:
```sh
python3 fan_control.py
```
Within the menu you can view the current temperature and fan speed,
switch between `quiet`, `default`, and `performance` profiles, or edit
the `custom` profile. The settings are stored in
`~/.config/nvidia-fan-control/profiles.json`.

Additional command line options are available:
```sh
python3 fan_control.py --status           # print current temperature and fan speed
python3 fan_control.py --profile quiet   # run using a specific profile
python3 fan_control.py --service         # run continuously as a service
```

This Python tool operates independently from the C based service
(`nvidia_fan_controlV2`) and does not yet modify its configuration.

## Future Enhancements
### (please contribute if you have any 2-Fan or 3-Fan architecture and you need a more complex fan_control design)
- Support for multi-fan GPUs.
- Improved adaptive fan curve options.
- A full-screen terminal-based GUI for configuring fan curves interactively.
