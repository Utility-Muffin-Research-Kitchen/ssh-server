#include "runtime.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void umrk__set_error(char *error, size_t error_len, const char *fmt, ...) {
    va_list args;

    if (!error || error_len == 0 || !fmt) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(error, error_len, fmt, args);
    va_end(args);
}

static int umrk__path_join(char *out, size_t out_len, const char *base, const char *suffix) {
    if (!out || !base || !suffix) {
        return -1;
    }
    return snprintf(out, out_len, "%s/%s", base, suffix) >= (int)out_len ? -1 : 0;
}

static int umrk__resolve_executable(const char *env_name, const char *bundled_path,
                                    const char *fallback_name, char *out, size_t out_len) {
    const char *env_path = getenv(env_name);
    const char *path_env;
    char candidate[PATH_MAX];

    if (env_path && env_path[0] && access(env_path, X_OK) == 0) {
        return snprintf(out, out_len, "%s", env_path) >= (int)out_len ? -1 : 0;
    }
    if (bundled_path && bundled_path[0] && access(bundled_path, X_OK) == 0) {
        return snprintf(out, out_len, "%s", bundled_path) >= (int)out_len ? -1 : 0;
    }
    if (fallback_name[0] == '/') {
        return access(fallback_name, X_OK) == 0 &&
               snprintf(out, out_len, "%s", fallback_name) < (int)out_len ? 0 : -1;
    }

    path_env = getenv("PATH");
    if (!path_env || !path_env[0]) {
        return -1;
    }

    while (*path_env) {
        const char *sep = strchr(path_env, ':');
        size_t len = sep ? (size_t)(sep - path_env) : strlen(path_env);
        if (len > 0 && len < sizeof(candidate)) {
            memcpy(candidate, path_env, len);
            candidate[len] = '\0';
            if (snprintf(candidate + len, sizeof(candidate) - len, "/%s", fallback_name) < (int)(sizeof(candidate) - len) &&
                access(candidate, X_OK) == 0) {
                return snprintf(out, out_len, "%s", candidate) >= (int)out_len ? -1 : 0;
            }
        }
        if (!sep) {
            break;
        }
        path_env = sep + 1;
    }

    return -1;
}

static int umrk__run_and_wait(char *const argv[], char *status, size_t status_len) {
    pid_t pid;
    int wait_status;

    pid = fork();
    if (pid < 0) {
        umrk__set_error(status, status_len, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }

    if (waitpid(pid, &wait_status, 0) < 0) {
        umrk__set_error(status, status_len, "waitpid failed: %s", strerror(errno));
        return -1;
    }

    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        umrk__set_error(status, status_len, "%s failed", argv[0]);
        return -1;
    }

    return 0;
}

static int umrk__pidfile_path(const umrk_ssh_paths *paths, char *out, size_t out_len) {
    return umrk__path_join(out, out_len, paths->run_dir, "dropbear.pid");
}

