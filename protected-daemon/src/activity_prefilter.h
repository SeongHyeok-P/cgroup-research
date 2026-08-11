#ifndef ACTIVITY_PREFILTER_H
#define ACTIVITY_PREFILTER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "proc_activity.h"

enum activity_prefilter_status {
    ACTIVITY_PREFILTER_OK = 0,
    ACTIVITY_PREFILTER_ERR_INVALID_ARG = -1,
    ACTIVITY_PREFILTER_ERR_NO_SPACE = -2,
    ACTIVITY_PREFILTER_ERR_NO_MEMORY = -3
};

struct activity_prefilter_config {
    size_t max_candidates;
    uint64_t min_rss_pages;
};

struct activity_prefilter_observation {
    pid_t pid;
    struct proc_activity_sample sample;
    struct proc_activity_delta delta;
};

struct activity_prefilter_candidate {
    pid_t pid;
    double screening_score;

    struct proc_activity_delta delta;
};

struct activity_prefilter_stats {
    size_t observations;
    size_t valid_observations;
    size_t excluded_zombie;
    size_t excluded_small_rss;
    size_t excluded_no_signal;
    size_t eligible_candidates;
    size_t selected_candidates;
};

void activity_prefilter_config_default(
    struct activity_prefilter_config *cfg);

void activity_prefilter_stats_reset(
    struct activity_prefilter_stats *stats);

int activity_prefilter_is_eligible(
    const struct activity_prefilter_observation *obs,
    const struct activity_prefilter_config *cfg,
    int *eligible_out);

int activity_prefilter_select_topk(
    const struct activity_prefilter_observation *observations,
    size_t observation_count,
    const struct activity_prefilter_config *cfg,
    struct activity_prefilter_candidate *out,
    size_t out_cap,
    size_t *out_count,
    struct activity_prefilter_stats *stats);

const char *activity_prefilter_strerror(int rc);

#endif
