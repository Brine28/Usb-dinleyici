# USB Monitor

A lightweight Windows background utility that listens for USB device arrival events, extracts the VID/PID, looks up the manufacturer and product name from the Gentoo `usb.ids` database, shows a tray notification, and writes the result to a UTF-8 log file.

This project is now entirely implemented in C++ and does not require Python or any external script runtime.

## How it works

The application runs as a hidden Win32 window with a tray icon and subscribes to `WM_DEVICECHANGE` notifications from Windows.

When a USB device is connected:

- the application parses the device interface path and extracts `VID_XXXX` and `PID_XXXX`
- it downloads or reuses a cached copy of the Gentoo `usb.ids` database
- it matches the VID/PID to the vendor and product name
- it writes the result to a log file
- it shows a Windows tray balloon notification

## Features

- tray icon with a right-click exit menu
- single-instance protection so it cannot be launched twice
- `usb.ids` cache with a 7-day maximum age to avoid unnecessary downloads
- safe temporary-file handling for updates during concurrent events
- fully standalone `.exe` build with static C++ runtime linking
- no Python dependency; everything is handled in the C++ executable

## Requirements

- Windows 10 / 11
- MinGW-w64 for building on Windows or cross-compiling from Linux

## Build

From Linux for Windows cross-compilation:

```bash
sudo apt install mingw-w64

x86_64-w64-mingw32-g++ -std=c++17 -O2 -mwindows -municode \
  usb_monitor.cpp -o usb_monitor.exe \
  -static -static-libgcc -static-libstdc++ \
  -luser32 -lshell32 -lole32 -lcomctl32 -lwinpthread
```

The generated executable is intentionally static so it does not rely on extra runtime DLL files such as `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, or `libwinpthread-1.dll` on the target machine. It uses the native Windows system DLLs instead.

## Usage

1. Copy `usb_monitor.exe` to any folder.
2. Run it once; the tray icon appears.
3. Connect a USB device.
4. The program automatically identifies the device and logs the result.
5. A balloon notification appears in the system tray when the vendor/product is resolved.

To start it automatically on Windows startup, place a shortcut to the executable in:

```text
shell:startup
```

## Output

The app writes a log file named `usb_devices_log.txt` next to the executable.

Example:

```text
[2026-07-17T14:32:05] VID=0951 PID=1666 Marka=Kingston Technology Urun=DataTraveler 100 G3
```
