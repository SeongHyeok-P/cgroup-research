#define _POSIX_C_SOURCE 200809L

#include "interference_detector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct active_candidate_internal {
	pid_t pid;
	struct proc_activity_delta delta;
};

static double clamp01(double value)
{
	if (value < 0.0)
		return 0.0;

	if (value > 1.0)
		return 1.0;

	return value;
}

static double compute_final_score(double activity_score,double overlap_score)
{
	return clamp01(activity_score) * clamp01(overlap_score);
}

/*
 * To use qsort
 */
static int compare_active_candidate_desc(const void *a, const void *b)
{
	const struct active_candidate_internal *ca = a;
	const struct active_candidate_internal *cb = b;

	if (ca->delta.total_score < cb->delta.total_score)
		return 1;
	
	if (ca->delta.total_score > cb->delta.total_score)
		return -1;

	if (ca->pid > cb->pid)
		return 1;

	if (ca->pid < cb->pid)
		return -1;

	return 0;
}

static void stats_set_error(struct interference_detector_stats *stats,int rc,const char *reason)
{
	if (stats == NULL)
		return;

	stats->last_error = rc;

	if (reason == NULL) {
		stats->last_error_reason[0] = '\0';
		return;
	}

	(void)snprintf(stats->last_error_reason,sizeof(stats->last_error_reason),"%s",reason);
}

void interference_detector_config_default(struct interference_detector_config *cfg)
{
	if (cfg == NULL)
		return;

	memset(cfg,0,sizeof(*cfg));

	cfg->max_normal_candidates = 16U;
	
	cfg->activity_score_threshold = 0.0;
	cfg->overlap_score_threshold = 0.30;
	cfg->throttle_score_threshold = 0.20;
	
	proc_activity_config_default(&cfg->activity_config);
	bank_profile_config_default(&cfg->profile_config);

	/*
	 * Detector uses bank_profile only for a small Top-K set
	 * Keep this default modest; experiments may tune it later
	 */
	cfg->profile_config.max_pages_to_sample = 4096U;
	cfg->profile_config.sample_stride_pages = 16U;
}

void interference_process_state_reset(struct interference_process_state *state)
{
	if (state == NULL)
		return;
	
	memset(state,0,sizeof(*state));
	state->role = INTERFERENCE_PROCESS_NORMAL;
	state->alive = false;
	state->has_activity_sample = false;
}

void interference_candidate_reset(struct interference_candidate *candidate)
{
	if (candidate == NULL)
		return;
	
	memset(candidate,0,sizeof(*candidate));
}

void interference_detector_stats_reset(struct interference_detector_stats *stats)
{
	if (stats == NULL)
		return;
	
	memset(stats,0,sizeof(*stats));
	stats->last_error = INTERFERENCE_DETECTOR_OK;
}

static int build_protected_aggregate_profile(struct interference_process_state *processes,size_t process_count,const struct dram_mapping *mapping,
					     const struct interference_detector_config *cfg, struct bank_profile *protected_aggregate, struct interference_detector_stats *stats)
{
	size_t i;

	if (processes == NULL || mapping == NULL || cfg == NULL || protected_aggregate == NULL)
		return INTERFERENCE_DETECTOR_ERR_INVALID_ARG;

	bank_profile_reset(protected_aggregate);

	for (i = 0U; i < process_count; i++) {
		struct bank_profile profile;
		int rc;

		if (!processes[i].alive)
			continue;
		if (processes[i].role != INTERFERENCE_PROCESS_PROTECTED)
			continue;

		if (stats != NULL)
			stats->protected_processes++;
		
		rc = bank_profile_build(processes[i].pid,mapping,&cfg->profile_config,&profile);
		if (rc != BANK_PROFILE_OK) {
			if (stats != NULL)
				stats->profile_errors++;

			continue;
		}

		rc = bank_profile_merge(protected_aggregate,&profile);
		if (rc != BANK_PROFILE_OK) {
			if (stats != NULL)
				stats->profile_errors++;

			return INTERFERENCE_DETECTOR_ERR_PROFILE;
		}

		if (stats != NULL)
			stats->protected_profiles_built++;
	}

	if (protected_aggregate->entry_count == 0U) 
		return INTERFERENCE_DETECTOR_ERR_NO_PROTECTED_PROFILE;

	return INTERFERENCE_DETECTOR_OK;
}

