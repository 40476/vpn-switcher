# VPN Switcher Daemon

A intelligent network automation daemon that dynamically switches between Tailscale, Cloudflare WARP, or disables VPN based on your active NetworkManager connection profiles.

## Overview

`vpn-switcher` automatically manages your VPN state by monitoring network connections and applying regex-based rules to determine which VPN service (if any) should be active. It runs as a background daemon with a CLI interface for manual control.

## Features

- **Automatic VPN Switching**: Dynamically switch between Tailscale, WARP, or no VPN based on connection name patterns
- **Regex Pattern Matching**: Flexible regex-based rules for matching NetworkManager connection profiles
- **Dual Mode Operation**: Auto mode (automatic) and Manual mode (locked state)
- **Desktop Notifications**: Native OS notifications for state changes
- **CLI Interface**: Easy-to-use command-line interface for control
- **Persistent Configuration**: Settings saved to `/etc/vpn-switcher/vpn-switcher.conf`
- **Thread-Safe Monitoring**: Background network monitoring with 3-second polling

## Requirements

### System Dependencies
- Linux with NetworkManager
- `tailscale` - Tailscale VPN client
- `warp-cli` - Cloudflare WARP CLI
- `nmcli` - NetworkManager command-line tool
- `notify-send` - Desktop notification utility

### Build Dependencies
- GCC or compatible C compiler
- POSIX-compliant system libraries

## Installation

### 1. Build the Daemon

```bash
gcc -o vpn-switcher vpn-switcher.c -lpthread
```

### 2. Install (Optional)

```bash
sudo cp vpn-switcher /usr/local/bin/
sudo chmod +x /usr/local/bin/vpn-switcher
```

### 3. Start the Daemon

```bash
# Run in foreground (for testing)
sudo ./vpn-switcher --daemon

# Or with systemd (recommended)
sudo systemctl enable --now vpn-switcher
```

**Note**: The daemon **must** run as root to manage system VPN services and write to `/etc/resolv.conf`.

### Usage

```bash
# Start daemon
sudo vpn-switcher --daemon

# Stop daemon (find PID and kill, or use systemd)
sudo pkill vpn-switcher
```

### CLI Commands

```bash
# Check current status
vpn-switcher --status

# View help
vpn-switcher --help

# Toggle auto/manual mode
vpn-switcher --mode auto
vpn-switcher --mode manual

# Manually set VPN state
vpn-switcher --set tailscale
vpn-switcher --set warp
vpn-switcher --set none
vpn-switcher --set auto    # Re-enable auto mode

# Cycle through states
vpn-switcher --cycle

# Add pattern rules
vpn-switcher --add-tailscale "^Office$"
vpn-switcher --add-warp ".*Coffee.*"
vpn-switcher --add-none "^Work$"

# Remove pattern rules
vpn-switcher --remove-tailscale "^Office$"
vpn-switcher --remove-warp ".*Coffee.*"
vpn-switcher --remove-none "^Work$"

# Set DNS nameserver
vpn-switcher --set-nameserver 8.8.8.8

# Manual trigger (advanced)
vpn-switcher --trigger up "HomeWiFi"
```

### Status Output Example

```
Current State      : tailscale
Active Network     : Home
Operating Mode     : auto
Configured DNS     : 1.1.1.1
None Patterns      : 0 registered
Tailscale Patterns : 1 registered
WARP Patterns      : 2 registered
```

## How It Works

### Pattern Matching Priority

1. **None (Bypass)**: If connection matches a `none_pattern`, VPN is disabled completely
2. **Tailscale**: If connection matches a `tailscale_pattern`, Tailscale is activated
3. **WARP**: If connection matches a `warp_pattern`, WARP is activated
4. **Fallback**: If no patterns match, VPN is disabled

### Network Monitoring

The daemon runs a background thread that:
- Polls network status every 3 seconds via `nmcli`
- Detects connection changes
- Automatically re-evaluates VPN state on network change
- Respects manual mode (skips auto-evaluation)

### State Transitions

```
None (No VPN) ←→ Tailscale ←→ WARP
     ↑                                    ↓
     └───────────── Auto Cycle ───────────┘
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    vpn-switcher Daemon                        │
│                                                             │
│  ┌─────────────┐   ┌──────────────┐   ┌──────────────────┐ │
│  │  Config     │   │  Network     │   │   VPN Services   │ │
│  │  Manager    │──▶│  Monitor     │──▶│   (Tailscale/WARP)│ │
│  └─────────────┘   └──────────────┘   └──────────────────┘ │
│           ▲              ▲                     ▲            │
│           │              │                     │            │
│  ┌─────────────┐   ┌──────────────┐   ┌──────────────────┐ │
│  │  CLI Socket │   │  Syslog      │   │  Desktop         │ │
│  │  Server     │   │  Logger      │   │  Notifications   │ │
│  └─────────────┘   └──────────────┘   └──────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## File Locations

| Path | Purpose |
|------|---------|
| `/var/run/vpn-switcher.sock` | Unix socket for CLI communication |
| `/var/run/vpn-switcher.lock` | Lock file for single-instance enforcement |
| `/etc/vpn-switcher/vpn-switcher.conf` | Persistent configuration |
| `/etc/resolv.conf` | DNS configuration (modified by daemon) |

## Security Considerations

- **Root Required**: Daemon must run as root for VPN management
- **Socket Permissions**: Unix socket created with `0666` permissions
- **Single Instance**: Lock file prevents multiple daemon instances
- **Config Permissions**: Configuration file set to `0644`

## Troubleshooting

### Daemon won't start
```bash
# Check if another instance is running
sudo flock -u /var/run/vpn-switcher.lock

# Verify root permissions
sudo vpn-switcher --daemon
```

### VPN not switching
```bash
# Check daemon status
sudo systemctl status vpn-switcher

# View logs
sudo journalctl -u vpn-switcher -f
```

# Test manual trigger
```bash
sudo vpn-switcher --trigger up "$(nmcli -t -f NAME connection show --active | head -1)"
```

## License

This project is provided as-is with no warranty. Dont be stupid >w<
