#define _POSIX_C_SOURCE 200809L

#include "throttle_controller.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TEST_PATH_MAX 512U

struct fake_clock {
    uint64_t now_ns;
    bool fail;
};

struct fake_cgroup {
    char root[TEST_PATH_MAX];
};

static int g_failures = 0;

#define CHECK_TRUE(expr)                                                   \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr, "[FAIL] %s:%d: %s\n",                       \
                    __FILE__, __LINE__, #expr);                            \
            g_failures++;                                                  \
            return false;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(expected, actual)                                     \
    do {                                                                   \
        int _expected = (expected);                                        \
        int _actual = (actual);                                            \
        if (_expected != _actual) {                                        \
            fprintf(stderr,                                                \
                    "[FAIL] %s:%d: expected %d, got %d\n",              \
                    __FILE__, __LINE__, _expected, _actual);               \
            g_failures++;                                                  \
            return false;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_SIZE(expected, actual)                                    \
    do {                                                                   \
        size_t _expected = (expected);                                     \
        size_t _actual = (actual);                                         \
        if (_expected != _actual) {                                        \
            fprintf(stderr,                                                \
                    "[FAIL] %s:%d: expected %zu, got %zu\n",            \
                    __FILE__, __LINE__, _expected, _actual);               \
            g_failures++;                                                  \
            return false;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_STR(expected, actual)                                     \
    do {                                                                   \
        const char *_expected = (expected);                                \
        const char *_actual = (actual);                                    \
        if (strcmp(_expected, _actual) != 0) {                             \
            fprintf(stderr,                                                \
                    "[FAIL] %s:%d: expected \"%s\", got \"%s\"\n", \
                    __FILE__, __LINE__, _expected, _actual);               \
            g_failures++;                                                  \
            return false;                                                  \
        }                                                                  \
    } while (0)

static int fake_clock_now(void *ctx, uint64_t *now_ns)
{
    struct fake_clock *clock = ctx;

    if (clock == NULL || now_ns == NULL || clock->fail)
        return THROTTLE_CONTROLLER_ERR_TIME;

    *now_ns = clock->now_ns;
    return THROTTLE_CONTROLLER_OK;
}

static void clock_set_ms(struct fake_clock *clock, uint64_t ms)
{
    clock->now_ns = ms * 1000000ULL;
}

static int join_path(char *out, size_t out_size,
                     const char *left, const char *right)
{
    int written = snprintf(out, out_size, "%s/%s", left, right);

    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static int create_empty_file(const char *path)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL)
        return -1;

    return fclose(fp) == 0 ? 0 : -1;
}

static bool write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL)
        return false;

    if (fputs(text, fp) == EOF) {
        (void)fclose(fp);
        return false;
    }

    return fclose(fp) == 0;
}

static bool read_text(const char *path, char *out, size_t out_size)
{
    FILE *fp;
    size_t n;

    if (out == NULL || out_size == 0U)
        return false;

    fp = fopen(path, "r");
    if (fp == NULL)
        return false;

    n = fread(out, 1U, out_size - 1U, fp);
    out[n] = '\0';

    if (ferror(fp)) {
        (void)fclose(fp);
        return false;
    }

    return fclose(fp) == 0;
}

static bool fake_cgroup_create(struct fake_cgroup *cg)
{
    static const char *groups[] = {
        "protected", "background", "throttled"
    };
    static const char *files[] = {
        "cgroup.procs", "cpu.max", "memory.low"
    };
    char template_path[] = "/tmp/throttle_controller_test.XXXXXX";
    size_t i;
    size_t j;

    if (mkdtemp(template_path) == NULL)
        return false;

    if (snprintf(cg->root, sizeof(cg->root), "%s", template_path) <= 0)
        return false;

    for (i = 0U; i < sizeof(groups) / sizeof(groups[0]); i++) {
        char group_path[TEST_PATH_MAX];

        if (join_path(group_path, sizeof(group_path),
                      cg->root, groups[i]) != 0)
            return false;

        if (mkdir(group_path, 0700) != 0)
            return false;

        for (j = 0U; j < sizeof(files) / sizeof(files[0]); j++) {
            char file_path[TEST_PATH_MAX];

            if (join_path(file_path, sizeof(file_path),
                          group_path, files[j]) != 0)
                return false;

            if (create_empty_file(file_path) != 0)
                return false;
        }
    }

    return cgroup_set_base_path(cg->root) == 0;
}

static void fake_cgroup_destroy(const struct fake_cgroup *cg)
{
    char command[TEST_PATH_MAX + 32U];

    if (cg == NULL || cg->root[0] == '\0')
        return;

    if (snprintf(command, sizeof(command), "rm -rf -- '%s'", cg->root) > 0) {
        int sys_rc = system(command);
	(void)sys_rc;
    }
}

static bool cgroup_file_path(const struct fake_cgroup *cg,
                             const char *group,
                             const char *file,
                             char *out,
                             size_t out_size)
{
    int written = snprintf(out, out_size, "%s/%s/%s",
                           cg->root, group, file);

    return written > 0 && (size_t)written < out_size;
}

static struct interference_candidate make_candidate(pid_t pid,
                                                     double activity,
                                                     double overlap,
                                                     double final,
                                                     bool active,
                                                     bool should_throttle)
{
    struct interference_candidate candidate;

    memset(&candidate, 0, sizeof(candidate));
    candidate.pid = pid;
    candidate.activity_score = activity;
    candidate.overlap_score = overlap;
    candidate.final_score = final;
    candidate.common_pages = 100U;
    candidate.active = active;
    candidate.should_throttle = should_throttle;

    return candidate;
}

static void test_config(struct throttle_controller_config *cfg,
                        struct fake_clock *clock)
{
    throttle_controller_config_default(cfg);
    cfg->clock_fn = fake_clock_now;
    cfg->clock_ctx = clock;
    cfg->min_throttle_duration_ms = 3000U;
    cfg->cooldown_duration_ms = 2000U;
    cfg->stale_after_ms = 2000U;
}

static bool test_defaults_and_helpers(void)
{
    struct throttle_controller_config cfg;
    struct throttle_record record;
    struct throttle_controller_stats stats;

    throttle_controller_config_default(&cfg);

    CHECK_EQ_INT(GROUP_BACKGROUND, cfg.normal_group);
    CHECK_EQ_INT(GROUP_THROTTLED, cfg.throttled_group);
    CHECK_EQ_INT(GROUP_PROTECTED, cfg.protected_group);
    CHECK_EQ_STR("20000 100000", cfg.throttled_cpu_max);
    CHECK_EQ_STR("0", cfg.protected_memory_low);
    CHECK_TRUE(cfg.high_score_threshold == 0.20);
    CHECK_TRUE(cfg.low_score_threshold == 0.10);
    CHECK_EQ_SIZE(256U, cfg.max_records);
    CHECK_TRUE(cfg.configure_throttled_cpu_max);
    CHECK_TRUE(!cfg.configure_protected_memory_low);
    CHECK_TRUE(!cfg.dry_run);
    CHECK_TRUE(cfg.clock_fn == NULL);

    throttle_controller_record_reset(&record);
    CHECK_EQ_INT(-1, record.pid);
    CHECK_EQ_INT(THROTTLE_STATE_NONE, record.state);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK, record.last_error);

    throttle_controller_stats_reset(&stats);
    CHECK_EQ_SIZE(0U, stats.errors);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK, stats.last_error);

    CHECK_EQ_STR("none", throttle_state_str(THROTTLE_STATE_NONE));
    CHECK_EQ_STR("throttled",
                 throttle_state_str(THROTTLE_STATE_THROTTLED));
    CHECK_EQ_STR("cooldown",
                 throttle_state_str(THROTTLE_STATE_COOLDOWN));
    CHECK_EQ_STR("cgroup operation failed",
                 throttle_controller_strerror(
                     THROTTLE_CONTROLLER_ERR_CGROUP));

    return true;
}

