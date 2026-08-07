#ifndef UMRK_SSH_ACCOUNT_H
#define UMRK_SSH_ACCOUNT_H

#include <stddef.h>

#include "config.h"

int umrk_ssh_apply_account(const umrk_ssh_config *cfg, const umrk_ssh_paths *paths,
                           char *status, size_t status_len);
int umrk_ssh_configure_password(umrk_ssh_config *cfg, const char *password,
                                char *status, size_t status_len);
int umrk_ssh_migrate_legacy_password(const umrk_ssh_paths *paths,
                                     umrk_ssh_config *cfg,
                                     char *status, size_t status_len);
int umrk_ssh_validate_bind_address(const char *text, char *error, size_t error_len);

#endif
