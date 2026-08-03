#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static struct event make_fork_event(pid_t parent_pid,
                                    pid_t child_pid,
                                    uint32_t uid,
                                    const char *comm)
{
    struct event e;

    memset(&e, 0, sizeof(e));
    e.type = EVENT_FORK;
    e.pid = parent_pid;
    e.child_pid = child_pid;
    e.uid = uid;
    (void)snprintf(e.comm, sizeof(e.comm), "%s", comm);

    return e;
}

static int test_basic_insert_get_update(void)
{
    struct event e;
    struct proc_info *p;

    process_table_reset();

    CHECK_EQ_LONG(0, process_table_count());
    CHECK_EQ_LONG(PROC_MAX, process_table_capacity());
    CHECK_TRUE(process_get(1234) == NULL);

    e = make_exec_event(1234, 100, 1000U, "first");
    p = process_upsert_exec(&e);
    CHECK_TRUE(p != NULL);
    CHECK_EQ_LONG(1, process_table_count());
    CHECK_EQ_LONG(1234, p->pid);
    CHECK_EQ_LONG(100, p->ppid);
    CHECK_EQ_LONG(1, p->exec_seen);
    CHECK_TRUE(strcmp(p->comm, "first") == 0);

    e = make_exec_event(1234, 101, 1001U, "second");
    p = process_upsert_exec(&e);
    CHECK_TRUE(p != NULL);
    CHECK_EQ_LONG(1, process_table_count());
    CHECK_EQ_LONG(101, p->ppid);
    CHECK_EQ_LONG(1001, p->uid);
    CHECK_TRUE(strcmp(p->comm, "second") == 0);

    return 0;
}

static int test_colliding_pids_do_not_overwrite(void)
{
    const pid_t p1 = 17;
    const pid_t p2 = p1 + (pid_t)PROC_MAX;
    const pid_t p3 = p2 + (pid_t)PROC_MAX;
    struct event e;

    process_table_reset();

    e = make_exec_event(p1, 1, 1000U, "p1");
    CHECK_TRUE(process_upsert_exec(&e) != NULL);

    e = make_exec_event(p2, 2, 1001U, "p2");
    CHECK_TRUE(process_upsert_exec(&e) != NULL);

    e = make_exec_event(p3, 3, 1002U, "p3");
    CHECK_TRUE(process_upsert_exec(&e) != NULL);

    CHECK_EQ_LONG(3, process_table_count());
    CHECK_TRUE(process_get(p1) != NULL);
    CHECK_TRUE(process_get(p2) != NULL);
    CHECK_TRUE(process_get(p3) != NULL);
    CHECK_TRUE(strcmp(process_get(p1)->comm, "p1") == 0);
    CHECK_TRUE(strcmp(process_get(p2)->comm, "p2") == 0);
    CHECK_TRUE(strcmp(process_get(p3)->comm, "p3") == 0);

    /* Updating the middle entry must not damage either neighbor. */
    e = make_exec_event(p2, 22, 2001U, "p2-new");
    CHECK_TRUE(process_upsert_exec(&e) != NULL);
    CHECK_EQ_LONG(3, process_table_count());
    CHECK_TRUE(strcmp(process_get(p1)->comm, "p1") == 0);
    CHECK_TRUE(strcmp(process_get(p2)->comm, "p2-new") == 0);
    CHECK_TRUE(strcmp(process_get(p3)->comm, "p3") == 0);

    return 0;
}

static int test_tombstone_preserves_probe_chain_and_is_reused(void)
{
    const pid_t p1 = 41;
    const pid_t p2 = p1 + (pid_t)PROC_MAX;
    const pid_t p3 = p2 + (pid_t)PROC_MAX;
    const pid_t p4 = p3 + (pid_t)PROC_MAX;
    struct event e;

    process_table_reset();

    e = make_exec_event(p1, 1, 1U, "p1");
    CHECK_TRUE(process_upsert_exec(&e) != NULL);
    e = make_exec_event(p2, 1, 1U, "p2");
    CHECK_TRUE(process_upsert_exec(&e) != NULL);
    e = make_exec_event(p3, 1, 1U, "p3");
    CHECK_TRUE(process_upsert_exec(&e) != NULL);

    /* Remove the first slot in a three-entry collision chain. */
    process_remove(p1);
    CHECK_TRUE(process_get(p1) == NULL);
    CHECK_TRUE(process_get(p2) != NULL);
    CHECK_TRUE(process_get(p3) != NULL);
    CHECK_EQ_LONG(2, process_table_count());

    /* The next colliding PID must reuse the tombstone safely. */
    e = make_exec_event(p4, 1, 1U, "p4");
    CHECK_TRUE(process_upsert_exec(&e) != NULL);
    CHECK_EQ_LONG(3, process_table_count());
    CHECK_TRUE(process_get(p2) != NULL);
    CHECK_TRUE(process_get(p3) != NULL);
    CHECK_TRUE(process_get(p4) != NULL);

    process_remove(9999999);
    CHECK_EQ_LONG(3, process_table_count());

    return 0;
}

