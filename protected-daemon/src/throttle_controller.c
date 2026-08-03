#define _POSIX_C_SOURCE 200809L

#include "throttle_controller.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NSEC_PER_MSEC 1000000ULL

static bool valid_control_group(enum proc_group group)
{
	return group == GROUP_PROTECTED || group == GROUP_BACKGROUND || group == GROUP_THROTTLED;
}

static size_t bounded_strlen(const char *s, size_t cap)
{
	size_t len = 0U;

	if (s == NULL)
		return 0U;

	while (len < cap && s[len] != '\0')
		len++;

	return len;
}

static bool valid_control_value(const char *value,size_t cap)
{
	size_t len;
	size_t i;

	if (value == NULL || cap == 0U)
		return false;

	len = bounded_strlen(value,cap);
	if (len == 0U || len >= cap)
		return false;

	for (i = 0U; i < len; i++) {
		if (value[i] == '\n' || value[i] == '\r')
			return false;
	}

	return true;
}

static int validate_config(const struct throttle_controller_config *cfg)
{
	if (cfg == NULL)
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	if (!valid_control_group(cfg->normal_group) || 
	    !valid_control_group(cfg->throttled_group) ||
	    !valid_control_group(cfg->protected_group))
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	if (cfg->normal_group == cfg->throttled_group || 
	    cfg->normal_group == cfg->protected_group ||
	    cfg-> throttled_group == cfg->protected_group)
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	if (!isfinite(cfg->low_score_threshold) || 
	    !isfinite(cfg->high_score_threshold) ||
	    cfg->low_score_threshold < 0.0 ||
	    cfg->high_score_threshold > 1.0 ||
	    cfg->low_score_threshold > cfg->high_score_threshold)
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	if (cfg->max_records == 0U)
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	if (cfg->configure_throttled_cpu_max && !valid_control_value(cfg->throttled_cpu_max,sizeof(cfg->throttled_cpu_max)))
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	if (cfg->configure_protected_memory_low && !valid_control_value(cfg->protected_memory_low,sizeof(cfg->protected_memory_low)))
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	return THROTTLE_CONTROLLER_OK;
}

static int monotonic_now_ns(uint64_t *now_ns)
{
	struct timespec ts;

	if (now_ns == NULL)
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	if (clock_gettime(CLOCK_MONOTONIC,&ts) != 0)
		return THROTTLE_CONTROLLER_ERR_TIME;

	*now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

	return THROTTLE_CONTROLLER_OK;
}

static int controller_now_ns(const struct throttle_controller *controller, uint64_t *now_ns)
{
	int rc;

	if (controller == NULL || now_ns == NULL)
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	if (controller->cfg.clock_fn == NULL)
		return monotonic_now_ns(now_ns);

	rc = controller->cfg.clock_fn(controller->cfg.clock_ctx,now_ns);
	if (rc != THROTTLE_CONTROLLER_OK)
		return THROTTLE_CONTROLLER_ERR_TIME;

	return THROTTLE_CONTROLLER_OK;
}

static uint64_t ms_to_ns(uint64_t ms)
{	
	/* prevent overflow */
	if (ms > UINT64_MAX / NSEC_PER_MSEC)
		return UINT64_MAX;

	return ms * NSEC_PER_MSEC;
}

/*
 * time out check func now - start >= duration_ns -> true else false
 */
static bool elapsed_at_least(uint64_t now_ns, uint64_t start_ns, uint64_t duration_ms)
{
	uint64_t duration_ns = ms_to_ns(duration_ms);

	if (now_ns < start_ns)
		return false;

	return now_ns - start_ns >= duration_ns;
}

static void set_reason(char *dst,size_t dst_size,const char *text)
{
	if (dst == NULL || dst_size == 0U)
		return;

	if (text == NULL) {
		dst[0] = '\0';
		return;
	}

	(void)snprintf(dst,dst_size,"%s",text);
}

static void set_stats_error(struct throttle_controller_stats *stats, int rc, const char *reason)
{
	if (stats == NULL)
		return;

	stats->errors++;
	stats->last_error = rc;
	set_reason(stats->last_error_reason,sizeof(stats->last_error_reason),reason);
}

/* 
 * read / write
 */						/* find where			   find what */
static struct throttle_record *find_record(struct throttle_controller *controller,pid_t pid)
{
	size_t i;

	/*
	 * controller 내부에 저장된 배열(records)을 처음부터 끝까지 탐색
	 * 찾으려는 pid와 일치하는 record를 발견했다면 
	 * 그 record의 메모리 주소(&)를 반환
	 */
	for (i = 0U; i < controller->record_count; i++) {
		if (controller->records[i].pid == pid)
			return &controller->records[i];
	}

