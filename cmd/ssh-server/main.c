#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "internal/account.h"
#include "internal/config.h"
#include "internal/runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    ROW_SERVER_STATE = 0,
    ROW_BIND_ADDRESS,
    ROW_USERNAME,
    ROW_PASSWORD,
    ROW_START_DIR,
    ROW_COUNT
};

typedef struct {
    umrk_ssh_paths      paths;
    umrk_ssh_config     config;
    char                status[256];
    int                 last_cursor;
    int                 last_visible;
    bool                show_hints;       /* CAT_SHOW_HINTS: gate footers */
} umrk_ssh_app;

static int umrk__apply_field_change(umrk_ssh_app *app, const char *success_message);
static void umrk__handle_option_changed(umrk_ssh_app *app, cat_options_item *items, int row);

static void umrk__show_message(const char *message) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = "OK", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 1,
    };
    cat_confirm_result result;
    cat_confirmation(&opts, &result);
}

static const char *umrk__desktop_font_path(const umrk_ssh_paths *paths) {
    static char candidate[PATH_MAX];
    const char *env_font = getenv("UMRK_SSH_DESKTOP_FONT");

    if (env_font && env_font[0] && access(env_font, R_OK) == 0) {
        return env_font;
    }
    if (snprintf(candidate, sizeof(candidate), "%s/res/font.ttf", paths->app_root) < (int)sizeof(candidate) &&
        access(candidate, R_OK) == 0) {
        return candidate;
    }
    return NULL;
}

static void umrk__password_mask(const char *password, char *out, size_t out_len) {
    size_t len;

    if (!password || password[0] == '\0') {
        snprintf(out, out_len, "%s", "(unset)");
        return;
    }

    len = strlen(password);
    if (len > out_len - 1) {
        len = out_len - 1;
    }
    memset(out, '*', len);
    out[len] = '\0';
}

static int umrk__copy_checked(char *dst, size_t dst_len, const char *src,
                              char *status, size_t status_len, const char *field_name) {
    if (snprintf(dst, dst_len, "%s", src ? src : "") >= (int)dst_len) {
        snprintf(status, status_len, "%s is too long", field_name);
        return -1;
    }
    return 0;
}

static int umrk__prompt_text(const char *initial, const char *help,
                             cat_keyboard_layout layout, char *out, size_t out_len,
                             char *status, size_t status_len) {
    cat_keyboard_result result;
    int rc = cat_keyboard(initial ? initial : "", help, layout, &result);

    if (rc == CAT_OK) {
        snprintf(out, out_len, "%s", result.text);
        return 0;
    }
    if (rc == CAT_ERROR) {
        snprintf(status, status_len, "%s", "Keyboard failed");
        return -1;
    }
    return 1;
}

static int umrk__prompt_port(const umrk_ssh_config *config, char *out, size_t out_len,
                             char *status, size_t status_len) {
    char initial[16];
    int port = UMRK_SSH_DEFAULT_PORT;

    if (umrk_ssh_config_get_port(config, &port, NULL, 0) != 0) {
        port = UMRK_SSH_DEFAULT_PORT;
    }
    snprintf(initial, sizeof(initial), "%d", port);
    return umrk__prompt_text(initial,
                             "Set SSH port.\nSTART saves.\nY cancels.",
                             CAT_KB_NUMERIC,
                             out,
                             out_len,
                             status,
                             status_len);
}

static int umrk__pick_start_dir(umrk_ssh_app *app) {
    cat_file_picker_opts opts = cat_file_picker_default_opts("Select Start Folder");
    cat_file_picker_result result;
    const char *root_path = CAT_PLATFORM_IS_DEVICE ? getenv("SDCARD_PATH") : "/";
    int rc;

    if (!root_path || !root_path[0]) {
        root_path = app->config.start_dir[0] ? app->config.start_dir : "/";
    }
    opts.mode = CAT_FILE_PICKER_DIRS;
    opts.root_path = root_path;
    opts.initial_path = app->config.start_dir[0] ? app->config.start_dir : root_path;

    rc = cat_file_picker(&opts, &result);
    if (rc == CAT_OK) {
        if (umrk__copy_checked(app->config.start_dir, sizeof(app->config.start_dir), result.path,
                               app->status, sizeof(app->status), "Start folder") != 0) {
            return -1;
        }
        return umrk__apply_field_change(app, "Saved start folder");
    }
    if (rc == CAT_ERROR) {
        snprintf(app->status, sizeof(app->status), "%s", "Folder picker failed");
        return -1;
    }
    return 1;
}

static void umrk__show_status_message(const umrk_ssh_app *app, const char *title) {
    char message[512];

    if (!app || !app->status[0]) {
        return;
    }

    if (title && title[0]) {
        if (snprintf(message, sizeof(message), "%s\n\n%s", title, app->status) >= (int)sizeof(message)) {
            snprintf(message, sizeof(message), "%s", app->status);
        }
    } else {
        snprintf(message, sizeof(message), "%s", app->status);
    }

    umrk__show_message(message);
}