static int collect_active_normal_candidates(struct interference_process_state *processes,size_t process_count,const struct interference_detector_config *cfg,
					    struct active_candidate_internal **active_out,size_t *active_count_out,struct interference_detector_stats *stats)
{	
	struct active_candidate_internal *active;
	size_t active_count = 0U;
	size_t i;

	if (processes == NULL || cfg == NULL || active_out == NULL || active_count_out == NULL)
		return INTERFERENCE_DETECTOR_ERR_INVALID_ARG;
	
	active = calloc(process_count , sizeof(*active));
	if (active == NULL)
		return INTERFERENCE_DETECTOR_ERR_NO_MEMORY;

	for (i = 0U; i < process_count; i++) {
		struct proc_activity_sample now;
		struct proc_activity_delta delta;
		int rc;

		if (!processes[i].alive)
			continue;

		if (processes[i].role != INTERFERENCE_PROCESS_NORMAL)
			continue;

		if (stats != NULL)
			stats->normal_processes++;

		rc = proc_activity_sample_now(processes[i].pid,&now);
		if (rc != PROC_ACTIVITY_OK) {
			processes[i].alive = false;
			processes[i].has_activity_sample = false;
			if (stats != NULL)
				stats->activity_errors++;

			continue;
		}

		if (stats != NULL)
			stats->activity_samples++;

		if (!processes[i].has_activity_sample) {
			processes[i].activity_sample = now;
			processes[i].has_activity_sample = true;
			continue;
		}

		rc = proc_activity_compute_delta(&processes[i].activity_sample,&now,&cfg->activity_config,&delta);

		processes[i].activity_sample = now;
		processes[i].has_activity_sample = true;

		if (rc != PROC_ACTIVITY_OK) {
			if (rc == PROC_ACTIVITY_ERR_PID_REUSED)
				processes[i].has_activity_sample = false;

			if (stats != NULL)
				stats->activity_errors++;

			continue;
		}

		if (!delta.active)
			continue;

		if (delta.total_score < cfg->activity_score_threshold)
			continue;

		active[active_count].pid = processes[i].pid;
		active[active_count].delta = delta;
		active_count++;
	}

	qsort(active,active_count,sizeof(*active),compare_active_candidate_desc);

	if (stats != NULL)
		stats->active_normal_candidates = active_count;

	*active_out = active;
	*active_count_out = active_count;

	return INTERFERENCE_DETECTOR_OK;
}