	return NULL;
}

/*
 * read only
 */
static const struct throttle_record *find_record_const(const struct throttle_controller *controller,pid_t pid)
{
	size_t i;

	for (i = 0U; i < controller->record_count; i++) {
		if (controller->records[i].pid == pid)
			return &controller->records[i];
	}
	
	return NULL;
}

static struct throttle_record *create_record(struct throttle_controller *controller, pid_t pid, uint64_t now_ns)
{
	struct throttle_record *record;

	if (controller->record_count >= controller->record_cap)
		return NULL;

	record = &controller->records[controller->record_count++];
	throttle_controller_record_reset(record);

	record->pid = pid;
	record->state = THROTTLE_STATE_NONE;
	record->first_seen_ns = now_ns;
	record->last_seen_ns = now_ns;
	record->state_entered_ns = now_ns;
	set_reason(record->reason,sizeof(record->reason), "new record");

	return record;
}

static bool candidate_requests_throttle(const struct throttle_controller *controller,const struct interference_candidate *candidate)
{
	return candidate->active && candidate->should_throttle && (candidate->final_score >= controller->cfg.high_score_threshold);
}

static int move_pid(const struct throttle_controller *controller,pid_t pid, enum proc_group group)
{
	if (controller->cfg.dry_run)
		return THROTTLE_CONTROLLER_OK;

	if (cgroup_move_pid(pid,group) != 0)
		return THROTTLE_CONTROLLER_ERR_CGROUP;

	return THROTTLE_CONTROLLER_OK;
}

static int throttle_record_pid(struct throttle_controller *controller,struct throttle_record *record, uint64_t now_ns,struct throttle_controller_stats *stats)
{
	int rc;

	rc = move_pid(controller,record->pid,controller->cfg.throttled_group);
	if (rc != THROTTLE_CONTROLLER_OK) {
		record->last_error = rc;
		set_reason(record->reason, sizeof(record->reason),"failed to move pid to throttled group");
		set_stats_error(stats,rc,record->reason);
		return rc;
	}

	record->state = THROTTLE_STATE_THROTTLED;
	record->state_entered_ns = now_ns;
	record->last_action_ns = now_ns;
	record->last_error = THROTTLE_CONTROLLER_OK;
	(void)snprintf(record->reason,sizeof(record->reason),"throttled: final=%.4f activity=%.4f overlap=%.4f",
							      record->last_final_score,
							      record->last_activity_score,
							      record->last_overlap_score);

	if (stats != NULL)
		stats->throttle_actions++;

	return THROTTLE_CONTROLLER_OK;
}

static int release_record_pid(struct throttle_controller *controller,struct throttle_record *record, uint64_t now_ns,bool stale, struct throttle_controller_stats *stats)
{
	int rc;

	rc = move_pid(controller,record->pid,controller->cfg.normal_group);
	if (rc != THROTTLE_CONTROLLER_OK) {
		record->last_error = rc;
		set_reason(record->reason,sizeof(record->reason),"failed to move pid to normal group");
		set_stats_error(stats,rc,record->reason);
		return rc;
	}

	record->state = THROTTLE_STATE_COOLDOWN;
	record->state_entered_ns = now_ns;
	record->last_action_ns = now_ns;
	record->last_error = THROTTLE_CONTROLLER_OK;

	if (stale)
		set_reason(record->reason,sizeof(record->reason),"released: stale candidate");
	else
		(void)snprintf(record->reason,sizeof(record->reason),"released: final=%.4f low=%.4f",record->last_final_score, controller->cfg.low_score_threshold);

	if (stats != NULL) {
		stats->release_actions++;
		if (stale)
			stats->stale_releases++;
	}

	return THROTTLE_CONTROLLER_OK;
}

static bool candidate_valid(const struct interference_candidate *candidate)
{
	if (candidate == NULL || candidate->pid <= 0)
		return false;

	if (!isfinite(candidate->activity_score) || !isfinite(candidate->overlap_score) || !isfinite(candidate->final_score))
		return false;

	if (candidate->activity_score < 0.0 || 
	    candidate->activity_score > 1.0 ||
	    candidate->overlap_score < 0.0 ||
	    candidate->overlap_score > 1.0 ||
	    candidate->final_score < 0.0 ||
	    candidate->final_score > 1.0)
		return false;

	return true;
}

