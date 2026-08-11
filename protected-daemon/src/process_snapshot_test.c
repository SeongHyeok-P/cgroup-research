#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK_TRUE(expr)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr,                                                    \
                    "[FAIL] %s:%d: %s\n",                                    \
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
                    "[FAIL] %s:%d: expected %ld, got %ld\n",                  \
                    __FILE__,                                                  \
                    __LINE__,                                                  \
                    expected_value,                                            \
                    actual_value);                                             \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static struct event make_exec_event(pid_t pid,
                                    pid_t ppid,
                                    uint32_t uid,
                                    const char *comm)
{
    struct event e;

    memset(&e, 0, sizeof(e));
    e.type = EVENT_EXEC;
    e.pid = pid;
    e.ppid = ppid;
    e.uid = uid;
    (void)snprintf(e.comm, sizeof(e.comm), "%s", comm);

    return e;
}

static const struct proc_info *snapshot_find(const struct proc_info *snapshot,
                                             size_t count,
                                             pid_t pid)
{
    size_t i;

    for (i = 0U; i < count; ++i) {
        if (snapshot[i].used && snapshot[i].pid == pid)
            return &snapshot[i];
    }

    return NULL;
}

static int insert_exec(pid_t pid, pid_t ppid, uint32_t uid, const char *comm)
{
    struct event e = make_exec_event(pid, ppid, uid, comm);
    return process_upsert_exec(&e) != NULL ? 0 : -1;
}

static int test_empty_snapshot(void)
{
    struct proc_info out[1];
    size_t count = 999U;
    int rc;

    process_table_reset();
    memset(out, 0xA5, sizeof(out));

    rc = process_snapshot(out, 1U, &count);

    CHECK_EQ_LONG(PROCESS_OK, rc);
    CHECK_EQ_LONG(0, count);
    CHECK_EQ_LONG(0, process_table_count());

    return 0;
}

static int test_basic_snapshot(void)
{
    struct proc_info out[3];
    size_t count = 0U;

    process_table_reset();

    CHECK_EQ_LONG(0, insert_exec(101, 1, 1000U, "p101"));
    CHECK_EQ_LONG(0, insert_exec(202, 2, 1001U, "p202"));
    CHECK_EQ_LONG(0, insert_exec(303, 3, 1002U, "p303"));

    CHECK_EQ_LONG(PROCESS_OK, process_snapshot(out, 3U, &count));
    CHECK_EQ_LONG(3, count);

    CHECK_TRUE(snapshot_find(out, count, 101) != NULL);
    CHECK_TRUE(snapshot_find(out, count, 202) != NULL);
    CHECK_TRUE(snapshot_find(out, count, 303) != NULL);

    return 0;
}

static int test_collision_entries_are_all_visible(void)
{
    const pid_t p1 = 17;
    const pid_t p2 = p1 + (pid_t)PROC_MAX;
    const pid_t p3 = p2 + (pid_t)PROC_MAX;
    struct proc_info out[3];
    size_t count = 0U;

    process_table_reset();

    CHECK_EQ_LONG(0, insert_exec(p1, 1, 1000U, "p1"));
    CHECK_EQ_LONG(0, insert_exec(p2, 2, 1001U, "p2"));
    CHECK_EQ_LONG(0, insert_exec(p3, 3, 1002U, "p3"));

    CHECK_EQ_LONG(PROCESS_OK, process_snapshot(out, 3U, &count));
    CHECK_EQ_LONG(3, count);

    CHECK_TRUE(snapshot_find(out, count, p1) != NULL);
    CHECK_TRUE(snapshot_find(out, count, p2) != NULL);
    CHECK_TRUE(snapshot_find(out, count, p3) != NULL);

    return 0;
}

static int test_tombstone_is_excluded_and_reused(void)
{
    const pid_t p1 = 31;
    const pid_t p2 = p1 + (pid_t)PROC_MAX;
    const pid_t p3 = p2 + (pid_t)PROC_MAX;
    const pid_t p4 = p3 + (pid_t)PROC_MAX;
    struct proc_info out[3];
    size_t count = 0U;

    process_table_reset();

    CHECK_EQ_LONG(0, insert_exec(p1, 1, 1000U, "p1"));
    CHECK_EQ_LONG(0, insert_exec(p2, 2, 1001U, "p2"));
    CHECK_EQ_LONG(0, insert_exec(p3, 3, 1002U, "p3"));

    process_remove(p1);

    CHECK_EQ_LONG(PROCESS_OK, process_snapshot(out, 3U, &count));
    CHECK_EQ_LONG(2, count);
    CHECK_TRUE(snapshot_find(out, count, p1) == NULL);
    CHECK_TRUE(snapshot_find(out, count, p2) != NULL);
    CHECK_TRUE(snapshot_find(out, count, p3) != NULL);

    CHECK_EQ_LONG(0, insert_exec(p4, 4, 1003U, "p4"));

    count = 0U;
    CHECK_EQ_LONG(PROCESS_OK, process_snapshot(out, 3U, &count));
    CHECK_EQ_LONG(3, count);
    CHECK_TRUE(snapshot_find(out, count, p1) == NULL);
    CHECK_TRUE(snapshot_find(out, count, p2) != NULL);
    CHECK_TRUE(snapshot_find(out, count, p3) != NULL);
    CHECK_TRUE(snapshot_find(out, count, p4) != NULL);

    return 0;
}

