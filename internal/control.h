#ifndef UMRK_SSH_CONTROL_H
#define UMRK_SSH_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

#define UMRK_SSH_SERVICE_ID "org.umrk.sshserver"

typedef struct {
    char effective_state[32];
    bool desired_enabled;
    bool has_last_exit;
    int last_exit_status;
    char last_transition_reason[128];
} umrk_ssh_service_status;

int umrk_ssh_control_status(umrk_ssh_service_status *out,
                            char *error, size_t error_len);
int umrk_ssh_control_request(const char *operation,
                             char *error, size_t error_len);
int umrk_ssh_control_logs(char *out, size_t out_len,
                          char *error, size_t error_len);

#endif
