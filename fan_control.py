#!/usr/bin/env python3
import curses
import json
import os
import time
import argparse
from pynvml import (
    nvmlInit,
    nvmlShutdown,
    nvmlDeviceGetHandleByIndex,
    nvmlDeviceGetTemperature,
    nvmlDeviceGetFanSpeed,
    nvmlDeviceSetFanSpeed_v2,
    NVML_TEMPERATURE_GPU,
)

PROFILE_FILE = os.path.expanduser('~/.config/nvidia-fan-control/profiles.json')
DEFAULT_PROFILES = {
    "quiet": {
        "temps": [0, 40, 55, 67, 75, 85],
        "speeds": [20, 25, 35, 50, 60, 80]
    },
    "default": {
        "temps": [0, 40, 55, 67, 75, 85],
        "speeds": [25, 30, 45, 65, 78, 99]
    },
    "performance": {
        "temps": [0, 40, 55, 67, 75, 85],
        "speeds": [40, 50, 65, 80, 90, 100]
    },
    "custom": {
        "temps": [0, 40, 55, 67, 75, 85],
        "speeds": [25, 30, 45, 65, 78, 99]
    }
}


def load_profiles():
    if not os.path.exists(PROFILE_FILE):
        os.makedirs(os.path.dirname(PROFILE_FILE), exist_ok=True)
        save_profiles(DEFAULT_PROFILES)
    with open(PROFILE_FILE, 'r') as f:
        return json.load(f)


def save_profiles(profiles):
    os.makedirs(os.path.dirname(PROFILE_FILE), exist_ok=True)
    with open(PROFILE_FILE, 'w') as f:
        json.dump(profiles, f, indent=2)


def get_fan_speed_for_temp(profile, temp):
    temps = profile['temps']
    speeds = profile['speeds']
    for i in range(len(temps)-1):
        if temp < temps[i+1]:
            return speeds[i]
    return speeds[-1]


def apply_profile(profile_name, device):
    profiles = load_profiles()
    prof = profiles.get(profile_name, profiles['default'])
    temp = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU)
    speed = get_fan_speed_for_temp(prof, temp)
    try:
        nvmlDeviceSetFanSpeed_v2(device, 0, speed)
    except Exception:
        pass
    return temp, speed


def status_screen(stdscr, device, profile):
    stdscr.clear()
    stdscr.addstr(0, 0, f"Profile: {profile}")
    temp, speed = apply_profile(profile, device)
    stdscr.addstr(2, 0, f"Temperature: {temp}C")
    stdscr.addstr(3, 0, f"Fan Speed: {speed}%")
    stdscr.addstr(5, 0, "Press any key to return")
    stdscr.refresh()
    stdscr.getch()


def select_profile_screen(stdscr, profiles, current):
    idx = list(profiles.keys()).index(current)
    while True:
        stdscr.clear()
        stdscr.addstr(0, 0, "Select Profile")
        for i, key in enumerate(profiles.keys()):
            marker = '>' if i == idx else ' '
            stdscr.addstr(i+2, 0, f"{marker} {key}")
        stdscr.refresh()
        k = stdscr.getch()
        if k == curses.KEY_UP:
            idx = max(0, idx-1)
        elif k == curses.KEY_DOWN:
            idx = min(len(profiles)-1, idx+1)
        elif k in [curses.KEY_ENTER, ord('\n')]:
            return list(profiles.keys())[idx]
        elif k in [ord('q'), 27]:
            return current


def edit_custom_screen(stdscr, profiles):
    temps = profiles['custom']['temps']
    speeds = profiles['custom']['speeds']
    idx = 0
    while True:
        stdscr.clear()
        stdscr.addstr(0,0,"Edit Custom Fan Curve")
        stdscr.addstr(2,0,"Use LEFT/RIGHT to choose point, UP/DOWN to change speed")
        for i, (t, s) in enumerate(zip(temps, speeds)):
            marker = '>' if i==idx else ' '
            stdscr.addstr(i+4, 0, f"{marker} T={t}C -> {s}%")
        stdscr.addstr(len(temps)+6,0,"Press s to save, q to cancel")
        stdscr.refresh()
        k = stdscr.getch()
        if k == curses.KEY_LEFT:
            idx = max(0, idx-1)
        elif k == curses.KEY_RIGHT:
            idx = min(len(temps)-1, idx+1)
        elif k == curses.KEY_UP:
            speeds[idx] = min(100, speeds[idx]+1)
        elif k == curses.KEY_DOWN:
            speeds[idx] = max(0, speeds[idx]-1)
        elif k == ord('s'):
            profiles['custom']['speeds'] = speeds
            save_profiles(profiles)
            return
        elif k in [ord('q'), 27]:
            return


def main_menu(stdscr):
    curses.curs_set(0)
    profiles = load_profiles()
    current_profile = 'default'
    nvmlInit()
    device = nvmlDeviceGetHandleByIndex(0)
    menu = ['View Status', 'Select Profile', 'Edit Custom Fan Curve', 'Apply and Exit']
    idx = 0
    while True:
        stdscr.clear()
        stdscr.addstr(0,0,"NVIDIA Fan Control")
        for i, item in enumerate(menu):
            marker = '>' if i==idx else ' '
            stdscr.addstr(i+2,0,f"{marker} {item}")
        stdscr.refresh()
        k = stdscr.getch()
        if k == curses.KEY_UP:
            idx = max(0, idx-1)
        elif k == curses.KEY_DOWN:
            idx = min(len(menu)-1, idx+1)
        elif k in [curses.KEY_ENTER, ord('\n')]:
            if idx==0:
                status_screen(stdscr, device, current_profile)
            elif idx==1:
                current_profile = select_profile_screen(stdscr, profiles, current_profile)
            elif idx==2:
                edit_custom_screen(stdscr, profiles)
            elif idx==3:
                break
        elif k in [ord('q'), 27]:
            break
    nvmlShutdown()


def run_service(profile):
    nvmlInit()
    device = nvmlDeviceGetHandleByIndex(0)
    try:
        while True:
            apply_profile(profile, device)
            time.sleep(5)
    except KeyboardInterrupt:
        pass
    nvmlShutdown()


def print_status(profile):
    nvmlInit()
    device = nvmlDeviceGetHandleByIndex(0)
    temp, speed = apply_profile(profile, device)
    print(f"Temp: {temp}C, Fan: {speed}% (profile {profile})")
    nvmlShutdown()


def cli():
    parser = argparse.ArgumentParser(description="NVIDIA Fan Control")
    parser.add_argument("--service", action="store_true", help="run as service")
    parser.add_argument("--status", action="store_true", help="show current status")
    parser.add_argument("--profile", default="default", help="profile to use")
    args = parser.parse_args()

    if args.service:
        run_service(args.profile)
    elif args.status:
        print_status(args.profile)
    else:
        curses.wrapper(main_menu)


if __name__ == '__main__':
    cli()