static int umrk__save_config(umrk_ssh_app *app) {
    return umrk_ssh_config_save(&app->paths, &app->config, app->status, sizeof(app->status));
}

static void umrk__refresh_items(umrk_ssh_app *app, cat_options_item *items,
                                cat_option values[][1], char display[][PATH_MAX]) {
    int pid = 0;
    char password[128];

    if (umrk_ssh_server_is_running(&app->paths, &pid)) {
        snprintf(display[ROW_SERVER_STATE], PATH_MAX, "%s", "On");
    } else {
        snprintf(display[ROW_SERVER_STATE], PATH_MAX, "%s", "Off");
    }
    items[ROW_SERVER_STATE].selected_option = pid > 0 ? 1 : 0;
    if (umrk_ssh_format_reachable_address(&app->config, display[ROW_BIND_ADDRESS], PATH_MAX) != 0 &&
        display[ROW_BIND_ADDRESS][0] == '\0') {
        snprintf(display[ROW_BIND_ADDRESS], PATH_MAX, "%s", "Offline");
    }
    snprintf(display[ROW_USERNAME], PATH_MAX, "%s", app->config.username[0] ? app->config.username : "(unset)");
    umrk__password_mask(app->config.password, password, sizeof(password));
    snprintf(display[ROW_PASSWORD], PATH_MAX, "%s", password);
    snprintf(display[ROW_START_DIR], PATH_MAX, "%s", app->config.start_dir[0] ? app->config.start_dir : "(unset)");

    for (int i = 1; i < ROW_COUNT; ++i) {
        values[i][0].label = display[i];
        values[i][0].value = display[i];
        items[i].options = values[i];
    }
}

static int umrk__apply_account(umrk_ssh_app *app) {
    if (umrk_ssh_apply_account(&app->config, &app->paths, app->status, sizeof(app->status)) != 0) {
        return -1;
    }
    snprintf(app->config.last_applied_username, sizeof(app->config.last_applied_username), "%s",
             app->config.username);
    if (umrk_ssh_config_save(&app->paths, &app->config, app->status, sizeof(app->status)) != 0) {
        return -1;
    }
    snprintf(app->status, sizeof(app->status), "Applied account %s and saved config", app->config.username);
    return 0;
}

static int umrk__apply_field_change(umrk_ssh_app *app, const char *success_message) {
    if (umrk__save_config(app) != 0) {
        umrk__show_status_message(app, "Save Failed");
        return -1;
    }
    snprintf(app->status, sizeof(app->status), "%s", success_message);
    return 0;
}

static void umrk__handle_selected(umrk_ssh_app *app, cat_options_item *items, int row) {
    char text[PATH_MAX];
    int port = 0;
    int result;

    switch (row) {
        case ROW_SERVER_STATE:
            items[ROW_SERVER_STATE].selected_option = items[ROW_SERVER_STATE].selected_option == 0 ? 1 : 0;
            umrk__handle_option_changed(app, items, ROW_SERVER_STATE);
            break;
        case ROW_BIND_ADDRESS:
            result = umrk__prompt_port(&app->config, text, sizeof(text), app->status, sizeof(app->status));
            if (result == 0 &&
                umrk_ssh_parse_port_text(text, &port, app->status, sizeof(app->status)) == 0 &&
                umrk_ssh_config_set_port(&app->config, port, app->status, sizeof(app->status)) == 0) {
                umrk__apply_field_change(app, "Saved port");
            } else if (result == 0) {
                umrk__show_status_message(app, "Invalid Port");
            }
            break;
        case ROW_USERNAME:
            result = umrk__prompt_text(app->config.username,
                                       "Set SSH username.\nSTART saves.\nY cancels.",
                                       CAT_KB_GENERAL,
                                       text, sizeof(text), app->status, sizeof(app->status));
            if (result == 0) {
                if (umrk__copy_checked(app->config.username, sizeof(app->config.username), text,
                                       app->status, sizeof(app->status), "Username") == 0) {
                    umrk__apply_field_change(app, "Saved username");
                }
            }
            break;
        case ROW_PASSWORD:
            result = umrk__prompt_text(app->config.password,
                                       "Set SSH password.\nSTART saves.\nY cancels.",
                                       CAT_KB_GENERAL,
                                       text, sizeof(text), app->status, sizeof(app->status));
            if (result == 0) {
                if (umrk__copy_checked(app->config.password, sizeof(app->config.password), text,
                                       app->status, sizeof(app->status), "Password") == 0) {
                    umrk__apply_field_change(app, "Saved password");
                }
            }
            break;
        case ROW_START_DIR:
            result = umrk__pick_start_dir(app);
            if (result == -1) {
                umrk__show_status_message(app, "Folder Picker Failed");
            }
            break;
        default:
            break;
    }
}

