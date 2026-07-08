#define _POSIX_C_SOURCE 200809L

#include "calibration.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CALIBRATION_JSON_MAX_BYTES (4U * 1024U * 1024U)
#define WAIT_POLL_MS 100U
#define TERM_GRACE_MS 2000U

struct json_range {
    const char *begin;
    const char *end; /* one past last byte */
};

static void set_error(struct calibration_result *out, const char *fmt, ...)
{
    va_list ap;

    if (out == NULL) {
        return;
    }

    va_start(ap, fmt);
    (void)vsnprintf(out->error_message,
                    sizeof(out->error_message),
                    fmt,
                    ap);
    va_end(ap);
}

void calibration_result_reset(struct calibration_result *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->worker_exit_code = -1;
}

static int validate_config(const struct calibration_config *cfg)
{
    if (cfg == NULL ||
        cfg->worker_path == NULL ||
        cfg->result_path == NULL ||
        cfg->work_dir == NULL) {
        return CALIBRATION_ERR_INVALID_ARG;
    }

    if (cfg->worker_path[0] == '\0' ||
        cfg->result_path[0] == '\0' ||
        cfg->work_dir[0] == '\0' ||
        cfg->runs == 0U ||
        cfg->measurements == 0U ||
        cfg->timing_rounds == 0U ||
        cfg->memory_percent <= 0.0 ||
        cfg->memory_percent > 100.0) {
        return CALIBRATION_ERR_INVALID_ARG;
    }

    return CALIBRATION_OK;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }

    return (long long)ts.tv_sec * 1000LL +
           (long long)ts.tv_nsec / 1000000LL;
}

static void sleep_ms(unsigned int ms)
{
    struct timespec req;

    req.tv_sec = (time_t)(ms / 1000U);
    req.tv_nsec = (long)((ms % 1000U) * 1000000UL);

    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
        /* retry remaining time */
    }
}

static void terminate_process_group(pid_t pid)
{
    long long deadline;

    if (pid <= 0) {
        return;
    }

    (void)kill(-pid, SIGTERM);

    deadline = monotonic_ms();
    if (deadline >= 0) {
        deadline += (long long)TERM_GRACE_MS;
    }

    for (;;) {
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);

        if (rc == pid) {
            return;
        }

        if (rc < 0 && errno != EINTR) {
            break;
        }

        if (deadline >= 0 && monotonic_ms() >= deadline) {
            break;
        }

        sleep_ms(WAIT_POLL_MS);
    }

    (void)kill(-pid, SIGKILL);

    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        /* retry */
    }
}

static int wait_child(pid_t pid,
                      unsigned int timeout_sec,
                      int *exit_code)
{
    long long start_ms = -1;
    long long timeout_ms = 0;

    if (exit_code == NULL) {
        return CALIBRATION_ERR_INVALID_ARG;
    }

    if (timeout_sec > 0U) {
        start_ms = monotonic_ms();
        if (start_ms < 0) {
            return CALIBRATION_ERR_WAIT;
        }
        timeout_ms = (long long)timeout_sec * 1000LL;
    }

    for (;;) {
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);

        if (rc == pid) {
            if (WIFEXITED(status)) {
                *exit_code = WEXITSTATUS(status);
                return CALIBRATION_OK;
            }

            if (WIFSIGNALED(status)) {
                *exit_code = 128 + WTERMSIG(status);
                return CALIBRATION_OK;
            }

            return CALIBRATION_ERR_WAIT;
        }

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return CALIBRATION_ERR_WAIT;
        }

        if (timeout_sec > 0U) {
            long long now_ms = monotonic_ms();

            if (now_ms < 0) {
                terminate_process_group(pid);
                return CALIBRATION_ERR_WAIT;
            }

            if (now_ms - start_ms >= timeout_ms) {
                terminate_process_group(pid);
                return CALIBRATION_ERR_TIMEOUT;
            }
        }

        sleep_ms(WAIT_POLL_MS);
    }
}

static int redirect_child_log(const char *log_path)
{
    int fd;

    if (log_path == NULL) {
        return CALIBRATION_OK;
    }

    fd = open(log_path,
              O_WRONLY | O_CREAT | O_TRUNC,
              0640);
    if (fd < 0) {
        return CALIBRATION_ERR_IO;
    }

    if (dup2(fd, STDOUT_FILENO) < 0 ||
        dup2(fd, STDERR_FILENO) < 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return CALIBRATION_ERR_IO;
    }

    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        (void)close(fd);
    }

    return CALIBRATION_OK;
}

