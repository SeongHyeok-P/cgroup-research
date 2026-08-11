#include "activity_prefilter.h"

#include <stdlib.h>
#include <string.h>

#define ACTIVITY_PREFILTER_DEFAULT_TOP_K1 64U

static double max3(double a, double b, double c)
{
	double max_value = a;

	if (b > max_value)
		max_value = b;

	if (c > max_value)
		max_value = c;

	return max_value;
}

static int observation_is_valid(const struct activity_prefilter_observation *obs)
{
	if (obs == NULL)
		return 0;

	if (obs->pid <= 0)
		return 0;

	if (!obs->sample.valid || !obs->delta.valid)
		return 0;

	if (obs->sample.pid != obs->pid || obs->delta.pid != obs->pid)
		return 0;

	return 1;
}

static int has_cheap_activity_signal(const struct activity_prefilter_observation *obs)
{
	if (obs->delta.cpu_ticks_delta > 0U)
		return 1;

	if (obs->delta.minflt_delta > 0U || obs->delta.majflt_delta > 0U)
		return 1;

	if (obs->sample.state == 'R' || obs->sample.state == 'D')
		return 1;

	return 0;
}

/*
 * qsort
 */
static int compare_candidate_desc(const void *lhs, const void *rhs)
{
	const struct activity_prefilter_candidate *a = lhs;
	const struct activity_prefilter_candidate *b = rhs;

	if (a->screening_score < b->screening_score)
		return 1;

	if (a->screening_score > b->screening_score)
		return -1;

	if (a->pid > b->pid)
		return 1;

	if (a->pid < b->pid)
		return -1;

	return 0;
}

void activity_prefilter_config_default(struct activity_prefilter_config *cfg)
{
	if (cfg == NULL)
		return;

	memset(cfg,0,sizeof(*cfg));
	cfg->max_candidates = ACTIVITY_PREFILTER_DEFAULT_TOP_K1;
	cfg->min_rss_pages = 1U;
}

void activity_prefilter_stats_reset(struct activity_prefilter_stats *stats)
{
	if (stats == NULL)
		return;

	memset(stats,0,sizeof(*stats));
}

int activity_prefilter_is_eligible(const struct activity_prefilter_observation *obs,const struct activity_prefilter_config *cfg,int *eligible_out)
{
	if (obs == NULL || cfg == NULL || eligible_out == NULL)
		return ACTIVITY_PREFILTER_ERR_INVALID_ARG;

	*eligible_out = 0;

	if (!observation_is_valid(obs))
		return ACTIVITY_PREFILTER_OK;

	if (obs->sample.state == 'Z')
		return ACTIVITY_PREFILTER_OK;

	if (obs->sample.rss_pages <= 0 || (uint64_t)obs->sample.rss_pages < cfg->min_rss_pages)
		return ACTIVITY_PREFILTER_OK;

	if (!has_cheap_activity_signal(obs))
		return ACTIVITY_PREFILTER_OK;

	*eligible_out = 1;
	return ACTIVITY_PREFILTER_OK;
}

int activity_prefilter_select_topk(const struct activity_prefilter_observation *observations, size_t observation_count,
				   const struct activity_prefilter_config *cfg,
				   struct activity_prefilter_candidate *out,
	    			   size_t out_cap,
			   	   size_t *out_count,
		   		   struct activity_prefilter_stats *stats)
{
	struct activity_prefilter_candidate *eligible;
	size_t eligible_count = 0U;
	size_t selected_count;
	size_t i;

	if (out_count == NULL)
		return ACTIVITY_PREFILTER_ERR_INVALID_ARG;

	*out_count = 0U;

	if (stats != NULL)
		activity_prefilter_stats_reset(stats);

	if (cfg == NULL)
		return ACTIVITY_PREFILTER_ERR_INVALID_ARG;

	if (observation_count > 0U && observations == NULL)
		return ACTIVITY_PREFILTER_ERR_INVALID_ARG;

	if (cfg->max_candidates > 0U && out == NULL)
		return ACTIVITY_PREFILTER_ERR_INVALID_ARG;

	if (stats != NULL)
		stats->observations = observation_count;

	if (observation_count == 0U)
		return ACTIVITY_PREFILTER_OK;

	eligible = calloc(observation_count,sizeof(*eligible));
	if (eligible == NULL)
		return ACTIVITY_PREFILTER_ERR_NO_MEMORY;

	for (i = 0U; i < observation_count; i++) {
		const struct activity_prefilter_observation *obs = &observations[i];
		int eligible_now = 0;
		int rc;

		if (!observation_is_valid(obs))
			continue;

		if (stats != NULL)
			stats->valid_observations++;

		if (obs->sample.state == 'Z') {
			if (stats != NULL)
				stats->excluded_zombie++;
			continue;
		}

		if (obs->sample.rss_pages <= 0 || (uint64_t)obs->sample.rss_pages < cfg->min_rss_pages) {
			if (stats != NULL)
				stats->excluded_small_rss++;
			continue;
		}

		rc = activity_prefilter_is_eligible(obs,cfg,&eligible_now);
		if (rc != ACTIVITY_PREFILTER_OK) {
			free(eligible);
			return rc;
		}

		if (!eligible_now) {
			if (stats != NULL)
				stats->excluded_no_signal++;
			continue;
		}

		eligible[eligible_count].pid = obs->pid;
		eligible[eligible_count].screening_score = max3(obs->delta.cpu_score,obs->delta.fault_score,obs->delta.rss_score);
		eligible[eligible_count].delta = obs->delta;
		eligible_count++;
	}

	qsort(eligible,eligible_count,sizeof(*eligible),compare_candidate_desc);

	if (stats != NULL)
		stats->eligible_candidates = eligible_count;

	selected_count = eligible_count;

	if (cfg->max_candidates > 0U && selected_count > cfg->max_candidates)
		selected_count = cfg->max_candidates;

	if (out_cap < selected_count) {
		free(eligible);
		return ACTIVITY_PREFILTER_ERR_NO_SPACE;
	}

	if (selected_count > 0U) {
		memcpy(out,eligible,selected_count * sizeof(*out));
	}

	free(eligible);

	if (stats != NULL)
		stats->selected_candidates = selected_count;

	*out_count = selected_count;
	return ACTIVITY_PREFILTER_OK;
}

const char *activity_prefilter_strerror(int rc)
{
    switch (rc) {
    case ACTIVITY_PREFILTER_OK:
        return "ok";
    case ACTIVITY_PREFILTER_ERR_INVALID_ARG:
        return "invalid argument";
    case ACTIVITY_PREFILTER_ERR_NO_SPACE:
        return "insufficient output space";
    case ACTIVITY_PREFILTER_ERR_NO_MEMORY:
        return "out of memory";
    default:
        return "unknown activity prefilter error";
    }
}
