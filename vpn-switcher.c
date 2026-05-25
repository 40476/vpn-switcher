#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <regex.h>
#include <pwd.h>
#include <fcntl.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>

#define SOCKET_PATH "/var/run/vpn-switcher.sock"
#define LOCK_PATH "/var/run/vpn-switcher.lock"
#define CONFIG_DIR "/etc/vpn-switcher"
#define CONFIG_FILE "/etc/vpn-switcher/vpn-switcher.conf"

#define MAX_PATTERNS 64
#define MAX_PATTERN_LEN 128
#define STATE_NONE "none"
#define STATE_TAILSCALE "tailscale"
#define STATE_WARP "warp"

#define MODE_AUTO "auto"
#define MODE_MANUAL "manual"

// Configuration State Struct
typedef struct {
    char nameserver[64];
    char mode[16]; // "auto" or "manual"
    char tailscale_patterns[MAX_PATTERNS][MAX_PATTERN_LEN];
    int tailscale_count;
    char warp_patterns[MAX_PATTERNS][MAX_PATTERN_LEN];
    int warp_count;
    char none_patterns[MAX_PATTERNS][MAX_PATTERN_LEN];
    int none_count;
} Config;

Config config;
char current_state[32] = STATE_NONE;
char current_connection[256] = "Disconnected";
int lock_fd = -1;

// Forward Declarations
void load_defaults();
void save_config();
void load_config();
void send_notification(const char *title, const char *msg, const char *icon);
void apply_vpn_state(const char *state);
void handle_trigger(const char *action, const char *connection_id);
void trim_whitespace(char *str);
void get_active_nm_connection(char *out_conn, size_t max_len);
void *network_monitor_thread(void *arg);

// Helper to run a command and grab output
void get_cmd_output(const char *cmd, char *out_buf, size_t max_len) {
    out_buf[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (fp) {
        if (fgets(out_buf, max_len, fp)) {
            trim_whitespace(out_buf);
        }
        pclose(fp);
    }
}

// Strip leading/trailing whitespaces in-place
void trim_whitespace(char *str) {
    if (!str) return;
    char *start = str;
    char *end;
    while (isspace((unsigned char)*start)) start++;
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
    if (*str == 0) return;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
}

// Load hardcoded default values
void load_defaults() {
    strcpy(config.nameserver, "1.1.1.1");
    strcpy(config.mode, MODE_AUTO);
    config.tailscale_count = 1;
    strcpy(config.tailscale_patterns[0], "^Home$");
    config.warp_count = 2;
    strcpy(config.warp_patterns[0], ".*Guest.*");
    strcpy(config.warp_patterns[1], "Public-.*");
    config.none_count = 0;
}

// Save config state back to file
void save_config() {
    mkdir(CONFIG_DIR, 0755);
    FILE *fp = fopen(CONFIG_FILE, "w");
    if (!fp) {
        syslog(LOG_ERR, "Failed to open config file for writing: %s", strerror(errno));
        return;
    }

    fprintf(fp, "# /etc/vpn-switcher/vpn-switcher.conf\n\n");
    fprintf(fp, "nameserver=%s\n", config.nameserver);
    fprintf(fp, "mode=%s\n\n", config.mode);

    fprintf(fp, "# Tailscale Network Matches\n");
    for (int i = 0; i < config.tailscale_count; i++) {
        fprintf(fp, "tailscale_pattern=%s\n", config.tailscale_patterns[i]);
    }
    fprintf(fp, "\n# WARP Network Matches\n");
    for (int i = 0; i < config.warp_count; i++) {
        fprintf(fp, "warp_pattern=%s\n", config.warp_patterns[i]);
    }
    fprintf(fp, "\n# None (No-VPN) Network Matches\n");
    for (int i = 0; i < config.none_count; i++) {
        fprintf(fp, "none_pattern=%s\n", config.none_patterns[i]);
    }

    fclose(fp);
    chmod(CONFIG_FILE, 0644);
    syslog(LOG_INFO, "Configuration saved successfully.");
}

// Parse configuration file
void load_config() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        syslog(LOG_WARNING, "Config not found. Initializing defaults.");
        load_defaults();
        save_config();
        return;
    }

    // Reset patterns to read clean
    config.tailscale_count = 0;
    config.warp_count = 0;
    config.none_count = 0;
    strcpy(config.nameserver, "1.1.1.1");
    strcpy(config.mode, MODE_AUTO);

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        char *key = strtok(line, "=");
        char *val = strtok(NULL, "");
        if (!key || !val) continue;

        trim_whitespace(key);
        trim_whitespace(val);

        if (strcmp(key, "nameserver") == 0) {
            strncpy(config.nameserver, val, sizeof(config.nameserver) - 1);
        } else if (strcmp(key, "mode") == 0) {
            strncpy(config.mode, val, sizeof(config.mode) - 1);
        } else if (strcmp(key, "tailscale_pattern") == 0) {
            if (config.tailscale_count < MAX_PATTERNS) {
                strncpy(config.tailscale_patterns[config.tailscale_count++], val, MAX_PATTERN_LEN - 1);
            }
        } else if (strcmp(key, "warp_pattern") == 0) {
            if (config.warp_count < MAX_PATTERNS) {
                strncpy(config.warp_patterns[config.warp_count++], val, MAX_PATTERN_LEN - 1);
            }
        } else if (strcmp(key, "none_pattern") == 0) {
            if (config.none_count < MAX_PATTERNS) {
                strncpy(config.none_patterns[config.none_count++], val, MAX_PATTERN_LEN - 1);
            }
        }
    }
    fclose(fp);
    syslog(LOG_INFO, "Configuration loaded successfully.");
}