static int launch_worker(const struct calibration_config *cfg,
                         pid_t *child_pid)
{
    pid_t pid;

    if (child_pid == NULL) {
        return CALIBRATION_ERR_INVALID_ARG;
    }

    pid = fork();
    if (pid < 0) {
        return CALIBRATION_ERR_FORK;
    }

    if (pid == 0) {
        char runs_buf[32];
        char mem_buf[64];
        char measurements_buf[32];
        char timing_rounds_buf[32];
        char *argv[32];
        size_t n = 0U;

        /*
         * Separate process group so timeout can terminate Python worker
         * and any Knock-Knock collectors it spawned.
         */
        if (setpgid(0, 0) != 0) {
            _exit(126);
        }

        if (redirect_child_log(cfg->log_path) != CALIBRATION_OK) {
            _exit(126);
        }

        (void)snprintf(runs_buf, sizeof(runs_buf), "%u", cfg->runs);
        (void)snprintf(mem_buf, sizeof(mem_buf), "%.6g", cfg->memory_percent);
        (void)snprintf(measurements_buf,
                       sizeof(measurements_buf),
                       "%u",
                       cfg->measurements);
        (void)snprintf(timing_rounds_buf,
                       sizeof(timing_rounds_buf),
                       "%u",
                       cfg->timing_rounds);

        argv[n++] = (char *)cfg->worker_path;
        argv[n++] = "--runs";
        argv[n++] = runs_buf;
        argv[n++] = "--memory-percent";
        argv[n++] = mem_buf;
        argv[n++] = "--measurements";
        argv[n++] = measurements_buf;
        argv[n++] = "--timing-rounds";
        argv[n++] = timing_rounds_buf;
        argv[n++] = "--work-dir";
        argv[n++] = (char *)cfg->work_dir;
        argv[n++] = "--output";
        argv[n++] = (char *)cfg->result_path;

        if (cfg->kk_main_path != NULL &&
            cfg->kk_main_path[0] != '\0') {
            argv[n++] = "--kk-main";
            argv[n++] = (char *)cfg->kk_main_path;
        }

        if (cfg->keep_csv) {
            argv[n++] = "--keep-csv";
        }

        argv[n] = NULL;

        execv(cfg->worker_path, argv);
        _exit(127);
    }

    /*
     * Close the setpgid race. EACCES/ESRCH are harmless if child
     * already exec'd/exited after creating its process group.
     */
    if (setpgid(pid, pid) != 0 &&
        errno != EACCES &&
        errno != ESRCH) {
        terminate_process_group(pid);
        return CALIBRATION_ERR_FORK;
    }

    *child_pid = pid;
    return CALIBRATION_OK;
}

static int read_file_limited(const char *path,
                             char **buffer_out,
                             size_t *size_out)
{
    FILE *fp;
    long file_size;
    char *buffer;
    size_t nread;

    if (path == NULL || buffer_out == NULL || size_out == NULL) {
        return CALIBRATION_ERR_INVALID_ARG;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return CALIBRATION_ERR_IO;
    }

    if (fseek(fp, 0L, SEEK_END) != 0) {
        (void)fclose(fp);
        return CALIBRATION_ERR_IO;
    }

    file_size = ftell(fp);
    if (file_size < 0 ||
        (unsigned long)file_size > CALIBRATION_JSON_MAX_BYTES) {
        (void)fclose(fp);
        return CALIBRATION_ERR_IO;
    }

    if (fseek(fp, 0L, SEEK_SET) != 0) {
        (void)fclose(fp);
        return CALIBRATION_ERR_IO;
    }

    buffer = calloc((size_t)file_size + 1U, 1U);
    if (buffer == NULL) {
        (void)fclose(fp);
        return CALIBRATION_ERR_IO;
    }

    nread = fread(buffer, 1U, (size_t)file_size, fp);
    if (nread != (size_t)file_size) {
        free(buffer);
        (void)fclose(fp);
        return CALIBRATION_ERR_IO;
    }

    if (fclose(fp) != 0) {
        free(buffer);
        return CALIBRATION_ERR_IO;
    }

    buffer[nread] = '\0';
    *buffer_out = buffer;
    *size_out = nread;

    return CALIBRATION_OK;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static const char *find_key_value(struct json_range range,
                                  const char *key)
{
    char pattern[128];
    const char *p;
    int len;

    if (key == NULL) {
        return NULL;
    }

    len = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (len <= 0 || (size_t)len >= sizeof(pattern)) {
        return NULL;
    }

    p = range.begin;

    while (p < range.end) {
        const char *match = strstr(p, pattern);
        const char *colon;

        if (match == NULL || match >= range.end) {
            return NULL;
        }

        colon = skip_ws(match + (size_t)len, range.end);
        if (colon < range.end && *colon == ':') {
            return skip_ws(colon + 1, range.end);
        }

        p = match + 1;
    }

    return NULL;
}

static int scan_balanced(const char *open,
                         const char *end,
                         char open_ch,
                         char close_ch,
                         struct json_range *out)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    const char *p;

    if (open == NULL || out == NULL ||
        open >= end || *open != open_ch) {
        return CALIBRATION_ERR_JSON;
    }

    for (p = open; p < end; ++p) {
        char ch = *p;

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
            continue;
        }

        if (ch == open_ch) {
            ++depth;
        } else if (ch == close_ch) {
            --depth;
            if (depth == 0) {
                out->begin = open;
                out->end = p + 1;
                return CALIBRATION_OK;
            }
        }
    }

    return CALIBRATION_ERR_JSON;
}

