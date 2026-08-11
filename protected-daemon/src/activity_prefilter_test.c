#include "activity_prefilter.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK_TRUE(expr)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "[FAIL] %s:%d: %s\n",                            \
                    __FILE__, __LINE__, #expr);                                \
            return -1;                                                         \
        }                                                                      \
    } while (0)

#define CHECK_EQ_LONG(expected, actual)                                        \
    do {                                                                       \
        long expected_value = (long)(expected);                                \
        long actual_value = (long)(actual);                                    \
        if (expected_value != actual_value) {                                  \
            fprintf(stderr,                                                    \
                    "[FAIL] %s:%d: expected %ld, got %ld\n",                 \
                    __FILE__, __LINE__, expected_value, actual_value);          \
            return -1;                                                         \
        }                                                                      \
    } while (0)

#define CHECK_DOUBLE(expected, actual)                                         \
    do {                                                                       \
        double expected_value = (expected);                                    \
        double actual_value = (actual);                                        \
        double diff = expected_value - actual_value;                           \
        if (diff < 0.0)                                                        \
            diff = -diff;                                                      \
        if (diff > 1e-12) {                                                    \
            fprintf(stderr,                                                    \
                    "[FAIL] %s:%d: expected %.12f, got %.12f\n",             \
                    __FILE__, __LINE__, expected_value, actual_value);          \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static struct activity_prefilter_observation make_obs(
    pid_t pid,
    char state,
    int64_t rss_pages,
    uint64_t cpu_ticks_delta,
    uint64_t minflt_delta,
    uint64_t majflt_delta,
    double cpu_score,
    double fault_score,
    double rss_score)
{
    struct activity_prefilter_observation obs;

    memset(&obs, 0, sizeof(obs));
    obs.pid = pid;

    obs.sample.valid = true;
    obs.sample.pid = pid;
    obs.sample.state = state;
    obs.sample.rss_pages = rss_pages;

    obs.delta.valid = true;
    obs.delta.pid = pid;
    obs.delta.cpu_ticks_delta = cpu_ticks_delta;
    obs.delta.minflt_delta = minflt_delta;
    obs.delta.majflt_delta = majflt_delta;
    obs.delta.cpu_score = cpu_score;
    obs.delta.fault_score = fault_score;
    obs.delta.rss_score = rss_score;

    return obs;
}

static int test_defaults(void)
{
    struct activity_prefilter_config cfg;

    activity_prefilter_config_default(&cfg);

    CHECK_EQ_LONG(64, cfg.max_candidates);
    CHECK_EQ_LONG(1, cfg.min_rss_pages);
    return 0;
}

static int test_permissive_activity_gate(void)
{
    struct activity_prefilter_config cfg;
    struct activity_prefilter_observation obs;
    int eligible = 0;

    activity_prefilter_config_default(&cfg);

    obs = make_obs(1001, 'S', 100, 1, 0, 0,
                   0.01, 0.0, 0.1);
    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_is_eligible(&obs, &cfg, &eligible));
    CHECK_TRUE(eligible);

    obs = make_obs(1002, 'S', 100, 0, 1, 0,
                   0.0, 0.01, 0.1);
    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_is_eligible(&obs, &cfg, &eligible));
    CHECK_TRUE(eligible);

    obs = make_obs(1003, 'R', 100, 0, 0, 0,
                   0.0, 0.0, 0.1);
    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_is_eligible(&obs, &cfg, &eligible));
    CHECK_TRUE(eligible);

    obs.sample.state = 'D';
    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_is_eligible(&obs, &cfg, &eligible));
    CHECK_TRUE(eligible);

    return 0;
}

static int test_zombie_small_rss_and_idle_are_excluded(void)
{
    struct activity_prefilter_config cfg;
    struct activity_prefilter_observation obs;
    int eligible = 1;

    activity_prefilter_config_default(&cfg);

    obs = make_obs(2001, 'Z', 100, 10, 10, 1,
                   0.9, 0.9, 0.9);
    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_is_eligible(&obs, &cfg, &eligible));
    CHECK_TRUE(!eligible);

    obs = make_obs(2002, 'S', 0, 10, 10, 1,
                   0.9, 0.9, 0.0);
    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_is_eligible(&obs, &cfg, &eligible));
    CHECK_TRUE(!eligible);

    obs = make_obs(2003, 'S', 100, 0, 0, 0,
                   0.0, 0.0, 0.1);
    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_is_eligible(&obs, &cfg, &eligible));
    CHECK_TRUE(!eligible);

    return 0;
}