// Find active graphical desktop user and send native notification
void send_notification(const char *title, const char *msg, const char *icon) {
    char active_user[128] = "";
    char user_uid[32] = "";

    get_cmd_output("w -h | awk '{print $1}' | head -n 1", active_user, sizeof(active_user));
    
    if (strlen(active_user) == 0) {
        get_cmd_output("loginctl list-users | awk 'NR==2 {print $2}'", active_user, sizeof(active_user));
    }

    if (strlen(active_user) > 0) {
        char cmd_uid[256];
        snprintf(cmd_uid, sizeof(cmd_uid), "id -u %s", active_user);
        get_cmd_output(cmd_uid, user_uid, sizeof(user_uid));

        if (strlen(user_uid) > 0) {
            char notify_cmd[1024];
            snprintf(notify_cmd, sizeof(notify_cmd),
                     "sudo -u %s DBUS_SESSION_BUS_ADDRESS=\"unix:path=/run/user/%s/bus\" "
                     "DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 "
                     "notify-send -a \"%s\" \"%s\" -i \"%s\" -t 4000 >/dev/null 2>&1",
                     active_user, user_uid, title, msg, icon);
            system(notify_cmd);
        }
    }
}

// Transition actual system service state
void apply_vpn_state(const char *state) {
    if (strcmp(state, STATE_TAILSCALE) == 0) {
        syslog(LOG_INFO, "Transitioning to Tailscale.");
        system("warp-cli disconnect >/dev/null 2>&1");
        system("systemctl disable --now warp-svc.service >/dev/null 2>&1");
        system("killall -9 warp-svc 2>/dev/null");

        // Write Nameserver
        FILE *dns = fopen("/etc/resolv.conf", "w");
        if (dns) {
            fprintf(dns, "nameserver %s\n", config.nameserver);
            fclose(dns);
        }

        system("tailscale up >/dev/null 2>&1");
        strcpy(current_state, STATE_TAILSCALE);
    } 
    else if (strcmp(state, STATE_WARP) == 0) {
        syslog(LOG_INFO, "Transitioning to WARP.");
        system("tailscale down >/dev/null 2>&1");

        // Write Nameserver
        FILE *dns = fopen("/etc/resolv.conf", "w");
        if (dns) {
            fprintf(dns, "nameserver %s\n", config.nameserver);
            fclose(dns);
        }

        system("systemctl start warp-svc >/dev/null 2>&1");
        system("warp-cli connect >/dev/null 2>&1");
        strcpy(current_state, STATE_WARP);
    } 
    else {
        syslog(LOG_INFO, "Disabling VPNs.");
        system("warp-cli disconnect >/dev/null 2>&1");
        system("systemctl disable --now warp-svc.service >/dev/null 2>&1");
        system("tailscale down >/dev/null 2>&1");

        // Write Nameserver
        FILE *dns = fopen("/etc/resolv.conf", "w");
        if (dns) {
            fprintf(dns, "nameserver %s\n", config.nameserver);
            fclose(dns);
        }
        strcpy(current_state, STATE_NONE);
    }
}