static int extract_object(struct json_range range,
                          const char *key,
                          struct json_range *out)
{
    const char *value = find_key_value(range, key);

    if (value == NULL) {
        return CALIBRATION_ERR_JSON;
    }

    return scan_balanced(value, range.end, '{', '}', out);
}

static int extract_array(struct json_range range,
                         const char *key,
                         struct json_range *out)
{
    const char *value = find_key_value(range, key);

    if (value == NULL) {
        return CALIBRATION_ERR_JSON;
    }

    return scan_balanced(value, range.end, '[', ']', out);
}

static int extract_string(struct json_range range,
                          const char *key,
                          char *dst,
                          size_t dst_size)
{
    const char *p = find_key_value(range, key);
    size_t out_len = 0U;
    bool escaped = false;

    if (p == NULL || dst == NULL || dst_size == 0U ||
        p >= range.end || *p != '"') {
        return CALIBRATION_ERR_JSON;
    }

    ++p;

    while (p < range.end) {
        char ch = *p++;

        if (escaped) {
            if (out_len + 1U >= dst_size) {
                return CALIBRATION_ERR_JSON;
            }
            dst[out_len++] = ch;
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            escaped = true;
            continue;
        }

        if (ch == '"') {
            dst[out_len] = '\0';
            return CALIBRATION_OK;
        }

        if (out_len + 1U >= dst_size) {
            return CALIBRATION_ERR_JSON;
        }

        dst[out_len++] = ch;
    }

    return CALIBRATION_ERR_JSON;
}

static int extract_bool(struct json_range range,
                        const char *key,
                        bool *out)
{
    const char *p = find_key_value(range, key);

    if (p == NULL || out == NULL) {
        return CALIBRATION_ERR_JSON;
    }

    if ((size_t)(range.end - p) >= 4U &&
        strncmp(p, "true", 4U) == 0) {
        *out = true;
        return CALIBRATION_OK;
    }

    if ((size_t)(range.end - p) >= 5U &&
        strncmp(p, "false", 5U) == 0) {
        *out = false;
        return CALIBRATION_OK;
    }

    return CALIBRATION_ERR_JSON;
}

static int extract_double(struct json_range range,
                          const char *key,
                          double *out)
{
    const char *p = find_key_value(range, key);
    char *endptr = NULL;
    double value;

    if (p == NULL || out == NULL) {
        return CALIBRATION_ERR_JSON;
    }

    errno = 0;
    value = strtod(p, &endptr);

    if (errno != 0 || endptr == p || endptr > range.end) {
        return CALIBRATION_ERR_JSON;
    }

    *out = value;
    return CALIBRATION_OK;
}

static int extract_u64(struct json_range range,
                       const char *key,
                       uint64_t *out)
{
    const char *p = find_key_value(range, key);
    char *endptr = NULL;
    unsigned long long value;

    if (p == NULL || out == NULL) {
        return CALIBRATION_ERR_JSON;
    }

    errno = 0;
    value = strtoull(p, &endptr, 10);

    if (errno != 0 || endptr == p || endptr > range.end) {
        return CALIBRATION_ERR_JSON;
    }

    *out = (uint64_t)value;
    return CALIBRATION_OK;
}

