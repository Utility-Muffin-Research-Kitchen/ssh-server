#include "control.h"

#include "cJSON.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define UMRK_CTL1_MAX_PAYLOAD (64u * 1024u)

static void umrk__set_error(char *error, size_t error_len, const char *message) {
    if (error && error_len > 0) {
        snprintf(error, error_len, "%s", message ? message : "control request failed");
    }
}

static int umrk__socket_path(char *out, size_t out_len) {
    const char *explicit_path = getenv("JAWAKA_SOCKET_PATH");
    const char *runtime_path = getenv("UMRK_RUNTIME_PATH");

    if (explicit_path && explicit_path[0]) {
        return snprintf(out, out_len, "%s", explicit_path) >= (int)out_len ? -1 : 0;
    }
    if (runtime_path && runtime_path[0]) {
        return snprintf(out, out_len, "%s/jawakad.sock", runtime_path) >= (int)out_len ? -1 : 0;
    }
    return snprintf(out, out_len, "%s", "/tmp/jawaka-runtime/jawakad.sock") >= (int)out_len ? -1 : 0;
}

static int umrk__write_all(int fd, const void *buffer, size_t length) {
    const unsigned char *cursor = buffer;

    while (length > 0) {
#if defined(MSG_NOSIGNAL)
        ssize_t written = send(fd, cursor, length, MSG_NOSIGNAL);
#else
        ssize_t written = send(fd, cursor, length, 0);
#endif
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return -1;
        }
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int umrk__read_all(int fd, void *buffer, size_t length) {
    unsigned char *cursor = buffer;

    while (length > 0) {
        ssize_t received = recv(fd, cursor, length, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return -1;
        }
        cursor += (size_t)received;
        length -= (size_t)received;
    }
    return 0;
}

static cJSON *umrk__exchange(cJSON *request, char *error, size_t error_len) {
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un address;
    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    char *request_json = NULL;
    char *response_json = NULL;
    cJSON *response = NULL;
    uint32_t frame_length;
    int fd = -1;

    request_json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!request_json) {
        umrk__set_error(error, error_len, "could not encode control request");
        return NULL;
    }
    size_t request_len = strlen(request_json);
    if (request_len == 0 || request_len > UMRK_CTL1_MAX_PAYLOAD ||
        umrk__socket_path(socket_path, sizeof(socket_path)) != 0) {
        cJSON_free(request_json);
        umrk__set_error(error, error_len, "invalid control request or socket path");
        return NULL;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        cJSON_free(request_json);
        umrk__set_error(error, error_len, "could not create control socket");
        return NULL;
    }
#if defined(SO_NOSIGPIPE)
    {
        int enabled = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
    }
#endif
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        cJSON_free(request_json);
        umrk__set_error(error, error_len, "Jawaka service control is unavailable");
        return NULL;
    }

    frame_length = htonl((uint32_t)request_len);
    if (umrk__write_all(fd, &frame_length, sizeof(frame_length)) != 0 ||
        umrk__write_all(fd, request_json, request_len) != 0 ||
        umrk__read_all(fd, &frame_length, sizeof(frame_length)) != 0) {
        close(fd);
        cJSON_free(request_json);
        umrk__set_error(error, error_len, "Jawaka control exchange failed");
        return NULL;
    }
    cJSON_free(request_json);

    size_t response_len = (size_t)ntohl(frame_length);
    if (response_len == 0 || response_len > UMRK_CTL1_MAX_PAYLOAD) {
        close(fd);
        umrk__set_error(error, error_len, "Jawaka returned an invalid control frame");
        return NULL;
    }
    response_json = malloc(response_len + 1u);
    if (!response_json || umrk__read_all(fd, response_json, response_len) != 0) {
        close(fd);
        free(response_json);
        umrk__set_error(error, error_len, "Jawaka control response was incomplete");
        return NULL;
    }
    close(fd);
    response_json[response_len] = '\0';

    const char *parse_end = NULL;
    response = cJSON_ParseWithLengthOpts(response_json, response_len, &parse_end, false);
    if (!response || parse_end != response_json + response_len) {
        cJSON_Delete(response);
        response = NULL;
        umrk__set_error(error, error_len, "Jawaka returned malformed control JSON");
    }
    free(response_json);
    return response;
}

static cJSON *umrk__request(const char *operation) {
    static unsigned long request_counter;
    char request_id[64];
    cJSON *request = cJSON_CreateObject();

    request_counter++;
    snprintf(request_id, sizeof(request_id), "ssh-%ld-%lu", (long)getpid(), request_counter);
    if (!request ||
        !cJSON_AddNumberToObject(request, "v", 1) ||
        !cJSON_AddStringToObject(request, "op", operation) ||
        !cJSON_AddStringToObject(request, "id", request_id) ||
        (strcmp(operation, "list") != 0 && strcmp(operation, "capabilities") != 0 &&
         !cJSON_AddStringToObject(request, "service_id", UMRK_SSH_SERVICE_ID))) {
        cJSON_Delete(request);
        return NULL;
    }
    return request;
}

