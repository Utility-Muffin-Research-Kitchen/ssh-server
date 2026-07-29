#ifndef UMRK_SSH_RUNTIME_H
#define UMRK_SSH_RUNTIME_H

#include <stddef.h>

#include "config.h"

int umrk_ssh_generate_hostkeys(const umrk_ssh_paths *paths, int *generated_out,
                               char *status, size_t status_len);
int umrk_ssh_format_reachable_address(const umrk_ssh_config *cfg, char *out, size_t out_len);
int umrk_ssh_hostkey_fingerprint(const umrk_ssh_paths *paths,
                                 char *out, size_t out_len);
int umrk_ssh_service_run(umrk_ssh_config *cfg, const umrk_ssh_paths *paths,
                         char *status, size_t status_len);

#endif