static int parse_bank_masks(struct json_range array,
                            struct calibration_result *out)
{
    const char *p = array.begin + 1;

    while (p < array.end - 1) {
        char text[64];
        size_t len = 0U;
        char *endptr = NULL;
        unsigned long long mask;

        p = skip_ws(p, array.end - 1);

        if (p >= array.end - 1) {
            break;
        }

        if (*p == ',') {
            ++p;
            continue;
        }

        if (*p != '"') {
            return CALIBRATION_ERR_JSON;
        }

        ++p;

        while (p < array.end - 1 && *p != '"') {
            if (len + 1U >= sizeof(text)) {
                return CALIBRATION_ERR_JSON;
            }
            text[len++] = *p++;
        }

        if (p >= array.end - 1 || *p != '"') {
            return CALIBRATION_ERR_JSON;
        }

        ++p;
        text[len] = '\0';

        if (out->bank_mask_count >= CALIBRATION_MAX_BANK_MASKS) {
            return CALIBRATION_ERR_TOO_MANY_MASKS;
        }

        errno = 0;
        mask = strtoull(text, &endptr, 0);

        if (errno != 0 ||
            endptr == text ||
            *endptr != '\0' ||
            mask == 0ULL) {
            return CALIBRATION_ERR_JSON;
        }

        out->bank_masks[out->bank_mask_count++] = (uint64_t)mask;
    }

    return out->bank_mask_count > 0U
               ? CALIBRATION_OK
               : CALIBRATION_ERR_REJECTED;
}

int calibration_load_result(const char *result_path,
                            struct calibration_result *out)
{
    char *json = NULL;
    size_t json_size = 0U;
    struct json_range root;
    struct json_range acceptance;
    struct json_range masks;
    struct json_range metrics;
    struct json_range holdout;
    char status[32];
    char error_text[CALIBRATION_ERROR_MESSAGE_MAX];
    int rc;

    if (result_path == NULL || out == NULL) {
        return CALIBRATION_ERR_INVALID_ARG;
    }

    calibration_result_reset(out);

    rc = read_file_limited(result_path, &json, &json_size);
    if (rc != CALIBRATION_OK) {
        set_error(out, "cannot read result JSON: %s", result_path);
        return rc;
    }

    root.begin = json;
    root.end = json + json_size;

    if (extract_string(root,
                       "error",
                       error_text,
                       sizeof(error_text)) == CALIBRATION_OK) {
        set_error(out, "%s", error_text);
    }

    rc = extract_string(root, "status", status, sizeof(status));
    if (rc != CALIBRATION_OK) {
        set_error(out, "result JSON missing string field: status");
        free(json);
        return CALIBRATION_ERR_JSON;
    }

    out->status_success = (strcmp(status, "success") == 0);
    if (!out->status_success) {
        if (out->error_message[0] == '\0') {
            set_error(out, "calibration worker status is '%s'", status);
        }
        free(json);
        return CALIBRATION_ERR_REJECTED;
    }

    rc = extract_object(root, "acceptance", &acceptance);
    if (rc != CALIBRATION_OK ||
        extract_bool(acceptance,
                     "passed",
                     &out->acceptance_passed) != CALIBRATION_OK) {
        set_error(out, "result JSON missing acceptance.passed");
        free(json);
        return CALIBRATION_ERR_JSON;
    }

    if (!out->acceptance_passed) {
        set_error(out, "calibration acceptance gate failed");
        free(json);
        return CALIBRATION_ERR_REJECTED;
    }

    rc = extract_array(root, "bank_masks", &masks);
    if (rc != CALIBRATION_OK) {
        set_error(out, "result JSON missing bank_masks");
        free(json);
        return CALIBRATION_ERR_JSON;
    }

    rc = parse_bank_masks(masks, out);
    if (rc != CALIBRATION_OK) {
        set_error(out, "invalid or empty bank_masks array");
        free(json);
        return rc;
    }

    rc = extract_string(root,
                        "mask_set_id",
                        out->mask_set_id,
                        sizeof(out->mask_set_id));
    if (rc != CALIBRATION_OK) {
        set_error(out, "result JSON missing mask_set_id");
        free(json);
        return CALIBRATION_ERR_JSON;
    }