static int test_fork_inheritance_survives_exec(void)
{
    const pid_t parent_pid = 500;
    const pid_t child_pid = parent_pid + (pid_t)PROC_MAX;
    struct event e;
    struct proc_info *parent;
    struct proc_info *child;

    process_table_reset();

    e = make_exec_event(parent_pid, 1, 0U, "parent");
    parent = process_upsert_exec(&e);
    CHECK_TRUE(parent != NULL);
    parent->is_protected = 1;
    parent->group = GROUP_PROTECTED;

    e = make_fork_event(parent_pid, child_pid, 0U, "parent");
    child = process_upsert_fork(&e);
    CHECK_TRUE(child != NULL);
    CHECK_EQ_LONG(1, child->is_protected);
    CHECK_EQ_LONG(1, child->inherited_protected);
    CHECK_EQ_LONG(GROUP_PROTECTED, child->group);

    e = make_exec_event(child_pid, parent_pid, 0U, "child-exec");
    child = process_upsert_exec(&e);
    CHECK_TRUE(child != NULL);
    CHECK_EQ_LONG(1, child->is_protected);
    CHECK_EQ_LONG(1, child->inherited_protected);
    CHECK_EQ_LONG(GROUP_PROTECTED, child->group);
    CHECK_EQ_LONG(1, child->exec_seen);
    CHECK_TRUE(strcmp(child->comm, "child-exec") == 0);

    return 0;
}

static int test_full_table_and_recovery(void)
{
    size_t i;
    struct event e;
    const pid_t replacement_pid = (pid_t)(PROC_MAX * 4U + 7U);

    process_table_reset();

    /* Use fork upsert to avoid /proc reads while filling all 32768 slots. */
    for (i = 0U; i < (size_t)PROC_MAX; ++i) {
        pid_t child_pid = (pid_t)(i + 1U);

        e = make_fork_event(900000, child_pid, 1000U, "fill");
        CHECK_TRUE(process_upsert_fork(&e) != NULL);
    }

    CHECK_EQ_LONG(PROC_MAX, process_table_count());

    e = make_fork_event(900000, replacement_pid, 1000U, "overflow");
    CHECK_TRUE(process_upsert_fork(&e) == NULL);
    CHECK_EQ_LONG(PROC_MAX, process_table_count());

    process_remove(7);
    CHECK_EQ_LONG(PROC_MAX - 1U, process_table_count());

    e = make_fork_event(900000, replacement_pid, 1000U, "replacement");
    CHECK_TRUE(process_upsert_fork(&e) != NULL);
    CHECK_EQ_LONG(PROC_MAX, process_table_count());
    CHECK_TRUE(process_get(replacement_pid) != NULL);

    return 0;
}

static int test_invalid_inputs(void)
{
    struct event e;

    process_table_reset();

    CHECK_TRUE(process_get(0) == NULL);
    CHECK_TRUE(process_get(-1) == NULL);
    CHECK_TRUE(process_upsert_exec(NULL) == NULL);
    CHECK_TRUE(process_upsert_fork(NULL) == NULL);

    e = make_exec_event(0, 1, 0U, "invalid");
    CHECK_TRUE(process_upsert_exec(&e) == NULL);

    e = make_fork_event(1, 0, 0U, "invalid");
    CHECK_TRUE(process_upsert_fork(&e) == NULL);

    process_remove(0);
    process_remove(-1);
    CHECK_EQ_LONG(0, process_table_count());

    CHECK_EQ_LONG(-1, process_read_exe(NULL));
    CHECK_EQ_LONG(-1, process_read_cmdline(NULL));

    return 0;
}

static void run_test(const char *name, int (*test_fn)(void))
{
    if (test_fn() == 0) {
        printf("[PASS] %s\n", name);
    } else {
        failures++;
        printf("[FAIL] %s\n", name);
    }
}

int main(void)
{
    run_test("basic insert, get, and update", test_basic_insert_get_update);
    run_test("colliding PIDs do not overwrite", test_colliding_pids_do_not_overwrite);
    run_test("tombstone preserves chain and is reused", test_tombstone_preserves_probe_chain_and_is_reused);
    run_test("fork inheritance survives exec", test_fork_inheritance_survives_exec);
    run_test("full table and recovery", test_full_table_and_recovery);
    run_test("invalid inputs", test_invalid_inputs);

    if (failures == 0) {
        printf("[PASS] all process collision tests\n");
        return 0;
    }

    printf("[FAIL] process collision tests: %d failure(s)\n", failures);
    return 1;
}