static int prevalidate_candidates(const struct throttle_controller *controller, const struct interference_candidate *candidates,size_t candidate_count)
{
	size_t i;
	size_t j;
	size_t new_records = 0U;

	if (candidate_count > 0U && candidates == NULL)
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	for (i = 0U; i < candidate_count; i++) {
		if (!candidate_valid(&candidates[i]))
			return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

		for (j = 0U; j < i; j++) {
			if (candidates[i].pid == candidates[j].pid)
				return THROTTLE_CONTROLLER_ERR_INVALID_ARG;
		}

		if (find_record_const(controller,candidates[i].pid) == NULL)
			new_records++;
	}

	if (new_records > controller->record_cap - controller->record_count)
		return THROTTLE_CONTROLLER_ERR_NO_SPACE;

	return THROTTLE_CONTROLLER_OK;
}

void throttle_controller_config_default(struct throttle_controller_config *cfg)
{
	if (cfg == NULL)
		return;

	memset(cfg,0,sizeof(*cfg));

	cfg->normal_group = GROUP_BACKGROUND;
	cfg->throttled_group = GROUP_THROTTLED;
	cfg->protected_group = GROUP_PROTECTED;

	(void)snprintf(cfg->throttled_cpu_max, sizeof(cfg->throttled_cpu_max), "%s", "20000 100000");
	(void)snprintf(cfg->protected_memory_low,sizeof(cfg->protected_memory_low), "%s", "0");

	cfg->high_score_threshold = 0.20;
	cfg->low_score_threshold = 0.10;

	cfg->min_throttle_duration_ms = 3000U;
	cfg->cooldown_duration_ms = 2000U;
	cfg->stale_after_ms = 2000U;

	cfg->max_records = 256U;

	cfg->dry_run = false;
	cfg->configure_throttled_cpu_max = true;
	cfg->configure_protected_memory_low = false;

	cfg->clock_fn = NULL;
	cfg->clock_ctx = NULL;
}

void throttle_controller_stats_reset(struct throttle_controller_stats *stats)
{
	if (stats == NULL)
		return; 
	
	memset(stats,0,sizeof(*stats));
	stats->last_error = THROTTLE_CONTROLLER_OK;
}

void throttle_controller_record_reset(struct throttle_record *record)
{
	if (record == NULL)
		return;

	memset(record,0,sizeof(*record));
	record->pid = -1;
	record->state = THROTTLE_STATE_NONE;
	record->last_error = THROTTLE_CONTROLLER_OK;
}

int throttle_controller_init(struct throttle_controller *controller,const struct throttle_controller_config *cfg)
{
	struct throttle_controller_config local_cfg;
	int rc;

	if (controller == NULL)
		return THROTTLE_CONTROLLER_ERR_INVALID_ARG;

	memset(controller,0,sizeof(*controller));

	if (cfg == NULL) {
		throttle_controller_config_default(&local_cfg);
		cfg = &local_cfg;
	}

	rc = validate_config(cfg);
	if (rc != THROTTLE_CONTROLLER_OK)
		return rc;

	controller->records = calloc(cfg->max_records,sizeof(*controller->records));
	if (controller->records == NULL)
		return THROTTLE_CONTROLLER_ERR_NO_MEMORY;

	controller->cfg = *cfg;
	controller->record_cap = cfg->max_records;

	   /* !dry_run == real mode */
	if (!cfg->dry_run && cfg->configure_throttled_cpu_max) {
		if (cgroup_set_cpu_max(cfg->throttled_group,cfg->throttled_cpu_max) != 0) {
			free(controller->records);
			memset(controller,0,sizeof(*controller));
			return THROTTLE_CONTROLLER_ERR_CGROUP;
		}
	}

	if (!cfg->dry_run && cfg->configure_protected_memory_low) {
		if (cgroup_set_memory_low(cfg->protected_group,cfg->protected_memory_low) != 0) {
			free(controller->records);
			memset(controller,0,sizeof(*controller));
			return THROTTLE_CONTROLLER_ERR_CGROUP;
		}
	}

	controller->initialized = true;
	return THROTTLE_CONTROLLER_OK;
}

void throttle_controller_destroy(struct throttle_controller *controller)
{
	if (controller == NULL)
		return;

	free(controller->records);
	memset(controller,0,sizeof(*controller));
}