    rc = extract_object(root, "metrics", &metrics);
    if (rc != CALIBRATION_OK ||
        extract_object(metrics, "holdout", &holdout) != CALIBRATION_OK) {
        set_error(out, "result JSON missing metrics.holdout");
        free(json);
        return CALIBRATION_ERR_JSON;
    }

    if (extract_u64(holdout, "tp", &out->holdout.tp) != CALIBRATION_OK ||
        extract_u64(holdout, "tn", &out->holdout.tn) != CALIBRATION_OK ||
        extract_u64(holdout, "fp", &out->holdout.fp) != CALIBRATION_OK ||
        extract_u64(holdout, "fn", &out->holdout.fn) != CALIBRATION_OK ||
        extract_double(holdout,
                       "accuracy",
                       &out->holdout.accuracy) != CALIBRATION_OK ||
        extract_double(holdout,
                       "precision",
                       &out->holdout.precision) != CALIBRATION_OK ||
        extract_double(holdout,
                       "recall",
                       &out->holdout.recall) != CALIBRATION_OK ||
        extract_double(holdout,
                       "f1",
                       &out->holdout.f1) != CALIBRATION_OK) {
        set_error(out, "invalid metrics.holdout object");
        free(json);
        return CALIBRATION_ERR_JSON;
    }

    free(json);
    return CALIBRATION_OK;
}

int calibration_run(const struct calibration_config *cfg,
                    struct calibration_result *out)
{
    pid_t pid;
    int worker_exit_code = -1;
    int rc;
    int load_rc;

    if (out == NULL) {
        return CALIBRATION_ERR_INVALID_ARG;
    }

    calibration_result_reset(out);

    rc = validate_config(cfg);
    if (rc != CALIBRATION_OK) {
        set_error(out, "invalid calibration configuration");
        return rc;
    }

    /*
     * Never accept a stale success file from a previous run/boot.
     * kk_calibrate.py atomically writes the replacement.
     */
    if (unlink(cfg->result_path) != 0 && errno != ENOENT) {
        set_error(out,
                  "cannot remove stale result '%s': %s",
                  cfg->result_path,
                  strerror(errno));
        return CALIBRATION_ERR_IO;
    }

    rc = launch_worker(cfg, &pid);
    if (rc != CALIBRATION_OK) {
        set_error(out,
                  "cannot launch calibration worker '%s'",
                  cfg->worker_path);
        return rc;
    }

    rc = wait_child(pid, cfg->timeout_sec, &worker_exit_code);
    out->worker_exit_code = worker_exit_code;

    if (rc != CALIBRATION_OK) {
        if (rc == CALIBRATION_ERR_TIMEOUT) {
            set_error(out,
                      "calibration timed out after %u seconds",
                      cfg->timeout_sec);
        } else {
            set_error(out, "failed while waiting for calibration worker");
        }
        return rc;
    }

    /*
     * Worker exit code 2 can still leave status=failed JSON with a useful
     * error field, so parse the file before deciding the final C error.
     */
    load_rc = calibration_load_result(cfg->result_path, out);
    out->worker_exit_code = worker_exit_code;

    if (worker_exit_code != 0) {
        if (load_rc == CALIBRATION_ERR_REJECTED) {
            return CALIBRATION_ERR_REJECTED;
        }

        if (out->error_message[0] == '\0') {
            set_error(out,
                      "calibration worker exited with code %d",
                      worker_exit_code);
        }

        return CALIBRATION_ERR_WORKER_EXIT;
    }

    return load_rc;
}

const char *calibration_strerror(int rc)
{
    switch (rc) {
    case CALIBRATION_OK:
        return "success";
    case CALIBRATION_ERR_INVALID_ARG:
        return "invalid argument";
    case CALIBRATION_ERR_FORK:
        return "fork/process-group setup failed";
    case CALIBRATION_ERR_WAIT:
        return "waitpid/clock failure";
    case CALIBRATION_ERR_TIMEOUT:
        return "calibration timeout";
    case CALIBRATION_ERR_WORKER_EXIT:
        return "calibration worker failed";
    case CALIBRATION_ERR_IO:
        return "I/O failure";
    case CALIBRATION_ERR_JSON:
        return "invalid calibration JSON";
    case CALIBRATION_ERR_REJECTED:
        return "calibration result rejected";
    case CALIBRATION_ERR_TOO_MANY_MASKS:
        return "too many bank masks";
    default:
        return "unknown calibration error";
    }
}