int interference_detector_run_once(struct interference_process_state *processes,size_t process_count,const struct dram_mapping *mapping,
				   const struct interference_detector_config *cfg, struct interference_candidate *out, size_t out_cap, 
				   size_t *out_count, struct interference_detector_stats *stats)
{
	struct bank_profile protected_aggregate;
	struct active_candidate_internal *active = NULL;
	size_t active_count = 0U;
	size_t limit;
	size_t produced = 0U;
	size_t i;
	int rc;

	if (out_count != NULL)
		*out_count = 0U;

	if (stats != NULL)
		interference_detector_stats_reset(stats);

	if (processes == NULL || mapping == NULL || cfg == NULL || out == NULL || out_count == NULL)
		return INTERFERENCE_DETECTOR_ERR_INVALID_ARG;

	if (process_count > 0U && out_cap == 0U)
		return INTERFERENCE_DETECTOR_ERR_INVALID_ARG;

	if (!dram_mapping_is_ready(mapping)) {
		stats_set_error(stats,INTERFERENCE_DETECTOR_ERR_NO_MAPPING,"DRAM mapping is not ready");
		return INTERFERENCE_DETECTOR_ERR_NO_MAPPING;
	}

	if (stats != NULL)
		stats->process_count = process_count;

	rc = build_protected_aggregate_profile(processes, process_count,mapping,cfg,&protected_aggregate,stats);
	if (rc != INTERFERENCE_DETECTOR_OK) {
		stats_set_error(stats,rc,"failed to build protected aggregate profile");
		return rc;
	}

	rc = collect_active_normal_candidates(processes, process_count, cfg,&active,&active_count,stats);
	if (rc != INTERFERENCE_DETECTOR_OK) {
		stats_set_error(stats,rc,"failed to collect active normal candidates");
		return rc;
	}

	limit = active_count;
	if (cfg->max_normal_candidates > 0U && limit > cfg->max_normal_candidates) 
		limit = cfg->max_normal_candidates;
	
	if (limit > out_cap)
		limit = out_cap;

	for (i = 0U; i < limit; i++) {
		struct bank_profile normal_profile;
		double overlap_score = 0.0;
		uint64_t common_pages = 0U;

		rc = bank_profile_build(active[i].pid,mapping,&cfg->profile_config,&normal_profile);
		if (rc != BANK_PROFILE_OK) {
			if (stats != NULL)
				stats->profile_errors++;

			continue;
		}

		if (stats != NULL)
			stats->normal_profiles_built++;

		rc = bank_profile_overlap_score(&protected_aggregate, &normal_profile,&overlap_score,&common_pages);
		if (rc != BANK_PROFILE_OK) {
			if (stats != NULL)
				stats->profile_errors++;

			continue;
		}

		interference_candidate_reset(&out[produced]);
		out[produced].pid = active[i].pid;

		out[produced].activity_score = active[i].delta.total_score;
		out[produced].cpu_score = active[i].delta.cpu_score;
		out[produced].fault_score = active[i].delta.fault_score;
		out[produced].rss_score = active[i].delta.rss_score;

		out[produced].overlap_score = overlap_score;
		out[produced].common_pages = common_pages;
		out[produced].final_score = compute_final_score(out[produced].activity_score,out[produced].overlap_score);

		out[produced].active = true;
		out[produced].should_throttle = (overlap_score >= cfg->overlap_score_threshold) && (out[produced].final_score >= cfg->throttle_score_threshold);

		(void)snprintf(out[produced].reason,sizeof(out[produced].reason),"activity=%.4f overlap=%.4f final=%.4f common_pages=%llu throttle=%s",
										out[produced].activity_score, 
										out[produced].overlap_score,
										out[produced].final_score,
										(unsigned long long)out[produced].common_pages,
										out[produced].should_throttle ? "true" : "false");
		produced++;
	}

	free(active);

	if (stats != NULL)
		stats->output_candidates = produced;

	*out_count = produced;

	return INTERFERENCE_DETECTOR_OK;
}

const char *interference_detector_strerror(int rc)
{
	switch(rc) {
		case INTERFERENCE_DETECTOR_OK:
			return "ok";
		case INTERFERENCE_DETECTOR_ERR_INVALID_ARG:
			return "invalid argument";
		case INTERFERENCE_DETECTOR_ERR_NO_MAPPING:
			return "DRAM mapping is not ready";
		case INTERFERENCE_DETECTOR_ERR_NO_MEMORY:
			return "out of memory";
		case INTERFERENCE_DETECTOR_ERR_NO_PROTECTED_PROFILE:
			return "no protected profile available";
		case INTERFERENCE_DETECTOR_ERR_PROFILE:
			return "bank profile error";
		case INTERFERENCE_DETECTOR_ERR_ACTIVITY:
			return "activity sampling error";
		default:
			return "unknown interference detector error";
	}
}

