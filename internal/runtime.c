#include "runtime.h"

#include "account.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
#if defined(__linux__)
#include <sys/prctl.h>
#endif
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
    pid_t expected_parent = getpid();
    int wait_status;
#if !defined(__linux__)
    (void)expected_parent;
#endif

    pid = fork();
    if (pid < 0) {
        umrk__set_error(status, status_len, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
#if defined(__linux__)
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != expected_parent) {
            _exit(127);
        }
#endif
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


static int umrk__interface_priority(const char *name) {
    if (!name) {
        return 0;
    }
    const char *preferred = getenv("UMRK_SSH_PRIMARY_IFACE");
    if (preferred && preferred[0] && strcmp(name, preferred) == 0) {
        return 3;
    }
    if (strcmp(name, "ap0") == 0 || strcmp(name, "eth0") == 0) {
        return 2;
    }
    return 1;
}

static int umrk__detect_reachable_ip(char *out, size_t out_len, int *family_out) {
    const char *override = getenv("UMRK_SSH_DEVICE_IP");

    if (!out || out_len == 0) {
        return -1;
    }

    if (override && override[0] != '\0') {
        if (family_out) {
            *family_out = strchr(override, ':') ? AF_INET6 : AF_INET;
        }
        return snprintf(out, out_len, "%s", override) >= (int)out_len ? -1 : 0;
    }

#if defined(PLATFORM_MAC)
    if (family_out) {
        *family_out = AF_INET;
    }
    return snprintf(out, out_len, "%s", "127.0.0.1") >= (int)out_len ? -1 : 0;
#else
    {
        struct ifaddrs *ifaddr = NULL;
        struct ifaddrs *ifa = NULL;
        char best_v4[INET_ADDRSTRLEN] = {0};
        char best_v6[INET6_ADDRSTRLEN] = {0};
        int best_v4_priority = 0;
        int best_v6_priority = 0;

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
            if (priority < best_v4_priority) {
                continue;
            }
            if (!inet_ntop(AF_INET,
                           &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                           best_v4,
                           (socklen_t)sizeof(best_v4))) {
                continue;
            }

            best_v4_priority = priority;
            if (priority >= 3) {
                break;
            }
        }

        if (best_v4[0] == '\0') {
            for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
                const struct in6_addr *addr;
                int priority;

                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET6) {
                    continue;
                }
                if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
                    continue;
                }

                addr = &((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
                if (IN6_IS_ADDR_UNSPECIFIED(addr) ||
                    IN6_IS_ADDR_LOOPBACK(addr) ||
                    IN6_IS_ADDR_LINKLOCAL(addr) ||
                    IN6_IS_ADDR_MULTICAST(addr)) {
                    continue;
                }

                priority = umrk__interface_priority(ifa->ifa_name);
                if (priority < best_v6_priority) {
                    continue;
                }
                if (!inet_ntop(AF_INET6, addr, best_v6, (socklen_t)sizeof(best_v6))) {
                    continue;
                }

                best_v6_priority = priority;
                if (priority >= 3) {
                    break;
                }
            }
        }

        freeifaddrs(ifaddr);
        if (best_v4[0] != '\0') {
            if (family_out) {
                *family_out = AF_INET;
            }
            return snprintf(out, out_len, "%s", best_v4) >= (int)out_len ? -1 : 0;
        }
        if (best_v6[0] != '\0') {
            if (family_out) {
                *family_out = AF_INET6;
            }
            return snprintf(out, out_len, "%s", best_v6) >= (int)out_len ? -1 : 0;
        }
        return -1;
    }
#endif
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
    char ip[128];
    int family = AF_UNSPEC;
    int port = 0;

    if (!cfg || !out || out_len == 0) {
        return -1;
    }
    if (umrk_ssh_config_get_port(cfg, &port, NULL, 0) != 0) {
        snprintf(out, out_len, "%s", "Invalid port");
        return -1;
    }
    if (umrk__detect_reachable_ip(ip, sizeof(ip), &family) == 0) {
        if (family == AF_INET6) {
            return snprintf(out, out_len, "[%s]:%d", ip, port) >= (int)out_len ? -1 : 0;
        }
        return snprintf(out, out_len, "%s:%d", ip, port) >= (int)out_len ? -1 : 0;
    }
    return snprintf(out, out_len, "Offline (port %d)", port) >= (int)out_len ? -1 : 1;
}