int throttle_controller_apply_candidates(struct throttle_controller *controller, const struct interference_candidate *candidates, size_t candidate_count, struct throttle_controller_stats *stats)
{
	uint64_t now_ns;
	size_t i;
	int rc;
	int overall_rc = THROTTLE_CONTROLLER_OK;

	if (stats != NULL)
		throttle_controller_stats_reset(stats);

	if (controller == NULL || !controller->initialized)
		return THROTTLE_CONTROLLER_ERR_NOT_INITIALIZED;

	rc = prevalidate_candidates(controller,candidates,candidate_count);
	if (rc != THROTTLE_CONTROLLER_OK) {
		set_stats_error(stats,rc,"candidate validation failed");
		return rc;
	}

	rc = controller_now_ns(controller,&now_ns);
	if (rc != THROTTLE_CONTROLLER_OK) {
		set_stats_error(stats,rc,"failed to read monotonic time");
		return rc;
	}

	if (stats != NULL)
		stats->candidate_count = candidate_count;

	for (i = 0U; i < controller->record_count; i++)
		controller->records[i].seen_in_last_apply = false;

	for (i = 0U; i < candidate_count; i++) {   /* 후보에 있는 경우 */
		const struct interference_candidate *candidate = &candidates[i];
		struct throttle_record *record;
		bool request_throttle;

		record = find_record(controller,candidate->pid);
		if (record == NULL)
			record = create_record(controller,candidate->pid, now_ns);

		if (record == NULL) {
			set_stats_error(stats, THROTTLE_CONTROLLER_ERR_NO_SPACE,"record capacity exhausted");
			return THROTTLE_CONTROLLER_ERR_NO_SPACE;
		}

		record->seen_in_last_apply = true;
		record->last_seen_ns = now_ns;
		record->last_activity_score = candidate->activity_score;
		record->last_overlap_score = candidate->overlap_score;
		record->last_final_score = candidate->final_score;
		record->last_common_pages = candidate->common_pages;
		record->last_should_throttle = candidate->should_throttle;
		record->last_error = THROTTLE_CONTROLLER_OK;

		if (stats != NULL)
			stats->records_seen++;

		request_throttle = candidate_requests_throttle(controller,candidate);

		switch(record->state) {
			case THROTTLE_STATE_NONE:		
				if (request_throttle) {		/* NONE -> THROTTLED */
					rc = throttle_record_pid(controller,record,now_ns,stats);
					if (rc != THROTTLE_CONTROLLER_OK)
						overall_rc = rc;
				}
				else {				/* NONE -> NONE */
					if (stats != NULL)
						stats->skipped_low_score++;
					(void)snprintf(record->reason,sizeof(record->reason),"not throttled: final=%.4f high=%.4f active=%s detector=%s",
												             record->last_final_score,
													     controller->cfg.high_score_threshold,
													     candidate->active ? "true" : "false",
													     candidate->should_throttle ? "true" : "false");
				}
				break;

			case THROTTLE_STATE_THROTTLED:		
				if ((record->last_final_score <= controller->cfg.low_score_threshold) && elapsed_at_least(now_ns,record->state_entered_ns,controller->cfg.min_throttle_duration_ms)) {
					rc = release_record_pid(controller,record,now_ns,false,stats);	/* THROTTLED -> COOLDOWN */
					if (rc != THROTTLE_CONTROLLER_OK)
						overall_rc = rc;
				}
				else {
					if (stats != NULL)		/* THROTTLED -> THROTTLED */
						stats->kept_throttled++;
					(void)snprintf(record->reason,sizeof(record->reason),"kept throttled: final=%.4f low=%.4f min_done=%s",
											      record->last_final_score,
											      controller->cfg.low_score_threshold,
											      elapsed_at_least(now_ns,record->state_entered_ns,
												      controller->cfg.min_throttle_duration_ms) ? "true" : "false");
				}
				break;

			case THROTTLE_STATE_COOLDOWN:
				if (!elapsed_at_least(now_ns,record->state_entered_ns,controller->cfg.cooldown_duration_ms)) {	/* COOLDOWN -> COOLDOWN */
					if (stats != NULL)
						stats->skipped_cooldown++;
					set_reason(record->reason,sizeof(record->reason),"cooldown active");
					break;
				}

				record->state = THROTTLE_STATE_NONE;	/* COOLDOWN -> NONE */
				record->state_entered_ns = now_ns;

				if (request_throttle) {			/* COOLDOWN -> THROTTLED */
					rc = throttle_record_pid(controller,record,now_ns,stats);
					if (rc != THROTTLE_CONTROLLER_OK)
						overall_rc = rc;
				}
				else {
					if (stats != NULL)
						stats->skipped_low_score++;
					set_reason(record->reason,sizeof(record->reason),"cooldown complete: remains normal");
				}
				break;

			default:
				record->last_error = THROTTLE_CONTROLLER_ERR_INVALID_ARG;
				set_reason(record->reason,sizeof(record->reason),"invalid internal throttle state");
				set_stats_error(stats, THROTTLE_CONTROLLER_ERR_INVALID_ARG,record->reason);
				overall_rc = THROTTLE_CONTROLLER_ERR_INVALID_ARG;
				break;
		}
	}

	for (i = 0U; i < controller->record_count; i++) {	/* 후보에 없는 경우 */
		struct throttle_record *record = &controller->records[i];

		if (record->seen_in_last_apply)
			continue;

		if (record->state == THROTTLE_STATE_THROTTLED) { /* candidate목록에는 없지만 아직 THROTTLED STATE인 경우 */
			bool stale_done = elapsed_at_least(now_ns,record->last_seen_ns,controller->cfg.stale_after_ms);
			bool min_done = elapsed_at_least(now_ns,record->state_entered_ns,controller->cfg.min_throttle_duration_ms);

			if (stale_done && min_done) {		/* stale_after_ms (프로세스가 오래동안 반응이 없고) min_throttle_duration 지난경우 제한 풀기 */
				rc = release_record_pid(controller,record,now_ns,true,stats);
				if (rc != THROTTLE_CONTROLLER_OK)
					overall_rc = rc;
			}
			else {
				if (stats != NULL)
					stats->kept_throttled++;

				(void)snprintf(record->reason,sizeof(record->reason),"unseen but kept: stale_done=%s min_done=%s",
										      stale_done ? "true" : "false",
										      min_done ? "true" : "false");
			}
		}
		else if (record->state == THROTTLE_STATE_COOLDOWN && elapsed_at_least(now_ns,record->state_entered_ns,controller->cfg.cooldown_duration_ms)) {
			record->state = THROTTLE_STATE_NONE;
			record->state_entered_ns = now_ns;
			set_reason(record->reason,sizeof(record->reason),"cooldown complete: normal");
		}
	}

	return overall_rc;
}