// Evaluate connection profiles on network change
void handle_trigger(const char *action, const char *connection_id) {
    if (strcmp(action, "up") != 0 || strlen(connection_id) == 0 || strcmp(connection_id, "Disconnected") == 0) {
        // Safe fallback cleanup if no active profiles are found
        apply_vpn_state(STATE_NONE);
        return;
    }

    strncpy(current_connection, connection_id, sizeof(current_connection) - 1);
    syslog(LOG_INFO, "Evaluating connection update: %s", connection_id);

    // 1. Evaluate Explicit None (No-VPN / Bypass) Regex Matching
    for (int i = 0; i < config.none_count; i++) {
        regex_t regex;
        int reti = regcomp(&regex, config.none_patterns[i], REG_EXTENDED | REG_NOSUB);
        if (reti == 0) {
            reti = regexec(&regex, connection_id, 0, NULL, 0);
            regfree(&regex);
            if (reti == 0) {
                syslog(LOG_INFO, "Matched None (Bypass VPN) regex: %s", config.none_patterns[i]);
                apply_vpn_state(STATE_NONE);
                char notify_msg[256];
                snprintf(notify_msg, sizeof(notify_msg), "Matched %s. Bypassed VPN (No VPN Active).", connection_id);
                send_notification("Network Automator", notify_msg, "network-vpn");
                return;
            }
        }
    }

    // 2. Evaluate Tailscale Regex Matching
    for (int i = 0; i < config.tailscale_count; i++) {
        regex_t regex;
        int reti = regcomp(&regex, config.tailscale_patterns[i], REG_EXTENDED | REG_NOSUB);
        if (reti == 0) {
            reti = regexec(&regex, connection_id, 0, NULL, 0);
            regfree(&regex);
            if (reti == 0) {
                syslog(LOG_INFO, "Matched tailscale regex: %s", config.tailscale_patterns[i]);
                apply_vpn_state(STATE_TAILSCALE);
                char notify_msg[256];
                snprintf(notify_msg, sizeof(notify_msg), "Matched %s. Switched to Tailscale.", connection_id);
                send_notification("Network Automator", notify_msg, "network-vpn");
                return;
            }
        }
    }

    // 3. Evaluate Cloudflare WARP Regex Matching
    for (int i = 0; i < config.warp_count; i++) {
        regex_t regex;
        int reti = regcomp(&regex, config.warp_patterns[i], REG_EXTENDED | REG_NOSUB);
        if (reti == 0) {
            reti = regexec(&regex, connection_id, 0, NULL, 0);
            regfree(&regex);
            if (reti == 0) {
                syslog(LOG_INFO, "Matched WARP regex: %s", config.warp_patterns[i]);
                apply_vpn_state(STATE_WARP);
                char notify_msg[256];
                snprintf(notify_msg, sizeof(notify_msg), "Matched %s. Switched to WARP.", connection_id);
                send_notification("Network Automator", notify_msg, "network-vpn");
                return;
            }
        }
    }

    // 4. Fallback for untracked profiles
    syslog(LOG_INFO, "No regex matches found for %s. Cleaning up active VPN connections.", connection_id);
    apply_vpn_state(STATE_NONE);
}