static bool test_init_configures_cgroups(void)
{
    struct fake_cgroup cg = {{0}};
    struct fake_clock clock = {0};
    struct throttle_controller_config cfg;
    struct throttle_controller controller;
    char path[TEST_PATH_MAX];
    char text[128];

    CHECK_TRUE(fake_cgroup_create(&cg));
    test_config(&cfg, &clock);
    cfg.configure_protected_memory_low = true;
    (void)snprintf(cfg.protected_memory_low,
                   sizeof(cfg.protected_memory_low), "%s", "4096");

    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_init(&controller, &cfg));

    CHECK_TRUE(cgroup_file_path(&cg, "throttled", "cpu.max",
                                path, sizeof(path)));
    CHECK_TRUE(read_text(path, text, sizeof(text)));
    CHECK_EQ_STR("20000 100000\n", text);

    CHECK_TRUE(cgroup_file_path(&cg, "protected", "memory.low",
                                path, sizeof(path)));
    CHECK_TRUE(read_text(path, text, sizeof(text)));
    CHECK_EQ_STR("4096\n", text);

    throttle_controller_destroy(&controller);
    fake_cgroup_destroy(&cg);
    return true;
}

static bool test_policy_hysteresis_and_cooldown(void)
{
    struct fake_cgroup cg = {{0}};
    struct fake_clock clock = {0};
    struct throttle_controller_config cfg;
    struct throttle_controller controller;
    struct throttle_controller_stats stats;
    struct throttle_record record;
    struct interference_candidate candidate;
    char path[TEST_PATH_MAX];
    char text[128];

    CHECK_TRUE(fake_cgroup_create(&cg));
    test_config(&cfg, &clock);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_init(&controller, &cfg));

    clock_set_ms(&clock, 0U);
    candidate = make_candidate(1001, 0.4, 0.4, 0.16,
                               true, false);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));
    CHECK_EQ_SIZE(1U, stats.skipped_low_score);
    CHECK_TRUE(throttle_controller_get_record(&controller, 1001, &record));
    CHECK_EQ_INT(THROTTLE_STATE_NONE, record.state);

    clock_set_ms(&clock, 100U);
    candidate = make_candidate(1001, 0.8, 0.6, 0.48,
                               true, true);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));
    CHECK_EQ_SIZE(1U, stats.throttle_actions);
    CHECK_TRUE(throttle_controller_get_record(&controller, 1001, &record));
    CHECK_EQ_INT(THROTTLE_STATE_THROTTLED, record.state);

    CHECK_TRUE(cgroup_file_path(&cg, "throttled", "cgroup.procs",
                                path, sizeof(path)));
    CHECK_TRUE(read_text(path, text, sizeof(text)));
    CHECK_EQ_STR("1001\n", text);

    /* Between low and high: detector false must not break hysteresis. */
    clock_set_ms(&clock, 1000U);
    candidate = make_candidate(1001, 0.5, 0.3, 0.15,
                               true, false);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));
    CHECK_EQ_SIZE(1U, stats.kept_throttled);
    CHECK_TRUE(throttle_controller_get_record(&controller, 1001, &record));
    CHECK_EQ_INT(THROTTLE_STATE_THROTTLED, record.state);

    /* Low score, but minimum throttle duration has not elapsed. */
    clock_set_ms(&clock, 3099U);
    candidate = make_candidate(1001, 0.2, 0.2, 0.04,
                               true, false);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));
    CHECK_EQ_SIZE(1U, stats.kept_throttled);

    /* Throttled at 100 ms, so 3100 ms is exactly 3000 ms later. */
    clock_set_ms(&clock, 3100U);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));
    CHECK_EQ_SIZE(1U, stats.release_actions);
    CHECK_TRUE(throttle_controller_get_record(&controller, 1001, &record));
    CHECK_EQ_INT(THROTTLE_STATE_COOLDOWN, record.state);

    CHECK_TRUE(cgroup_file_path(&cg, "background", "cgroup.procs",
                                path, sizeof(path)));
    CHECK_TRUE(read_text(path, text, sizeof(text)));
    CHECK_EQ_STR("1001\n", text);

    clock_set_ms(&clock, 4000U);
    candidate = make_candidate(1001, 0.9, 0.7, 0.63,
                               true, true);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));
    CHECK_EQ_SIZE(1U, stats.skipped_cooldown);
    CHECK_TRUE(throttle_controller_get_record(&controller, 1001, &record));
    CHECK_EQ_INT(THROTTLE_STATE_COOLDOWN, record.state);

    clock_set_ms(&clock, 5100U);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));
    CHECK_EQ_SIZE(1U, stats.throttle_actions);
    CHECK_TRUE(throttle_controller_get_record(&controller, 1001, &record));
    CHECK_EQ_INT(THROTTLE_STATE_THROTTLED, record.state);

    throttle_controller_destroy(&controller);
    fake_cgroup_destroy(&cg);
    return true;
}

