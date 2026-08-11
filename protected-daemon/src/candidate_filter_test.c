#define _POSIX_C_SOURCE 200809L

#include "candidate_filter.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK_TRUE(expr)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr,                                                    \
                    "[FAIL] %s:%d: %s\n",                                      \
                    __FILE__,                                                  \
                    __LINE__,                                                  \
                    #expr);                                                    \
            return -1;                                                         \
        }                                                                      \
    } while (0)

#define CHECK_EQ_LONG(expected, actual)                                        \
    do {                                                                       \
        long expected_value = (long)(expected);                                \
        long actual_value = (long)(actual);                                    \
        if (expected_value != actual_value) {                                  \
            fprintf(stderr,                                                    \
                    "[FAIL] %s:%d: expected %ld, got %ld\n",                   \
                    __FILE__,                                                  \
                    __LINE__,                                                  \
                    expected_value,                                            \
                    actual_value);                                             \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static struct proc_info make_proc(pid_t pid)
{
    struct proc_info p;

    memset(&p, 0, sizeof(p));

    p.used = 1;
    p.pid = pid;
    p.group = GROUP_UNKNOWN;

    (void)snprintf(p.comm,
                   sizeof(p.comm),
                   "pid-%ld",
                   (long)pid);

    return p;
}

static int test_system_slice_path_matching(void)
{
    CHECK_TRUE(candidate_filter_path_is_system_slice(
        "/system.slice"));

    CHECK_TRUE(candidate_filter_path_is_system_slice(
        "/system.slice/ssh.service"));

    CHECK_TRUE(candidate_filter_path_is_system_slice(
        "/system.slice/example.slice/work.service"));

    CHECK_TRUE(!candidate_filter_path_is_system_slice(
        "/"));

    CHECK_TRUE(!candidate_filter_path_is_system_slice(
        "/user.slice/user-1000.slice/session-1.scope"));

    CHECK_TRUE(!candidate_filter_path_is_system_slice(
        "/system.slice2/example.service"));

    CHECK_TRUE(!candidate_filter_path_is_system_slice(
        "/foo/system.slice/example.service"));

    CHECK_TRUE(!candidate_filter_path_is_system_slice(
        NULL));

    return 0;
}

static int test_background_candidate(void)
{
    struct proc_info p = make_proc(1001);
    struct candidate_filter_result result;

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_OK,
        candidate_filter_classify_path(
            &p,
            9999,
            "/user.slice/user-1000.slice/session-1.scope",
            &result));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_BACKGROUND,
        result.kind);

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_REASON_NONE,
        result.reason);

    return 0;
}

static int test_system_slice_is_excluded(void)
{
    struct proc_info p = make_proc(1002);
    struct candidate_filter_result result;

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_OK,
        candidate_filter_classify_path(
            &p,
            9999,
            "/system.slice/example.service",
            &result));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_EXCLUDED,
        result.kind);

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_REASON_SYSTEM_SLICE,
        result.reason);

    return 0;
}

static int test_protected_has_priority_over_system_slice(void)
{
    struct proc_info p = make_proc(1003);
    struct candidate_filter_result result;

    p.is_protected = 1;
    p.group = GROUP_PROTECTED;

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_OK,
        candidate_filter_classify_path(
            &p,
            9999,
            "/system.slice/protected.service",
            &result));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_PROTECTED,
        result.kind);

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_REASON_NONE,
        result.reason);

    return 0;
}

static int test_inherited_protected_has_priority(void)
{
    struct proc_info p = make_proc(1004);
    struct candidate_filter_result result;

    p.inherited_protected = 1;

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_OK,
        candidate_filter_classify_path(
            &p,
            9999,
            "/system.slice/child.service",
            &result));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_PROTECTED,
        result.kind);

    return 0;
}