// Integrated Network Monitor Thread
void *network_monitor_thread(void *arg) {
    char prev_conn[256] = "";
    
    syslog(LOG_INFO, "Network background monitor loop initialized.");

    while (1) {
        sleep(3); // Query network status gently every 3 seconds

        if (strcmp(config.mode, MODE_MANUAL) == 0) {
            continue; // Ignore auto-updates while in manual mode
        }

        char current_conn[256] = "";
        get_active_nm_connection(current_conn, sizeof(current_conn));

        if (strcmp(current_conn, prev_conn) != 0) {
            syslog(LOG_INFO, "Network shift detected: '%s' -> '%s'", prev_conn, current_conn);
            strncpy(prev_conn, current_conn, sizeof(prev_conn) - 1);
            strncpy(current_connection, current_conn, sizeof(current_connection) - 1);
            handle_trigger("up", current_conn);
        }
    }
    return NULL;
}

// Queries NetworkManager via CLI to obtain current active physical connection name
void get_active_nm_connection(char *out_conn, size_t max_len) {
    out_conn[0] = '\0';
    FILE *fp = popen("nmcli -t -f ACTIVE,NAME,TYPE connection show --active 2>/dev/null", "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        trim_whitespace(line);
        if (strncmp(line, "yes:", 4) == 0) {
            char *name_start = line + 4;
            char *type_start = strrchr(line, ':');
            if (type_start && type_start > name_start) {
                *type_start = '\0';
                type_start++;
                if (strstr(type_start, "vpn") == NULL && 
                    strstr(type_start, "wireguard") == NULL &&
                    strstr(type_start, "tun") == NULL &&
                    strstr(type_start, "bridge") == NULL) {
                    strncpy(out_conn, name_start, max_len - 1);
                    out_conn[max_len - 1] = '\0';
                    break;
                }
            }
        }
    }
    pclose(fp);

    if (strlen(out_conn) == 0) {
        strcpy(out_conn, "Disconnected");
    }
}

// Signal Handler for daemon
void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        syslog(LOG_INFO, "Stopping VPN Switcher Daemon.");
        unlink(SOCKET_PATH);
        if (lock_fd != -1) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        unlink(LOCK_PATH);
        exit(0);
    }
}