static bool umrk__is_pid_dir(const char *name) {
    if (!name || !name[0]) {
        return false;
    }
    for (size_t i = 0; name[i] != '\0'; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

static int umrk__read_proc_cmdline(int pid, char *out, size_t out_len) {
    char path[64];
    FILE *fp;
    size_t n;

    if (!out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    if (snprintf(path, sizeof(path), "/proc/%d/cmdline", pid) >= (int)sizeof(path)) {
        return -1;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    n = fread(out, 1, out_len - 1, fp);
    fclose(fp);
    if (n == 0) {
        return -1;
    }

    for (size_t i = 0; i < n; ++i) {
        if (out[i] == '\0') {
            out[i] = ' ';
        }
    }
    out[n] = '\0';
    return 0;
}

static bool umrk__cmdline_matches_dropbear(const umrk_ssh_paths *paths, const char *cmdline) {
    if (!paths || !cmdline || !strstr(cmdline, "dropbear")) {
        return false;
    }
    if (paths->bundled_dropbear[0] && strstr(cmdline, paths->bundled_dropbear)) {
        return true;
    }
    if (paths->hostkeys_dir[0] && strstr(cmdline, paths->hostkeys_dir)) {
        return true;
    }
    return false;
}

static int umrk__find_running_dropbear(const umrk_ssh_paths *paths, int *pid_out) {
    DIR *dir;
    struct dirent *entry;

    if (!paths) {
        return 0;
    }

    dir = opendir("/proc");
    if (!dir) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        char cmdline[4096];
        int pid;

        if (!umrk__is_pid_dir(entry->d_name)) {
            continue;
        }
        pid = atoi(entry->d_name);
        if (pid <= 0 || kill(pid, 0) != 0) {
            continue;
        }
        if (umrk__read_proc_cmdline(pid, cmdline, sizeof(cmdline)) != 0) {
            continue;
        }
        if (!umrk__cmdline_matches_dropbear(paths, cmdline)) {
            continue;
        }

        closedir(dir);
        if (pid_out) {
            *pid_out = pid;
        }
        return 1;
    }

    closedir(dir);
    return 0;
}

static void umrk__record_pidfile(const umrk_ssh_paths *paths, int pid) {
    char pidfile[PATH_MAX];
    FILE *fp;

    if (!paths || pid <= 0 || umrk__pidfile_path(paths, pidfile, sizeof(pidfile)) != 0) {
        return;
    }
    if (umrk_ssh_ensure_dir(paths->run_dir, 0755, NULL, 0) != 0) {
        return;
    }
    fp = fopen(pidfile, "w");
    if (!fp) {
        return;
    }
    fprintf(fp, "%d\n", pid);
    fclose(fp);
}

static int umrk__interface_priority(const char *name) {
    if (!name) {
        return 0;
    }
    if (strcmp(name, "wlan0") == 0) {
        return 3;
    }
    if (strcmp(name, "ap0") == 0 || strcmp(name, "eth0") == 0) {
        return 2;
    }
    return 1;
}

static int umrk__detect_reachable_ip(char *out, size_t out_len) {
    const char *override = getenv("UMRK_SSH_DEVICE_IP");

    if (!out || out_len == 0) {
        return -1;
    }

    if (override && override[0] != '\0') {
        return snprintf(out, out_len, "%s", override) >= (int)out_len ? -1 : 0;
    }

#if defined(PLATFORM_MAC)
    return snprintf(out, out_len, "%s", "127.0.0.1") >= (int)out_len ? -1 : 0;
#else
    {
        struct ifaddrs *ifaddr = NULL;
        struct ifaddrs *ifa = NULL;
        char best_addr[64] = {0};
        int best_priority = 0;

        if (getifaddrs(&ifaddr) != 0) {
            return -1;
        }

        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            int priority;

            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
                continue;
            }

            priority = umrk__interface_priority(ifa->ifa_name);
            if (priority < best_priority) {
                continue;
            }
            if (!inet_ntop(AF_INET,
                           &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                           best_addr,
                           (socklen_t)sizeof(best_addr))) {
                continue;
            }

            best_priority = priority;
            if (priority >= 3) {
                break;
            }
        }

        freeifaddrs(ifaddr);
        if (best_addr[0] == '\0') {
            return -1;
        }
        return snprintf(out, out_len, "%s", best_addr) >= (int)out_len ? -1 : 0;
    }
#endif
}

int umrk_ssh_server_is_running(const umrk_ssh_paths *paths, int *pid_out) {
    FILE *fp;
    char pidfile[PATH_MAX];
    int pid = 0;

    if (!paths || umrk__pidfile_path(paths, pidfile, sizeof(pidfile)) != 0) {
        return 0;
    }

    fp = fopen(pidfile, "r");
    if (!fp) {
        if (umrk__find_running_dropbear(paths, &pid)) {
            umrk__record_pidfile(paths, pid);
            if (pid_out) {
                *pid_out = pid;
            }
            return 1;
        }
        return 0;
    }
    if (fscanf(fp, "%d", &pid) != 1 || pid <= 0) {
        fclose(fp);
        if (umrk__find_running_dropbear(paths, &pid)) {
            umrk__record_pidfile(paths, pid);
            if (pid_out) {
                *pid_out = pid;
            }
            return 1;
        }
        return 0;
    }
    fclose(fp);

    if (kill(pid, 0) != 0) {
        if (!umrk__find_running_dropbear(paths, &pid)) {
            return 0;
        }
        umrk__record_pidfile(paths, pid);
        if (pid_out) {
            *pid_out = pid;
        }
        return 1;
    }

    {
        char cmdline[4096];
        if (umrk__read_proc_cmdline(pid, cmdline, sizeof(cmdline)) != 0 ||
            !umrk__cmdline_matches_dropbear(paths, cmdline)) {
            if (!umrk__find_running_dropbear(paths, &pid)) {
                return 0;
            }
            umrk__record_pidfile(paths, pid);
        }
    }

    if (pid_out) {
        *pid_out = pid;
    }
    return 1;
}

int umrk_ssh_generate_hostkeys(const umrk_ssh_paths *paths, int *generated_out,
                               char *status, size_t status_len) {
    char dropbearkey[PATH_MAX];
    char ed25519[PATH_MAX];
    char rsa[PATH_MAX];
    int generated_any = 0;

    if (!paths) {
        umrk__set_error(status, status_len, "%s", "missing runtime paths");
        return -1;
    }
    if (generated_out) {
        *generated_out = 0;
    }

    if (umrk_ssh_ensure_dir(paths->hostkeys_dir, 0700, status, status_len) != 0) {
        return -1;
    }

    if (umrk__path_join(ed25519, sizeof(ed25519), paths->hostkeys_dir, "dropbear_ed25519_host_key") != 0 ||
        umrk__path_join(rsa, sizeof(rsa), paths->hostkeys_dir, "dropbear_rsa_host_key") != 0) {
        umrk__set_error(status, status_len, "%s", "hostkey path too long");
        return -1;
    }

    if (access(ed25519, R_OK) == 0 && access(rsa, R_OK) == 0) {
        snprintf(status, status_len, "%s", "Host keys already present");
        return 0;
    }

    if (umrk__resolve_executable("UMRK_SSH_DROPBEARKEY_BIN", paths->bundled_dropbearkey,
                                 "dropbearkey", dropbearkey, sizeof(dropbearkey)) != 0) {
        umrk__set_error(status, status_len, "%s", "dropbearkey not found");
        return -1;
    }

    if (access(ed25519, R_OK) != 0) {
        char *argv[] = { dropbearkey, "-t", "ed25519", "-f", ed25519, NULL };
        if (umrk__run_and_wait(argv, status, status_len) != 0) {
            return -1;
        }
        generated_any = 1;
    }

    if (access(rsa, R_OK) != 0) {
        char *argv[] = { dropbearkey, "-t", "rsa", "-s", "4096", "-f", rsa, NULL };
        if (umrk__run_and_wait(argv, status, status_len) != 0) {
            return -1;
        }
        generated_any = 1;
    }

    if (generated_out) {
        *generated_out = generated_any;
    }
    snprintf(status, status_len, "%s", "Host keys ready");
    return 0;
}

int umrk_ssh_format_reachable_address(const umrk_ssh_config *cfg, char *out, size_t out_len) {
    char ip[64];
    int port = 0;

    if (!cfg || !out || out_len == 0) {
        return -1;
    }
    if (umrk_ssh_config_get_port(cfg, &port, NULL, 0) != 0) {
        snprintf(out, out_len, "%s", "Invalid port");
        return -1;
    }
    if (umrk__detect_reachable_ip(ip, sizeof(ip)) == 0) {
        return snprintf(out, out_len, "%s:%d", ip, port) >= (int)out_len ? -1 : 0;
    }
    return snprintf(out, out_len, "Offline (port %d)", port) >= (int)out_len ? -1 : 1;
}

int umrk_ssh_server_start(const umrk_ssh_config *cfg, const umrk_ssh_paths *paths,
                          char *status, size_t status_len) {
    char address[128];
    char dropbear[PATH_MAX];
    char ed25519[PATH_MAX];
    char rsa[PATH_MAX];
    char pidfile[PATH_MAX];
    pid_t pid;
    int wait_status;
    int server_pid = 0;
    int generated_hostkeys = 0;
    int port = 0;

    if (!cfg || !paths) {
        umrk__set_error(status, status_len, "%s", "missing start input");
        return -1;
    }

    if (umrk_ssh_server_is_running(paths, &server_pid)) {
        snprintf(status, status_len, "Server already running (pid %d)", server_pid);
        return 0;
    }

    if (umrk_ssh_ensure_dir(paths->state_root, 0755, status, status_len) != 0 ||
        umrk_ssh_ensure_dir(paths->run_dir, 0755, status, status_len) != 0 ||
        umrk_ssh_ensure_dir(paths->log_dir, 0755, status, status_len) != 0) {
        return -1;
    }

    if (umrk_ssh_generate_hostkeys(paths, &generated_hostkeys, status, status_len) != 0) {
        return -1;
    }

    if (umrk__resolve_executable("UMRK_SSH_DROPBEAR_BIN", paths->bundled_dropbear,
                                 "dropbear", dropbear, sizeof(dropbear)) != 0) {
        umrk__set_error(status, status_len, "%s", "dropbear not found");
        return -1;
    }

    if (umrk__path_join(ed25519, sizeof(ed25519), paths->hostkeys_dir, "dropbear_ed25519_host_key") != 0 ||
        umrk__path_join(rsa, sizeof(rsa), paths->hostkeys_dir, "dropbear_rsa_host_key") != 0 ||
        umrk__pidfile_path(paths, pidfile, sizeof(pidfile)) != 0) {
        umrk__set_error(status, status_len, "%s", "runtime path too long");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        umrk__set_error(status, status_len, "fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        char *argv[] = {
            dropbear,
            "-p", (char *)cfg->bind_address,
            "-P", pidfile,
            "-r", ed25519,
            "-r", rsa,
            NULL
        };
        execv(dropbear, argv);
        _exit(127);
    }

    if (waitpid(pid, &wait_status, 0) < 0) {
        umrk__set_error(status, status_len, "waitpid failed: %s", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        umrk__set_error(status, status_len, "%s", "dropbear launch failed");
        return -1;
    }

    for (int i = 0; i < 10; ++i) {
        if (umrk_ssh_server_is_running(paths, &server_pid)) {
            if (umrk_ssh_format_reachable_address(cfg, address, sizeof(address)) == 0) {
                snprintf(status,
                         status_len,
                         generated_hostkeys ? "Generated host keys and started server on %s (pid %d)"
                                            : "Server running on %s (pid %d)",
                         address,
                         server_pid);
            } else if (umrk_ssh_config_get_port(cfg, &port, NULL, 0) == 0) {
                snprintf(status,
                         status_len,
                         generated_hostkeys ? "Generated host keys and started server; reachable IP unavailable (port %d, pid %d)"
                                            : "Server running; reachable IP unavailable (port %d, pid %d)",
                         port,
                         server_pid);
            } else {
                snprintf(status,
                         status_len,
                         generated_hostkeys ? "Generated host keys and started server (pid %d)"
                                            : "Server running (pid %d)",
                         server_pid);
            }
            return 0;
        }
        usleep(100000);
    }

    umrk__set_error(status, status_len, "%s", "dropbear exited without a pidfile");
    return -1;
}

int umrk_ssh_server_stop(const umrk_ssh_paths *paths, char *status, size_t status_len) {
    int pid = 0;
    char pidfile[PATH_MAX];

    if (!paths) {
        umrk__set_error(status, status_len, "%s", "missing stop input");
        return -1;
    }

    if (!umrk_ssh_server_is_running(paths, &pid)) {
        snprintf(status, status_len, "%s", "Server is not running");
        return 0;
    }

    if (kill(pid, SIGTERM) != 0) {
        umrk__set_error(status, status_len, "kill(%d) failed: %s", pid, strerror(errno));
        return -1;
    }

    for (int i = 0; i < 20; ++i) {
        if (kill(pid, 0) != 0) {
            if (umrk__pidfile_path(paths, pidfile, sizeof(pidfile)) == 0) {
                unlink(pidfile);
            }
            snprintf(status, status_len, "Stopped server pid %d", pid);
            return 0;
        }
        usleep(100000);
    }

    umrk__set_error(status, status_len, "server pid %d did not exit", pid);
    return -1;
}
