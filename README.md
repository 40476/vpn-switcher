
# README.MD

## Setup and Integration Guide

This guide describes how to build, run, and manage the VPN Switcher Daemon.

## 1. Build & Compile

Compile the single-file C codebase using `gcc`. Note that the daemon relies on
standard UNIX POSIX threads (`lpthread`) to power its background network monitors.

```bash
gcc -O3 -Wall vpn-switcher.c -o vpn-switcher -lpthread
sudo cp vpn-switcher /usr/local/bin/
```

---

## 2. Configure Systemd Service Unit

Because the daemon runs directly in the foreground, we configure the systemd service
using `Type=simple`. This enables the system log service to aggregate all outputs
and logs natively.

Write the following configuration directly to `/etc/systemd/system/vpn-switcher.service`:

```ini
[Unit]
Description=VPN Switcher Foreground Daemon
After=network.target network-online.target NetworkManager.service

[Service]
Type=simple
ExecStart=/usr/local/bin/vpn-switcher --daemon
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Reload and spin up the background process:
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now vpn-switcher.service
```

---

## 3. Configuration Management & CLI Interface

With the daemon active, control configuration entries and switch states from any shell profile:

### Read Daemon Status
```bash
vpn-switcher --status
```

### Bypass Active VPN Rules (Direct / No-VPN Bypass Mode)
To ignore all auto-VPN rules and bypass any VPN transitions on specific networks (e.g., trusted local networks), register those SSIDs under `None`:
```bash
vpn-switcher --add-none "^SecureLocalNetwork$"
vpn-switcher --add-none "^HomeInternal$"
```

### Add Other Profile Rules
```bash
vpn-switcher --add-tailscale "^HomeOffice$"
vpn-switcher --add-warp ".*Airport_Wifi.*"
```

### Remove Profiles
```bash
vpn-switcher --remove-none "^SecureLocalNetwork$"
vpn-switcher --remove-tailscale "^HomeOffice$"
vpn-switcher --remove-warp ".*Airport_Wifi.*"
```

### Manual Controls & State Cycling
You can force transitions instantly. Manual transitions lock the system to manual mode, which prevents background network sweeps from overriding your choice.
```bash
vpn-switcher --set tailscale
vpn-switcher --set warp
vpn-switcher --set none
vpn-switcher --set auto       # Restores dynamic automatic evaluations immediately
```

Toggle sequentially through: None -> Tailscale -> WARP -> Auto:
```bash
vpn-switcher --cycle
```

### Adjust Fallback Nameserver
```bash
vpn-switcher --set-nameserver 8.8.8.8
```