// Handle socket communication with CLI Client instances
void process_client_command(int client_fd, char *cmd_line) {
    char response[2048] = "";
    trim_whitespace(cmd_line);

    char *cmd = strtok(cmd_line, " ");
    char *arg1 = NULL;
    char *arg2 = NULL;

    if (cmd) {
        if (strcmp(cmd, "TRIGGER") == 0) {
            arg1 = strtok(NULL, " ");
            arg2 = strtok(NULL, "");
        } else {
            arg1 = strtok(NULL, "");
        }
    }

    if (arg1) trim_whitespace(arg1);
    if (arg2) trim_whitespace(arg2);

    if (!cmd) {
        strcpy(response, "ERR: Empty command\n");
        write(client_fd, response, strlen(response));
        return;
    }

    if (strcmp(cmd, "STATUS") == 0) {
        snprintf(response, sizeof(response),
                 "Current State      : %s\n"
                 "Active Network     : %s\n"
                 "Operating Mode     : %s\n"
                 "Configured DNS     : %s\n"
                 "None Patterns      : %d registered\n"
                 "Tailscale Patterns : %d registered\n"
                 "WARP Patterns      : %d registered\n",
                 current_state, current_connection, config.mode, config.nameserver,
                 config.none_count, config.tailscale_count, config.warp_count);
    } 
    else if (strcmp(cmd, "MODE") == 0) {
        if (!arg1) {
            strcpy(response, "ERR: Usage: MODE <auto|manual>\n");
        } else {
            if (strcmp(arg1, MODE_AUTO) == 0 || strcmp(arg1, MODE_MANUAL) == 0) {
                strncpy(config.mode, arg1, sizeof(config.mode) - 1);
                save_config();
                snprintf(response, sizeof(response), "OK: Mode updated to %s\n", config.mode);
            } else {
                strcpy(response, "ERR: Invalid mode choice. Select auto or manual\n");
            }
        }
    }
    else if (strcmp(cmd, "SET") == 0) {
        if (!arg1) {
            strcpy(response, "ERR: Usage: SET <tailscale|warp|none|auto>\n");
        } else {
            if (strcmp(arg1, MODE_AUTO) == 0) {
                strncpy(config.mode, MODE_AUTO, sizeof(config.mode) - 1);
                save_config();
                
                char current_conn[256] = "";
                get_active_nm_connection(current_conn, sizeof(current_conn));
                strncpy(current_connection, current_conn, sizeof(current_connection) - 1);
                handle_trigger("up", current_conn);
                
                char notify_msg[256];
                snprintf(notify_msg, sizeof(notify_msg), "Switched mode to AUTO (dynamic evaluations active)");
                send_notification("VPN Switcher CLI", notify_msg, "network-vpn");
                snprintf(response, sizeof(response), "OK: Switched mode to AUTO and triggered network re-evaluation\n");
            }
            else if (strcmp(arg1, STATE_TAILSCALE) == 0 || strcmp(arg1, STATE_WARP) == 0 || strcmp(arg1, STATE_NONE) == 0) {
                strncpy(config.mode, MODE_MANUAL, sizeof(config.mode) - 1);
                save_config();

                apply_vpn_state(arg1);
                char notify_msg[256];
                snprintf(notify_msg, sizeof(notify_msg), "Manually set VPN state to: %s (Mode switched to MANUAL)", arg1);
                send_notification("VPN Switcher CLI", notify_msg, "network-vpn");
                snprintf(response, sizeof(response), "OK: Switched state to %s and locked mode to MANUAL\n", arg1);
            } else {
                strcpy(response, "ERR: Invalid VPN state. Choose tailscale, warp, none, or auto\n");
            }
        }
    } 
    else if (strcmp(cmd, "CYCLE") == 0) {
        char notify_msg[256] = "";
        
        // Loop: None -> Tailscale -> WARP -> Auto -> None
        if (strcmp(config.mode, MODE_AUTO) == 0) {
            strncpy(config.mode, MODE_MANUAL, sizeof(config.mode) - 1);
            save_config();
            apply_vpn_state(STATE_NONE);
            
            snprintf(notify_msg, sizeof(notify_msg), "Cycled to: NONE (Mode: MANUAL)");
            snprintf(response, sizeof(response), "OK: Cycled state to %s and locked mode to MANUAL\n", STATE_NONE);
        } else {
            if (strcmp(current_state, STATE_NONE) == 0) {
                apply_vpn_state(STATE_TAILSCALE);
                snprintf(notify_msg, sizeof(notify_msg), "Cycled to: TAILSCALE (Mode: MANUAL)");
                snprintf(response, sizeof(response), "OK: Cycled state to %s and locked mode to MANUAL\n", STATE_TAILSCALE);
            } else if (strcmp(current_state, STATE_TAILSCALE) == 0) {
                apply_vpn_state(STATE_WARP);
                snprintf(notify_msg, sizeof(notify_msg), "Cycled to: WARP (Mode: MANUAL)");
                snprintf(response, sizeof(response), "OK: Cycled state to %s and locked mode to MANUAL\n", STATE_WARP);
            } else {
                strncpy(config.mode, MODE_AUTO, sizeof(config.mode) - 1);
                save_config();
                
                char current_conn[256] = "";
                get_active_nm_connection(current_conn, sizeof(current_conn));
                strncpy(current_connection, current_conn, sizeof(current_connection) - 1);
                handle_trigger("up", current_conn);
                
                snprintf(notify_msg, sizeof(notify_msg), "Cycled to: AUTO mode");
                snprintf(response, sizeof(response), "OK: Cycled mode to AUTO and triggered network re-evaluation\n");
            }
        }
        
        send_notification("VPN Switcher CLI", notify_msg, "network-vpn");
    } 
    else if (strcmp(cmd, "TRIGGER") == 0) {
        if (!arg1 || !arg2) {
            strcpy(response, "ERR: Usage: TRIGGER <action> <connection_id>\n");
        } else {
            handle_trigger(arg1, arg2);
            strcpy(response, "OK: Trigger evaluated\n");
        }
    } 
    else if (strcmp(cmd, "ADD_TAILSCALE") == 0) {
        if (!arg1) {
            strcpy(response, "ERR: Usage: ADD_TAILSCALE <pattern>\n");
        } else if (config.tailscale_count >= MAX_PATTERNS) {
            strcpy(response, "ERR: Maximum Tailscale pattern limit reached\n");
        } else {
            strncpy(config.tailscale_patterns[config.tailscale_count++], arg1, MAX_PATTERN_LEN - 1);
            save_config();
            snprintf(response, sizeof(response), "OK: Pattern '%s' registered to Tailscale\n", arg1);
        }
    } 
    else if (strcmp(cmd, "ADD_WARP") == 0) {
        if (!arg1) {
            strcpy(response, "ERR: Usage: ADD_WARP <pattern>\n");
        } else if (config.warp_count >= MAX_PATTERNS) {
            strcpy(response, "ERR: Maximum WARP pattern limit reached\n");
        } else {
            strncpy(config.warp_patterns[config.warp_count++], arg1, MAX_PATTERN_LEN - 1);
            save_config();
            snprintf(response, sizeof(response), "OK: Pattern '%s' registered to WARP\n", arg1);
        }
    } 
    else if (strcmp(cmd, "ADD_NONE") == 0) {
        if (!arg1) {
            strcpy(response, "ERR: Usage: ADD_NONE <pattern>\n");
        } else if (config.none_count >= MAX_PATTERNS) {
            strcpy(response, "ERR: Maximum None pattern limit reached\n");
        } else {
            strncpy(config.none_patterns[config.none_count++], arg1, MAX_PATTERN_LEN - 1);
            save_config();
            snprintf(response, sizeof(response), "OK: Pattern '%s' registered to None profile\n", arg1);
        }
    } 
    else if (strcmp(cmd, "REMOVE_TAILSCALE") == 0) {
        if (!arg1) {
            strcpy(response, "ERR: Usage: REMOVE_TAILSCALE <pattern>\n");
        } else {
            int found = -1;
            for (int i = 0; i < config.tailscale_count; i++) {
                if (strcmp(config.tailscale_patterns[i], arg1) == 0) {
                    found = i;
                    break;
                }
            }
            if (found == -1) {
                snprintf(response, sizeof(response), "ERR: Pattern '%s' not found under Tailscale config\n", arg1);
            } else {
                for (int i = found; i < config.tailscale_count - 1; i++) {
                    strcpy(config.tailscale_patterns[i], config.tailscale_patterns[i + 1]);
                }
                config.tailscale_count--;
                save_config();
                snprintf(response, sizeof(response), "OK: Pattern '%s' deleted from Tailscale\n", arg1);
            }
        }
    } 
    else if (strcmp(cmd, "REMOVE_WARP") == 0) {
        if (!arg1) {
            strcpy(response, "ERR: Usage: REMOVE_WARP <pattern>\n");
        } else {
            int found = -1;
            for (int i = 0; i < config.warp_count; i++) {
                if (strcmp(config.warp_patterns[i], arg1) == 0) {
                    found = i;
                    break;
                }
            }
            if (found == -1) {
                snprintf(response, sizeof(response), "ERR: Pattern '%s' not found under WARP config\n", arg1);
            } else {
                for (int i = found; i < config.warp_count - 1; i++) {
                    strcpy(config.warp_patterns[i], config.warp_patterns[i + 1]);
                }
                config.warp_count--;
                save_config();
                snprintf(response, sizeof(response), "OK: Pattern '%s' deleted from WARP\n", arg1);
            }
        }
    } 
    else if (strcmp(cmd, "REMOVE_NONE") == 0) {
        if (!arg1) {
            strcpy(response, "ERR: Usage: REMOVE_NONE <pattern>\n");
        } else {
            int found = -1;
            for (int i = 0; i < config.none_count; i++) {
                if (strcmp(config.none_patterns[i], arg1) == 0) {
                    found = i;
                    break;
                }
            }
            if (found == -1) {
                snprintf(response, sizeof(response), "ERR: Pattern '%s' not found under None config\n", arg1);
            } else {
                for (int i = found; i < config.none_count - 1; i++) {
                    strcpy(config.none_patterns[i], config.none_patterns[i + 1]);
                }
                config.none_count--;
                save_config();
                snprintf(response, sizeof(response), "OK: Pattern '%s' deleted from None profile\n", arg1);
            }
        }
    } 
    else if (strcmp(cmd, "SET_NAMESERVER") == 0) {
        if (!arg1) {
            strcpy(response, "ERR: Usage: SET_NAMESERVER <dns-ip>\n");
        } else {
            strncpy(config.nameserver, arg1, sizeof(config.nameserver) - 1);
            save_config();
            snprintf(response, sizeof(response), "OK: DNS Nameserver updated to %s\n", arg1);
        }
    } 
    else {
        strcpy(response, "ERR: Unknown command code\n");
    }

    write(client_fd, response, strlen(response));
}