static int test_single_strong_signal_ranks_high(void)
{
    struct activity_prefilter_config cfg;
    struct activity_prefilter_observation obs[3];
    struct activity_prefilter_candidate out[3];
    size_t count = 0U;

    activity_prefilter_config_default(&cfg);
    cfg.max_candidates = 3U;

    obs[0] = make_obs(3001, 'R', 100000, 1, 0, 0,
                      0.01, 0.00, 1.00);
    obs[1] = make_obs(3002, 'R', 1000, 10, 100, 0,
                      0.70, 0.80, 0.20);
    obs[2] = make_obs(3003, 'R', 1000, 10, 10, 0,
                      0.60, 0.50, 0.40);

    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_select_topk(
                      obs, 3U, &cfg, out, 3U, &count, NULL));

    CHECK_EQ_LONG(3, count);
    CHECK_EQ_LONG(3001, out[0].pid);
    CHECK_DOUBLE(1.0, out[0].screening_score);
    CHECK_EQ_LONG(3002, out[1].pid);
    CHECK_DOUBLE(0.8, out[1].screening_score);
    CHECK_EQ_LONG(3003, out[2].pid);
    CHECK_DOUBLE(0.6, out[2].screening_score);

    return 0;
}

static int test_topk_limit_and_tie_break(void)
{
    struct activity_prefilter_config cfg;
    struct activity_prefilter_observation obs[5];
    struct activity_prefilter_candidate out[3];
    size_t count = 0U;

    activity_prefilter_config_default(&cfg);
    cfg.max_candidates = 3U;

    /*
     * 4001, 4002, 4003 all have the same screening score = 0.9.
     * Therefore PID order alone must break the tie.
     */
    obs[0] = make_obs(4005, 'R', 100, 1, 0, 0,
                      0.5, 0.1, 0.1);

    obs[1] = make_obs(4002, 'R', 100, 1, 0, 0,
                      0.9, 0.1, 0.1);

    obs[2] = make_obs(4003, 'R', 100, 1, 0, 0,
                      0.9, 0.1, 0.1);

    obs[3] = make_obs(4001, 'R', 100, 1, 0, 0,
                      0.9, 0.1, 0.1);

    obs[4] = make_obs(4004, 'R', 100, 1, 0, 0,
                      0.7, 0.1, 0.1);

    CHECK_EQ_LONG(
        ACTIVITY_PREFILTER_OK,
        activity_prefilter_select_topk(
            obs, 5U, &cfg, out, 3U, &count, NULL));

    CHECK_EQ_LONG(3, count);

    CHECK_EQ_LONG(4001, out[0].pid);
    CHECK_EQ_LONG(4002, out[1].pid);
    CHECK_EQ_LONG(4003, out[2].pid);

    CHECK_DOUBLE(0.9, out[0].screening_score);
    CHECK_DOUBLE(0.9, out[1].screening_score);
    CHECK_DOUBLE(0.9, out[2].screening_score);

    return 0;
}

static int test_stats(void)
{
    struct activity_prefilter_config cfg;
    struct activity_prefilter_observation obs[5];
    struct activity_prefilter_candidate out[5];
    struct activity_prefilter_stats stats;
    size_t count = 0U;

    activity_prefilter_config_default(&cfg);

    obs[0] = make_obs(5001, 'R', 100, 1, 0, 0,
                      0.1, 0.0, 0.1);
    obs[1] = make_obs(5002, 'Z', 100, 1, 0, 0,
                      0.1, 0.0, 0.1);
    obs[2] = make_obs(5003, 'S', 0, 1, 0, 0,
                      0.1, 0.0, 0.0);
    obs[3] = make_obs(5004, 'S', 100, 0, 0, 0,
                      0.0, 0.0, 0.1);
    obs[4] = make_obs(5005, 'D', 100, 0, 0, 0,
                      0.0, 0.0, 0.1);

    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_select_topk(
                      obs, 5U, &cfg, out, 5U, &count, &stats));

    CHECK_EQ_LONG(2, count);
    CHECK_EQ_LONG(5, stats.observations);
    CHECK_EQ_LONG(5, stats.valid_observations);
    CHECK_EQ_LONG(1, stats.excluded_zombie);
    CHECK_EQ_LONG(1, stats.excluded_small_rss);
    CHECK_EQ_LONG(1, stats.excluded_no_signal);
    CHECK_EQ_LONG(2, stats.eligible_candidates);
    CHECK_EQ_LONG(2, stats.selected_candidates);
    return 0;
}

static int test_invalid_observation_is_skipped(void)
{
    struct activity_prefilter_config cfg;
    struct activity_prefilter_observation obs[2];
    struct activity_prefilter_candidate out[2];
    struct activity_prefilter_stats stats;
    size_t count = 0U;

    activity_prefilter_config_default(&cfg);
    memset(&obs[0], 0, sizeof(obs[0]));
    obs[1] = make_obs(6002, 'R', 100, 1, 0, 0,
                      0.1, 0.1, 0.1);

    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_select_topk(
                      obs, 2U, &cfg, out, 2U, &count, &stats));

    CHECK_EQ_LONG(1, count);
    CHECK_EQ_LONG(6002, out[0].pid);
    CHECK_EQ_LONG(1, stats.valid_observations);
    return 0;
}