static void umrk__handle_option_changed(umrk_ssh_app *app, cat_options_item *items, int row) {
    int enable_requested;

    if (!app || row != ROW_SERVER_STATE) {
        return;
    }

    enable_requested = items[ROW_SERVER_STATE].selected_option == 1;
    if (enable_requested) {
        if (umrk__apply_account(app) == 0 &&
            umrk_ssh_server_start(&app->config, &app->paths, app->status, sizeof(app->status)) == 0) {
            return;
        }
        umrk__show_status_message(app, "Could Not Start Server");
        return;
    }

    if (umrk_ssh_server_stop(&app->paths, app->status, sizeof(app->status)) != 0) {
        umrk__show_status_message(app, "Could Not Stop Server");
    }
}

static int umrk__run_app(umrk_ssh_app *app) {
    cat_option server_options[] = {
        { .label = "Off", .value = "off" },
        { .label = "On",  .value = "on" },
    };
    cat_option values[ROW_COUNT][1];
    cat_options_item items[ROW_COUNT] = {
        { .label = "Server",         .type = CAT_OPT_STANDARD,  .options = server_options, .option_count = 2, .selected_option = 0 },
        { .label = "IP:Port",        .type = CAT_OPT_CLICKABLE, .option_count = 1, .selected_option = 0 },
        { .label = "Username",       .type = CAT_OPT_CLICKABLE, .option_count = 1, .selected_option = 0 },
        { .label = "Password",       .type = CAT_OPT_CLICKABLE, .option_count = 1, .selected_option = 0 },
        { .label = "Start Folder",   .type = CAT_OPT_CLICKABLE, .option_count = 1, .selected_option = 0 },
    };
    char display[ROW_COUNT][PATH_MAX];
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "QUIT" },
        { .button = CAT_BTN_A, .label = "Edit", .is_confirm = true },
    };
    cat_options_list_opts opts = {
        .title = "SSH Server",
        .items = items,
        .item_count = ROW_COUNT,
        .footer = footer,
        .footer_count = 2,
        .help_text =
            "A edits the selected setting.\n"
            "Server toggles on or off.\n"
            "Host keys are created automatically on first start.",
        .return_on_option_change = true,
    };

    /* No status bar in this app — the SSH server is a quick in-and-out, so the
       top screen stays clean. Still hide the footer when the user disabled
       button hints in Settings. */
    if (!app->show_hints) {
        opts.footer_count = 0;
    }

    while (1) {
        cat_options_list_result result;

        umrk__refresh_items(app, items, values, display);
        opts.initial_selected_index = app->last_cursor;
        opts.visible_start_index = app->last_visible;
        opts.help_text = app->status[0]
                             ? app->status
                             : "A edits settings. Server toggles on or off. Host keys auto-generate on first start.";

        if (cat_options_list(&opts, &result) != CAT_OK) {
            break;
        }

        app->last_cursor = result.focused_index;
        app->last_visible = result.visible_start_index;

        if (result.focused_index < 0 || result.focused_index >= ROW_COUNT) {
            continue;
        }

        if (result.action == CAT_ACTION_OPTION_CHANGED) {
            umrk__handle_option_changed(app, items, result.focused_index);
            continue;
        }

        if (result.action == CAT_ACTION_SELECTED) {
            umrk__handle_selected(app, items, result.focused_index);
        }
    }

    return 0;
}

int main(void) {
    umrk_ssh_app app;
    cat_config cfg = {0};
    const char *font_path;

    memset(&app, 0, sizeof(app));
    umrk_ssh_config_set_defaults(&app.config);

    if (umrk_ssh_paths_init(&app.paths, app.status, sizeof(app.status)) != 0) {
        fprintf(stderr, "%s\n", app.status);
        return 1;
    }

    if (umrk_ssh_ensure_dir(app.paths.state_root, 0755, app.status, sizeof(app.status)) != 0 ||
        umrk_ssh_ensure_dir(app.paths.log_dir, 0755, app.status, sizeof(app.status)) != 0) {
        fprintf(stderr, "%s\n", app.status);
        return 1;
    }

    if (umrk_ssh_config_load(&app.paths, &app.config, app.status, sizeof(app.status)) != 0) {
        fprintf(stderr, "%s\n", app.status);
        return 1;
    }

    font_path = CAT_PLATFORM_IS_DEVICE ? NULL : umrk__desktop_font_path(&app.paths);
    cfg.window_title = "SSH Server";
    cfg.font_path = font_path;
    cfg.log_path = app.paths.app_log_path;

    if (cat_init(&cfg) != CAT_OK) {
        fprintf(stderr, "cat_init failed: %s\n", cat_get_error());
        return 1;
    }

    /* Inherit hint visibility from the appearance snapshot (CAT_SHOW_HINTS,
       exported by jawakad or defaulted by env.sh). No status bar: the SSH app
       is a quick in-and-out, so the top screen stays clean. */
    app.show_hints = cat_hints_enabled_from_env();

    umrk__run_app(&app);
    cat_quit();
    return 0;
}