static bool test_stale_release(void)
{
    struct fake_cgroup cg = {{0}};
    struct fake_clock clock = {0};
    struct throttle_controller_config cfg;
    struct throttle_controller controller;
    struct throttle_controller_stats stats;
    struct throttle_record record;
    struct interference_candidate candidate;

    CHECK_TRUE(fake_cgroup_create(&cg));
    test_config(&cfg, &clock);
    cfg.min_throttle_duration_ms = 1000U;
    cfg.stale_after_ms = 2000U;

    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_init(&controller, &cfg));

    clock_set_ms(&clock, 0U);
    candidate = make_candidate(2001, 0.9, 0.8, 0.72,
                               true, true);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));

    clock_set_ms(&clock, 1999U);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, NULL, 0U, &stats));
    CHECK_EQ_SIZE(0U, stats.stale_releases);

    clock_set_ms(&clock, 2000U);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, NULL, 0U, &stats));
    CHECK_EQ_SIZE(1U, stats.stale_releases);
    CHECK_EQ_SIZE(1U, stats.release_actions);
    CHECK_TRUE(throttle_controller_get_record(&controller, 2001, &record));
    CHECK_EQ_INT(THROTTLE_STATE_COOLDOWN, record.state);

    throttle_controller_destroy(&controller);
    fake_cgroup_destroy(&cg);
    return true;
}