static int test_exact_capacity(void)
{
    struct proc_info out[2];
    size_t count = 0U;

    process_table_reset();

    CHECK_EQ_LONG(0, insert_exec(401, 1, 1000U, "p401"));
    CHECK_EQ_LONG(0, insert_exec(402, 1, 1000U, "p402"));

    CHECK_EQ_LONG(PROCESS_OK, process_snapshot(out, 2U, &count));
    CHECK_EQ_LONG(2, count);
    CHECK_TRUE(snapshot_find(out, count, 401) != NULL);
    CHECK_TRUE(snapshot_find(out, count, 402) != NULL);

    return 0;
}

static int test_capacity_shortage_is_all_or_nothing(void)
{
    struct proc_info out[2];
    struct proc_info before[2];
    size_t count = 777U;
    int rc;

    process_table_reset();

    CHECK_EQ_LONG(0, insert_exec(501, 1, 1000U, "p501"));
    CHECK_EQ_LONG(0, insert_exec(502, 1, 1000U, "p502"));
    CHECK_EQ_LONG(0, insert_exec(503, 1, 1000U, "p503"));

    memset(out, 0x5A, sizeof(out));
    memcpy(before, out, sizeof(out));

    rc = process_snapshot(out, 2U, &count);

    CHECK_EQ_LONG(PROCESS_ERR_NO_SPACE, rc);
    CHECK_EQ_LONG(0, count);
    CHECK_TRUE(memcmp(out, before, sizeof(out)) == 0);

    return 0;
}

static int test_snapshot_is_independent_copy(void)
{
    struct proc_info out[1];
    struct proc_info *original;
    size_t count = 0U;

    process_table_reset();

    CHECK_EQ_LONG(0, insert_exec(601, 60, 1000U, "original"));

    original = process_get(601);
    CHECK_TRUE(original != NULL);
    original->group = GROUP_BACKGROUND;

    CHECK_EQ_LONG(PROCESS_OK, process_snapshot(out, 1U, &count));
    CHECK_EQ_LONG(1, count);
    CHECK_EQ_LONG(601, out[0].pid);
    CHECK_TRUE(strcmp(out[0].comm, "original") == 0);
    CHECK_EQ_LONG(GROUP_BACKGROUND, out[0].group);

    out[0].pid = 999999;
    out[0].group = GROUP_THROTTLED;
    (void)snprintf(out[0].comm, sizeof(out[0].comm), "%s", "modified");

    original = process_get(601);
    CHECK_TRUE(original != NULL);
    CHECK_EQ_LONG(601, original->pid);
    CHECK_EQ_LONG(GROUP_BACKGROUND, original->group);
    CHECK_TRUE(strcmp(original->comm, "original") == 0);
    CHECK_TRUE(process_get(999999) == NULL);

    return 0;
}

static int test_invalid_arguments(void)
{
    struct proc_info out[1];
    size_t count = 123U;

    process_table_reset();

    CHECK_EQ_LONG(PROCESS_ERR_INVALID_ARG,
                  process_snapshot(NULL, 1U, &count));
    CHECK_EQ_LONG(0, count);

    CHECK_EQ_LONG(PROCESS_ERR_INVALID_ARG,
                  process_snapshot(out, 1U, NULL));

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
    run_test("empty snapshot", test_empty_snapshot);
    run_test("basic snapshot", test_basic_snapshot);
    run_test("collision entries are all visible",
             test_collision_entries_are_all_visible);
    run_test("tombstone excluded and reused",
             test_tombstone_is_excluded_and_reused);
    run_test("exact capacity", test_exact_capacity);
    run_test("capacity shortage is all-or-nothing",
             test_capacity_shortage_is_all_or_nothing);
    run_test("snapshot is an independent copy",
             test_snapshot_is_independent_copy);
    run_test("invalid arguments", test_invalid_arguments);

    if (failures != 0) {
        fprintf(stderr,
                "[FAIL] process_snapshot tests: %d failure(s)\n",
                failures);
        return 1;
    }

    printf("[PASS] all process_snapshot tests\n");
    return 0;
}