int throttle_controller_release_all(struct throttle_controller *controller,struct throttle_controller_stats *stats)
{
	uint64_t now_ns;
	size_t i;
	int rc;
	int overall_rc = THROTTLE_CONTROLLER_OK;

	if (stats != NULL)
		throttle_controller_stats_reset(stats);

	if (controller == NULL || !controller->initialized)
		return THROTTLE_CONTROLLER_ERR_NOT_INITIALIZED;

	rc = controller_now_ns(controller,&now_ns);
	if (rc != THROTTLE_CONTROLLER_OK) {
		set_stats_error(stats,rc,"failed to read monotonic time");
		return rc;
	}

	for (i = 0U; i < controller->record_count; i++) {
		struct throttle_record *record = &controller->records[i];

		if (record->state != THROTTLE_STATE_THROTTLED)
			continue;

		rc = release_record_pid(controller,record,now_ns,false,stats);
		if (rc != THROTTLE_CONTROLLER_OK)
			overall_rc = rc;
	}

	return overall_rc;
}

bool throttle_controller_get_record(const struct throttle_controller *controller,pid_t pid,struct throttle_record *out)
{
	const struct throttle_record *record;

	if (controller == NULL || !controller->initialized || pid <= 0 || out == NULL)
		return false;

	record = find_record_const(controller,pid);
	if (record == NULL)
		return false;

	*out = *record;
	return true;
}

const char *throttle_state_str(enum throttle_state state)
{
	switch(state) {
		case THROTTLE_STATE_NONE:
			return "none";
		case THROTTLE_STATE_THROTTLED:
			return "throttled";
		case THROTTLE_STATE_COOLDOWN:
			return "cooldown";
		default:
			return "invalid";
	}
}

const char *throttle_controller_strerror(int rc)
{
    switch (rc) {
    case THROTTLE_CONTROLLER_OK:
        return "success";
    case THROTTLE_CONTROLLER_ERR_INVALID_ARG:
        return "invalid argument";
    case THROTTLE_CONTROLLER_ERR_NO_MEMORY:
        return "out of memory";
    case THROTTLE_CONTROLLER_ERR_NO_SPACE:
        return "record capacity exhausted";
    case THROTTLE_CONTROLLER_ERR_TIME:
        return "failed to read monotonic time";
    case THROTTLE_CONTROLLER_ERR_CGROUP:
        return "cgroup operation failed";
    case THROTTLE_CONTROLLER_ERR_NOT_INITIALIZED:
        return "controller is not initialized";
    default:
        return "unknown throttle-controller error";
    }
}