// Client IPC Socket Communication Helper
void send_daemon_cmd(const char *cmd_str) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("Failed to create socket client");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "Error: Unable to connect to background daemon (%s).\n"
                        "Please verify if 'vpn-switcher' daemon is running.\n", strerror(errno));
        close(fd);
        exit(EXIT_FAILURE);
    }

    if (write(fd, cmd_str, strlen(cmd_str)) == -1) {
        perror("Failed to dispatch command to socket");
        close(fd);
        exit(EXIT_FAILURE);
    }

    char buf[2048];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(fd);
}

void print_help(const char *prog) {
    printf("VPN-Switcher Daemon & CLI Controller\n");
    printf("Usage:\n");
    printf("  %s --daemon                       Run the controller in daemon background mode (root required)\n", prog);
    printf("  %s --status                       Retrieve status of active state and connected profiles\n", prog);
    printf("  %s --mode <auto|manual>           Toggle between dynamic monitoring and manual lock\n", prog);
    printf("  %s --set <tailscale|warp|none|auto> Manually pivot VPN state or switch to dynamic auto-evaluation\n", prog);
    printf("  %s --cycle                        Cycle through sequential states: (None -> Tailscale -> WARP -> Auto)\n", prog);
    printf("  %s --add-tailscale <pattern>      Add target matching regex for Tailscale transition\n", prog);
    printf("  %s --add-warp <pattern>           Add target matching regex for WARP transition\n", prog);
    printf("  %s --add-none <pattern>           Add target matching regex to bypass VPN (Direct None)\n", prog);
    printf("  %s --remove-tailscale <pattern>   Delete target matching regex under Tailscale profile\n", prog);
    printf("  %s --remove-warp <pattern>        Delete target matching regex under WARP profile\n", prog);
    printf("  %s --remove-none <pattern>        Delete target matching regex under None profile\n", prog);
    printf("  %s --set-nameserver <dns-ip>      Define primary fallback nameserver output to resolv.conf\n", prog);
    printf("  %s --trigger <action> <conn_id>   Trigger dispatcher update evaluation manually\n", prog);
}