static bool test_release_all_and_get_record_copy(void)
{
    struct fake_cgroup cg = {{0}};
    struct fake_clock clock = {0};
    struct throttle_controller_config cfg;
    struct throttle_controller controller;
    struct throttle_controller_stats stats;
    struct throttle_record copy;
    struct throttle_record verify;
    struct interference_candidate candidates[2];

    CHECK_TRUE(fake_cgroup_create(&cg));
    test_config(&cfg, &clock);
    cfg.min_throttle_duration_ms = 0U;

    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_init(&controller, &cfg));

    candidates[0] = make_candidate(3001, 0.8, 0.8, 0.64,
                                   true, true);
    candidates[1] = make_candidate(3002, 0.7, 0.7, 0.49,
                                   true, true);

    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, candidates, 2U, &stats));
    CHECK_EQ_SIZE(2U, stats.throttle_actions);

    CHECK_TRUE(throttle_controller_get_record(&controller, 3001, &copy));
    copy.state = THROTTLE_STATE_NONE;
    CHECK_TRUE(throttle_controller_get_record(&controller, 3001, &verify));
    CHECK_EQ_INT(THROTTLE_STATE_THROTTLED, verify.state);

    clock_set_ms(&clock, 100U);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_release_all(&controller, &stats));
    CHECK_EQ_SIZE(2U, stats.release_actions);
    CHECK_TRUE(throttle_controller_get_record(&controller, 3001, &verify));
    CHECK_EQ_INT(THROTTLE_STATE_COOLDOWN, verify.state);
    CHECK_TRUE(throttle_controller_get_record(&controller, 3002, &verify));
    CHECK_EQ_INT(THROTTLE_STATE_COOLDOWN, verify.state);

    throttle_controller_destroy(&controller);
    fake_cgroup_destroy(&cg);
    return true;
}

static bool test_dry_run(void)
{
    struct fake_cgroup cg = {{0}};
    struct fake_clock clock = {0};
    struct throttle_controller_config cfg;
    struct throttle_controller controller;
    struct throttle_controller_stats stats;
    struct throttle_record record;
    struct interference_candidate candidate;
    char path[TEST_PATH_MAX];
    char text[128];

    CHECK_TRUE(fake_cgroup_create(&cg));
    test_config(&cfg, &clock);
    cfg.dry_run = true;

    CHECK_TRUE(cgroup_file_path(&cg, "throttled", "cpu.max",
                                path, sizeof(path)));
    CHECK_TRUE(write_text(path, "unchanged\n"));

    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_init(&controller, &cfg));
    CHECK_TRUE(read_text(path, text, sizeof(text)));
    CHECK_EQ_STR("unchanged\n", text);

    candidate = make_candidate(4001, 0.9, 0.9, 0.81,
                               true, true);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));
    CHECK_EQ_SIZE(1U, stats.throttle_actions);
    CHECK_TRUE(throttle_controller_get_record(&controller, 4001, &record));
    CHECK_EQ_INT(THROTTLE_STATE_THROTTLED, record.state);

    CHECK_TRUE(cgroup_file_path(&cg, "throttled", "cgroup.procs",
                                path, sizeof(path)));
    CHECK_TRUE(read_text(path, text, sizeof(text)));
    CHECK_EQ_STR("", text);

    throttle_controller_destroy(&controller);
    fake_cgroup_destroy(&cg);
    return true;
}