static int test_output_capacity_is_all_or_nothing(void)
{
    struct activity_prefilter_config cfg;
    struct activity_prefilter_observation obs[3];
    struct activity_prefilter_candidate out[2];
    struct activity_prefilter_candidate before[2];
    size_t count = 999U;

    activity_prefilter_config_default(&cfg);
    cfg.max_candidates = 3U;

    obs[0] = make_obs(7001, 'R', 100, 1, 0, 0,
                      0.3, 0.2, 0.1);
    obs[1] = make_obs(7002, 'R', 100, 1, 0, 0,
                      0.4, 0.2, 0.1);
    obs[2] = make_obs(7003, 'R', 100, 1, 0, 0,
                      0.5, 0.2, 0.1);

    memset(out, 0x5A, sizeof(out));
    memcpy(before, out, sizeof(out));

    CHECK_EQ_LONG(ACTIVITY_PREFILTER_ERR_NO_SPACE,
                  activity_prefilter_select_topk(
                      obs, 3U, &cfg, out, 2U, &count, NULL));

    CHECK_EQ_LONG(0, count);
    CHECK_TRUE(memcmp(out, before, sizeof(out)) == 0);
    return 0;
}

static int test_zero_observations(void)
{
    struct activity_prefilter_config cfg;
    size_t count = 99U;

    activity_prefilter_config_default(&cfg);
    cfg.max_candidates = 0U;

    CHECK_EQ_LONG(ACTIVITY_PREFILTER_OK,
                  activity_prefilter_select_topk(
                      NULL, 0U, &cfg, NULL, 0U, &count, NULL));
    CHECK_EQ_LONG(0, count);
    return 0;
}

static int test_invalid_arguments(void)
{
    struct activity_prefilter_config cfg;
    struct activity_prefilter_candidate out[1];
    size_t count = 1U;

    activity_prefilter_config_default(&cfg);

    CHECK_EQ_LONG(ACTIVITY_PREFILTER_ERR_INVALID_ARG,
                  activity_prefilter_select_topk(
                      NULL, 1U, &cfg, out, 1U, &count, NULL));
    CHECK_EQ_LONG(0, count);

    CHECK_EQ_LONG(ACTIVITY_PREFILTER_ERR_INVALID_ARG,
                  activity_prefilter_select_topk(
                      NULL, 0U, NULL, NULL, 0U, &count, NULL));
    CHECK_EQ_LONG(0, count);
    return 0;
}

static int test_total_score_does_not_affect_ranking(void)
{
    struct activity_prefilter_config cfg;
    struct activity_prefilter_observation obs[3];
    struct activity_prefilter_candidate out[3];
    size_t count = 0U;

    activity_prefilter_config_default(&cfg);
    cfg.max_candidates = 3U;

    obs[0] = make_obs(8003, 'R', 100, 1, 0, 0,
                      0.8, 0.1, 0.1);

    obs[1] = make_obs(8001, 'R', 100, 1, 0, 0,
                      0.8, 0.1, 0.1);

    obs[2] = make_obs(8002, 'R', 100, 1, 0, 0,
                      0.8, 0.1, 0.1);

    /*
     * Deliberately give wildly different legacy total scores.
     * Proposed pre-filter must completely ignore them.
     */
    obs[0].delta.total_score = 1.0;
    obs[1].delta.total_score = 0.0;
    obs[2].delta.total_score = 0.5;

    CHECK_EQ_LONG(
        ACTIVITY_PREFILTER_OK,
        activity_prefilter_select_topk(
            obs, 3U, &cfg, out, 3U, &count, NULL));

    CHECK_EQ_LONG(3, count);

    /*
     * Same screening score, therefore PID order only.
     *
     * If total_score accidentally participates in ranking,
     * this test will fail.
     */
    CHECK_EQ_LONG(8001, out[0].pid);
    CHECK_EQ_LONG(8002, out[1].pid);
    CHECK_EQ_LONG(8003, out[2].pid);

    return 0;
}

static void run_test(const char *name, int (*fn)(void))
{
    if (fn() == 0)
        printf("[PASS] %s\n", name);
    else
        failures++;
}
int main(void)
{
    run_test("defaults", test_defaults);
    run_test("permissive activity gate", test_permissive_activity_gate);
    run_test("zombie, small RSS, and idle excluded",
             test_zombie_small_rss_and_idle_are_excluded);
    run_test("single strong signal ranks high",
             test_single_strong_signal_ranks_high);
    run_test("Top-K limit and tie break",
             test_topk_limit_and_tie_break);
    run_test("stats", test_stats);
    run_test("invalid observation is skipped",
             test_invalid_observation_is_skipped);
    run_test("output capacity is all-or-nothing",
             test_output_capacity_is_all_or_nothing);
    run_test("zero observations", test_zero_observations);
    run_test("invalid arguments", test_invalid_arguments);
    run_test(
    "total score does not affect ranking",
    test_total_score_does_not_affect_ranking);

    if (failures != 0) {
        fprintf(stderr,
                "[FAIL] activity_prefilter tests: %d failure(s)\n",
                failures);
        return 1;
    }

    printf("[PASS] all activity_prefilter tests\n");
    return 0;
}
