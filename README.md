# UNDER HEAVY DEVELOPEMENT

---
# NVIDIA Fan Control Guide

## Overview
This project provides a dynamic fan control utility for NVIDIA GPUs using NVML. It includes:
- A custom fan curve based on GPU temperature.
- A systemd service for automatic startup and logging.
- A terminal-based GUI for configuring the fan curve interactively.

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
To reconfigure the fan curve:
```sh
nvidia_fan_controlV2 --re-configure
```

## Future Enhancements
- Support for multi-fan GPUs.
- Improved adaptive fan curve options.
- A full-screen terminal-based GUI for configuring fan curves interactively.