static int umrk__response_error(const cJSON *response, char *error, size_t error_len) {
    const cJSON *error_object = cJSON_GetObjectItemCaseSensitive(response, "error");
    const cJSON *message = error_object
        ? cJSON_GetObjectItemCaseSensitive(error_object, "message") : NULL;

    umrk__set_error(error, error_len,
                    cJSON_IsString(message) ? message->valuestring : "Jawaka rejected the control request");
    return -1;
}

int umrk_ssh_control_status(umrk_ssh_service_status *out,
                            char *error, size_t error_len) {
    if (!out) {
        umrk__set_error(error, error_len, "missing status output");
        return -1;
    }
    memset(out, 0, sizeof(*out));

    cJSON *request = umrk__request("status");
    cJSON *response = request ? umrk__exchange(request, error, error_len) : NULL;
    if (!response) {
        return -1;
    }
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(response, "effective_state");
    const cJSON *desired = cJSON_GetObjectItemCaseSensitive(response, "desired_enabled");
    const cJSON *last_exit = cJSON_GetObjectItemCaseSensitive(response, "last_exit");
    const cJSON *transition = cJSON_GetObjectItemCaseSensitive(response, "last_transition");
    if (!cJSON_IsString(state) || !cJSON_IsBool(desired) ||
        !cJSON_IsObject(last_exit) || !cJSON_IsObject(transition)) {
        int rc = umrk__response_error(response, error, error_len);
        cJSON_Delete(response);
        return rc;
    }
    snprintf(out->effective_state, sizeof(out->effective_state), "%s", state->valuestring);
    out->desired_enabled = cJSON_IsTrue(desired);
    const cJSON *exit_status = cJSON_GetObjectItemCaseSensitive(last_exit, "status");
    if (cJSON_IsNumber(exit_status)) {
        out->has_last_exit = true;
        out->last_exit_status = exit_status->valueint;
    }
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(transition, "reason");
    if (cJSON_IsString(reason)) {
        snprintf(out->last_transition_reason, sizeof(out->last_transition_reason), "%s", reason->valuestring);
    }
    cJSON_Delete(response);
    return 0;
}

int umrk_ssh_control_request(const char *operation,
                             char *error, size_t error_len) {
    if (!operation ||
        (strcmp(operation, "run") != 0 && strcmp(operation, "stop") != 0 &&
         strcmp(operation, "restart") != 0 && strcmp(operation, "enable") != 0 &&
         strcmp(operation, "disable") != 0)) {
        umrk__set_error(error, error_len, "invalid service control operation");
        return -1;
    }
    cJSON *request = umrk__request(operation);
    cJSON *response = request ? umrk__exchange(request, error, error_len) : NULL;
    if (!response) {
        return -1;
    }
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    if (!cJSON_IsTrue(ok)) {
        int rc = umrk__response_error(response, error, error_len);
        cJSON_Delete(response);
        return rc;
    }
    cJSON_Delete(response);
    return 0;
}

int umrk_ssh_control_logs(char *out, size_t out_len,
                          char *error, size_t error_len) {
    if (!out || out_len == 0) {
        umrk__set_error(error, error_len, "missing log output");
        return -1;
    }
    out[0] = '\0';
    cJSON *request = umrk__request("logs");
    if (!request || !cJSON_AddNumberToObject(request, "tail", 4)) {
        cJSON_Delete(request);
        umrk__set_error(error, error_len, "could not encode log request");
        return -1;
    }
    cJSON *response = umrk__exchange(request, error, error_len);
    if (!response) {
        return -1;
    }
    const cJSON *lines = cJSON_GetObjectItemCaseSensitive(response, "lines");
    if (!cJSON_IsArray(lines)) {
        int rc = umrk__response_error(response, error, error_len);
        cJSON_Delete(response);
        return rc;
    }
    size_t used = 0;
    const cJSON *line = NULL;
    cJSON_ArrayForEach(line, lines) {
        if (!cJSON_IsString(line)) {
            continue;
        }
        int written = snprintf(out + used, out_len - used, "%s%s",
                               used ? "\n" : "", line->valuestring);
        if (written < 0 || (size_t)written >= out_len - used) {
            break;
        }
        used += (size_t)written;
    }
    cJSON_Delete(response);
    return 0;
}