// Entry Point
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    // Action: Daemon Initialization Mode (Runs in Foreground, strictly single-instance enforced)
    if (strcmp(argv[1], "--daemon") == 0) {
        if (getuid() != 0) {
            fprintf(stderr, "Fatal: Daemon mode demands root-level privileges (sudo).\n");
            return EXIT_FAILURE;
        }

        // 1. Enforce single-instance lock file
        lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT, 0600);
        if (lock_fd < 0) {
            fprintf(stderr, "Fatal: Unable to open daemon lock file %s: %s\n", LOCK_PATH, strerror(errno));
            return EXIT_FAILURE;
        }
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            fprintf(stderr, "Fatal: Another instance of vpn-switcher daemon is already running.\n");
            close(lock_fd);
            return EXIT_FAILURE;
        }

        // Setup signals for clean release of sockets/locks
        signal(SIGINT, handle_signal);
        signal(SIGTERM, handle_signal);

        // Open syslog configured to output to stderr concurrently
        openlog("vpn-switcher", LOG_PID | LOG_PERROR, LOG_DAEMON);
        syslog(LOG_INFO, "Starting VPN Switcher Daemon (Execution: Foreground)...");

        load_config();

        // Spawn independent network status monitoring thread
        pthread_t monitor_tid;
        if (pthread_create(&monitor_tid, NULL, network_monitor_thread, NULL) != 0) {
            syslog(LOG_ERR, "Failed to initialize active background monitoring thread.");
            return EXIT_FAILURE;
        }
        pthread_detach(monitor_tid);

        // Create IPC communication socket
        unlink(SOCKET_PATH);
        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd == -1) {
            syslog(LOG_ERR, "Failed to instantiate UNIX listener socket: %s", strerror(errno));
            return EXIT_FAILURE;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
            syslog(LOG_ERR, "Failed to bind UNIX listener socket: %s", strerror(errno));
            close(server_fd);
            return EXIT_FAILURE;
        }

        chmod(SOCKET_PATH, 0666);

        if (listen(server_fd, 10) == -1) {
            syslog(LOG_ERR, "Failed to listen on socket: %s", strerror(errno));
            close(server_fd);
            return EXIT_FAILURE;
        }

        syslog(LOG_INFO, "VPN Switcher Daemon operational.");

        while (1) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd == -1) {
                if (errno == EINTR) continue;
                syslog(LOG_ERR, "Socket accept failure: %s", strerror(errno));
                continue;
            }

            char read_buf[512] = "";
            ssize_t bytes_read = read(client_fd, read_buf, sizeof(read_buf) - 1);
            if (bytes_read > 0) {
                read_buf[bytes_read] = '\0';
                process_client_command(client_fd, read_buf);
            }
            close(client_fd);
        }

        close(server_fd);
        unlink(SOCKET_PATH);
        return EXIT_SUCCESS;
    }

    // Map command line arguments to Client IPC string queries
    char cmd_payload[1024] = "";
    if (strcmp(argv[1], "--status") == 0) {
        strcpy(cmd_payload, "STATUS");
    } else if (strcmp(argv[1], "--mode") == 0 && argc == 3) {
        snprintf(cmd_payload, sizeof(cmd_payload), "MODE %s", argv[2]);
    } else if (strcmp(argv[1], "--set") == 0 && argc == 3) {
        snprintf(cmd_payload, sizeof(cmd_payload), "SET %s", argv[2]);
    } else if (strcmp(argv[1], "--cycle") == 0) {
        strcpy(cmd_payload, "CYCLE");
    } else if (strcmp(argv[1], "--add-tailscale") == 0 && argc == 3) {
        snprintf(cmd_payload, sizeof(cmd_payload), "ADD_TAILSCALE %s", argv[2]);
    } else if (strcmp(argv[1], "--add-warp") == 0 && argc == 3) {
        snprintf(cmd_payload, sizeof(cmd_payload), "ADD_WARP %s", argv[2]);
    } else if (strcmp(argv[1], "--add-none") == 0 && argc == 3) {
        snprintf(cmd_payload, sizeof(cmd_payload), "ADD_NONE %s", argv[2]);
    } else if (strcmp(argv[1], "--remove-tailscale") == 0 && argc == 3) {
        snprintf(cmd_payload, sizeof(cmd_payload), "REMOVE_TAILSCALE %s", argv[2]);
    } else if (strcmp(argv[1], "--remove-warp") == 0 && argc == 3) {
        snprintf(cmd_payload, sizeof(cmd_payload), "REMOVE_WARP %s", argv[2]);
    } else if (strcmp(argv[1], "--remove-none") == 0 && argc == 3) {
        snprintf(cmd_payload, sizeof(cmd_payload), "REMOVE_NONE %s", argv[2]);
    } else if (strcmp(argv[1], "--set-nameserver") == 0 && argc == 3) {
        snprintf(cmd_payload, sizeof(cmd_payload), "SET_NAMESERVER %s", argv[2]);
    } else if (strcmp(argv[1], "--trigger") == 0 && argc == 4) {
        snprintf(cmd_payload, sizeof(cmd_payload), "TRIGGER %s %s", argv[2], argv[3]);
    } else {
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    send_daemon_cmd(cmd_payload);
    return EXIT_SUCCESS;
}