int umrk_ssh_hostkey_fingerprint(const umrk_ssh_paths *paths,
                                 char *out, size_t out_len) {
    char dropbearkey[PATH_MAX];
    char ed25519[PATH_MAX];
    int pipefd[2];
    pid_t pid;
    pid_t expected_parent = getpid();
    int wait_status;
#if !defined(__linux__)
    (void)expected_parent;
#endif
    char buffer[4096];
    size_t used = 0;

    if (!paths || !out || out_len == 0 ||
        umrk__path_join(ed25519, sizeof(ed25519), paths->hostkeys_dir,
                        "dropbear_ed25519_host_key") != 0 ||
        access(ed25519, R_OK) != 0) {
        return -1;
    }
    if (umrk__resolve_executable("UMRK_SSH_DROPBEARKEY_BIN", paths->bundled_dropbearkey,
                                 "dropbearkey", dropbearkey, sizeof(dropbearkey)) != 0 ||
        pipe(pipefd) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        char *argv[] = { dropbearkey, "-y", "-f", ed25519, NULL };
#if defined(__linux__)
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != expected_parent) {
            _exit(127);
        }
#endif
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);
        execv(dropbearkey, argv);
        _exit(127);
    }
    close(pipefd[1]);
    while (used + 1u < sizeof(buffer)) {
        ssize_t amount = read(pipefd[0], buffer + used, sizeof(buffer) - used - 1u);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            break;
        }
        used += (size_t)amount;
    }
    close(pipefd[0]);
    if (waitpid(pid, &wait_status, 0) < 0 ||
        !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        return -1;
    }
    buffer[used] = '\0';
    const char *fingerprint = strstr(buffer, "Fingerprint: ");
    if (!fingerprint) {
        return -1;
    }
    fingerprint += strlen("Fingerprint: ");
    size_t length = strcspn(fingerprint, "\r\n");
    if (length == 0 || length >= out_len) {
        return -1;
    }
    memcpy(out, fingerprint, length);
    out[length] = '\0';
    return 0;
}

int umrk_ssh_service_run(umrk_ssh_config *cfg, const umrk_ssh_paths *paths,
                         char *status, size_t status_len) {
    char dropbear[PATH_MAX];
    char ed25519[PATH_MAX];
    char rsa[PATH_MAX];
    const char *lease = getenv("UMRK_SERVICE_LEASE_FD");
    int generated_hostkeys = 0;

    if (!cfg || !paths) {
        umrk__set_error(status, status_len, "%s", "missing service run input");
        return -1;
    }
    if (!lease || strcmp(lease, "3") != 0 || fcntl(3, F_GETFD) < 0) {
        umrk__set_error(status, status_len, "%s", "service run requires Jawaka generation lease fd 3");
        return -1;
    }
    if (fcntl(3, F_SETFD, 0) != 0) {
        umrk__set_error(status, status_len, "%s", "could not preserve generation lease fd 3");
        return -1;
    }
#if defined(__linux__)
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() == 1) {
        umrk__set_error(status, status_len, "%s", "could not arm service parent-death protection");
        return -1;
    }
#endif

    if (umrk_ssh_migrate_legacy_password(paths, cfg, status, status_len) != 0 ||
        umrk_ssh_apply_account(cfg, paths, status, status_len) != 0) {
        return -1;
    }
    if (strcmp(cfg->last_applied_username, cfg->username) != 0) {
        snprintf(cfg->last_applied_username, sizeof(cfg->last_applied_username),
                 "%s", cfg->username);
        if (umrk_ssh_config_save(paths, cfg, status, status_len) != 0) {
            return -1;
        }
    }
    if (umrk_ssh_ensure_dir(paths->state_root, 0755, status, status_len) != 0 ||
        umrk_ssh_ensure_dir(paths->log_dir, 0755, status, status_len) != 0 ||
        umrk_ssh_generate_hostkeys(paths, &generated_hostkeys, status, status_len) != 0 ||
        umrk__resolve_executable("UMRK_SSH_DROPBEAR_BIN", paths->bundled_dropbear,
                                 "dropbear", dropbear, sizeof(dropbear)) != 0 ||
        umrk__path_join(ed25519, sizeof(ed25519), paths->hostkeys_dir,
                        "dropbear_ed25519_host_key") != 0 ||
        umrk__path_join(rsa, sizeof(rsa), paths->hostkeys_dir,
                        "dropbear_rsa_host_key") != 0) {
        umrk__set_error(status, status_len, "%s", "could not prepare Dropbear runtime");
        return -1;
    }

    char *argv[16];
    int argc = 0;
    argv[argc++] = dropbear;
    argv[argc++] = "-F";
    argv[argc++] = "-P";
    argv[argc++] = "/dev/null";
    argv[argc++] = "-p";
    argv[argc++] = cfg->bind_address;
    argv[argc++] = "-r";
    argv[argc++] = ed25519;
    argv[argc++] = "-r";
    argv[argc++] = rsa;
    if (!cfg->password_auth_enabled) {
        argv[argc++] = "-s";
    }
    argv[argc] = NULL;

    (void)generated_hostkeys;
    execv(dropbear, argv);
    umrk__set_error(status, status_len, "exec %s failed: %s", dropbear, strerror(errno));
    return -1;
}