static bool test_invalid_inputs_capacity_and_clock(void)
{
    struct fake_cgroup cg = {{0}};
    struct fake_clock clock = {0};
    struct throttle_controller_config cfg;
    struct throttle_controller controller;
    struct throttle_controller_stats stats;
    struct interference_candidate candidates[2];

    CHECK_TRUE(fake_cgroup_create(&cg));

    throttle_controller_config_default(&cfg);
    cfg.low_score_threshold = 0.5;
    cfg.high_score_threshold = 0.4;
    CHECK_EQ_INT(THROTTLE_CONTROLLER_ERR_INVALID_ARG,
                 throttle_controller_init(&controller, &cfg));

    test_config(&cfg, &clock);
    cfg.max_records = 1U;
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_init(&controller, &cfg));

    candidates[0] = make_candidate(5001, 0.8, 0.8, 0.64,
                                   true, true);
    candidates[1] = make_candidate(5002, 0.8, 0.8, 0.64,
                                   true, true);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_ERR_NO_SPACE,
                 throttle_controller_apply_candidates(
                     &controller, candidates, 2U, &stats));
    CHECK_EQ_SIZE(0U, controller.record_count);

    candidates[1] = candidates[0];
    CHECK_EQ_INT(THROTTLE_CONTROLLER_ERR_INVALID_ARG,
                 throttle_controller_apply_candidates(
                     &controller, candidates, 2U, &stats));

    clock.fail = true;
    CHECK_EQ_INT(THROTTLE_CONTROLLER_ERR_TIME,
                 throttle_controller_apply_candidates(
                     &controller, candidates, 1U, &stats));

    throttle_controller_destroy(&controller);
    fake_cgroup_destroy(&cg);
    return true;
}

static bool test_cgroup_failure_preserves_state(void)
{
    struct fake_cgroup cg = {{0}};
    struct fake_clock clock = {0};
    struct throttle_controller_config cfg;
    struct throttle_controller controller;
    struct throttle_controller_stats stats;
    struct throttle_record record;
    struct interference_candidate candidate;
    char path[TEST_PATH_MAX];

    CHECK_TRUE(fake_cgroup_create(&cg));
    test_config(&cfg, &clock);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_OK,
                 throttle_controller_init(&controller, &cfg));

    CHECK_TRUE(cgroup_file_path(&cg, "throttled", "cgroup.procs",
                                path, sizeof(path)));
    CHECK_EQ_INT(0, unlink(path));
    CHECK_EQ_INT(0, mkdir(path, 0700));

    candidate = make_candidate(6001, 0.9, 0.9, 0.81,
                               true, true);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_ERR_CGROUP,
                 throttle_controller_apply_candidates(
                     &controller, &candidate, 1U, &stats));
    CHECK_EQ_SIZE(1U, stats.errors);
    CHECK_EQ_SIZE(0U, stats.throttle_actions);
    CHECK_TRUE(throttle_controller_get_record(&controller, 6001, &record));
    CHECK_EQ_INT(THROTTLE_STATE_NONE, record.state);
    CHECK_EQ_INT(THROTTLE_CONTROLLER_ERR_CGROUP, record.last_error);

    throttle_controller_destroy(&controller);
    fake_cgroup_destroy(&cg);
    return true;
}

static bool run_test(const char *name, bool (*test_fn)(void))
{
    bool ok = test_fn();

    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    return ok;
}

int main(void)
{
    bool ok = true;

    ok = run_test("defaults and helpers",
                  test_defaults_and_helpers) && ok;
    ok = run_test("init configures cgroups",
                  test_init_configures_cgroups) && ok;
    ok = run_test("policy, hysteresis, minimum duration, cooldown",
                  test_policy_hysteresis_and_cooldown) && ok;
    ok = run_test("stale release",
                  test_stale_release) && ok;
    ok = run_test("release_all and record copy",
                  test_release_all_and_get_record_copy) && ok;
    ok = run_test("dry-run",
                  test_dry_run) && ok;
    ok = run_test("invalid input, capacity, and clock failure",
                  test_invalid_inputs_capacity_and_clock) && ok;
    ok = run_test("cgroup failure preserves state",
                  test_cgroup_failure_preserves_state) && ok;

    if (!ok || g_failures != 0) {
        fprintf(stderr, "[FAIL] throttle_controller tests: %d failure(s)\n",
                g_failures);
        return EXIT_FAILURE;
    }

    printf("[PASS] all throttle_controller tests\n");
    return EXIT_SUCCESS;
}