static int test_daemon_self_is_always_excluded(void)
{
    struct proc_info p = make_proc(2000);
    struct candidate_filter_result result;

    /*
     * Even if the daemon were accidentally marked protected,
     * self-exclusion has higher priority.
     */
    p.is_protected = 1;
    p.group = GROUP_PROTECTED;

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_OK,
        candidate_filter_classify_path(
            &p,
            2000,
            "/user.slice/test.scope",
            &result));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_EXCLUDED,
        result.kind);

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_REASON_DAEMON_SELF,
        result.reason);

    return 0;
}

static int test_cgroup_unavailable_is_nonfatal_exclusion(void)
{
    struct proc_info p = make_proc(3000);
    struct candidate_filter_result result;

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_OK,
        candidate_filter_classify_path(
            &p,
            9999,
            NULL,
            &result));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_EXCLUDED,
        result.kind);

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_REASON_CGROUP_UNAVAILABLE,
        result.reason);

    return 0;
}

static int test_invalid_process_record(void)
{
    struct proc_info p;
    struct candidate_filter_result result;

    memset(&p, 0, sizeof(p));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_OK,
        candidate_filter_classify_path(
            &p,
            9999,
            "/user.slice/test.scope",
            &result));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_EXCLUDED,
        result.kind);

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_REASON_INVALID_PROCESS,
        result.reason);

    return 0;
}

static int test_missing_pid_is_excluded_not_fatal(void)
{
    struct proc_info p = make_proc((pid_t)2147483000);
    struct candidate_filter_result result;

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_OK,
        candidate_filter_classify_pid(
            &p,
            9999,
            &result));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_EXCLUDED,
        result.kind);

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_REASON_CGROUP_UNAVAILABLE,
        result.reason);

    return 0;
}

static int test_protected_missing_pid_does_not_need_cgroup_read(void)
{
    struct proc_info p = make_proc((pid_t)2147483001);
    struct candidate_filter_result result;

    p.is_protected = 1;

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_OK,
        candidate_filter_classify_pid(
            &p,
            9999,
            &result));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_PROTECTED,
        result.kind);

    return 0;
}

static int test_invalid_result_pointer(void)
{
    struct proc_info p = make_proc(4000);

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_ERR_INVALID_ARG,
        candidate_filter_classify_path(
            &p,
            9999,
            "/user.slice/test.scope",
            NULL));

    CHECK_EQ_LONG(
        CANDIDATE_FILTER_ERR_INVALID_ARG,
        candidate_filter_classify_pid(
            &p,
            9999,
            NULL));

    return 0;
}

static void run_test(const char *name, int (*fn)(void))
{
    if (fn() == 0) {
        printf("[PASS] %s\n", name);
    } else {
        failures++;
    }
}

int main(void)
{
    run_test(
        "system.slice path matching",
        test_system_slice_path_matching);

    run_test(
        "background candidate",
        test_background_candidate);

    run_test(
        "system.slice is excluded",
        test_system_slice_is_excluded);

    run_test(
        "protected has priority over system.slice",
        test_protected_has_priority_over_system_slice);

    run_test(
        "inherited protected has priority",
        test_inherited_protected_has_priority);

    run_test(
        "daemon self is always excluded",
        test_daemon_self_is_always_excluded);

    run_test(
        "cgroup unavailable is nonfatal exclusion",
        test_cgroup_unavailable_is_nonfatal_exclusion);

    run_test(
        "invalid process record",
        test_invalid_process_record);

    run_test(
        "missing pid is excluded, not fatal",
        test_missing_pid_is_excluded_not_fatal);

    run_test(
        "protected missing pid skips cgroup dependency",
        test_protected_missing_pid_does_not_need_cgroup_read);

    run_test(
        "invalid result pointer",
        test_invalid_result_pointer);

    if (failures != 0) {
        fprintf(stderr,
                "[FAIL] candidate_filter tests: %d failure(s)\n",
                failures);
        return 1;
    }

    printf("[PASS] all candidate_filter tests\n");
    return 0;
}
