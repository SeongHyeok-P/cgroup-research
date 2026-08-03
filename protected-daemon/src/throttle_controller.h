#ifndef THROTTLE_CONTROLLER_H
#define THROTTLE_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "cgroup.h"
#include "interference_detector.h"

#define THROTTLE_CONTROLLER_VALUE_MAX 64U
#define THROTTLE_CONTROLLER_REASON_MAX 256U

enum throttle_controller_rc {
	THROTTLE_CONTROLLER_OK = 0,
	THROTTLE_CONTROLLER_ERR_INVALID_ARG = -1,  /* NULL arg,pid <= 0, out of 0 ~ 1 score, high_thr < low_thr, same pid same candidate */
	THROTTLE_CONTROLLER_ERR_NO_MEMORY = -2, /* failed to calloc records arr */
	THROTTLE_CONTROLLER_ERR_NO_SPACE = -3,  /* out of max_records */
	THROTTLE_CONTROLLER_ERR_TIME = -4,
	THROTTLE_CONTROLLER_ERR_CGROUP = -5,	/* failed cgroup_move_pid(), cgroup_set_cpu_max(), cgroup_set_mem_low() */
	THROTTLE_CONTROLLER_ERR_NOT_INITIALIZED = -6
};

enum throttle_state {
	THROTTLE_STATE_NONE = 0,	/* GROUP_BACKGROUND */
	THROTTLE_STATE_THROTTLED = 1,  /* cgroup_move_pid(pid,controller->cfg.throttled_group); */
	THROTTLE_STATE_COOLDOWN = 2	/* take time after move to BACKGROUND */
};

/*
 * Optional clock injection for deterministic unit tests.
 * Return THROTTLE_CONTROLLER_OK and store a CLOCK_MONOTONIC-like timestamp.
 * in *now_ns. Production code normally leaves clock_fn == NULL.
 */
typedef int (*throttle_controller_clock_fn)(void *clock_ctx,uint64_t *now_ns);

struct throttle_controller_config {
	enum proc_group normal_group;
	enum proc_group throttled_group;
	enum proc_group protected_group;

	char throttled_cpu_max[THROTTLE_CONTROLLER_VALUE_MAX]; /* cpu.max string ex) 20000 100000 */
	char protected_memory_low[THROTTLE_CONTROLLER_VALUE_MAX]; /* default 0 */

	double high_score_threshold; /* NONE -> THROTTLED */
	double low_score_threshold;  /* THROTTLED -> COOLDOWN */

	uint64_t min_throttle_duration_ms; /* min time to maintain throttled */
	uint64_t cooldown_duration_ms;	   /* min time to maintain released */
	uint64_t stale_after_ms; /* disappear in candidate AND stale_after_ms passes  AND min_throttle_duration_ms passes -> BACKGROUND */

	size_t max_records; /* max count pid that controller track */

	bool dry_run;	/* do not move_pid() just renew controller record, stats */
	bool configure_throttled_cpu_max; /*determine whether configure cpu max after init*/
	bool configure_protected_memory_low; 

	throttle_controller_clock_fn clock_fn; /* to test min duration and cool down duration */
	void *clock_ctx; /* in daemon it is NULL */
};

struct throttle_record {
	pid_t pid;	/* pid that this record track */
	enum throttle_state state; /* controller's state */

	uint64_t first_seen_ns; /* first seen time that controller detected in candidate; for debug */
	uint64_t last_seen_ns;	/* for judge stale release */
	uint64_t state_entered_ns; /* THROTTLED -> calc min duration , COOLDOWN -> calc cooldown duration */
	uint64_t last_action_ns;

	double last_activity_score; /* detector에게 전달받은 가장 최근 activity score */
	double last_overlap_score;  
	double last_final_score;
	uint64_t last_common_pages; /* for test ex) final_score이 높아도 common page작으면 제외 */

	bool seen_in_last_apply; /* 일단 전부 false candidate에 등장한 record만 true 마지막에 false인 record를 stale후보로*/
	bool last_should_throttle;

	int last_error;
	char reason[THROTTLE_CONTROLLER_REASON_MAX];
};

struct throttle_controller {
	bool initialized;
	struct throttle_controller_config cfg; /* config 복사본 */

	struct throttle_record *records; /* record arr */
	size_t record_count;	
	size_t record_cap;	/* max record */
};

struct throttle_controller_stats {
	size_t candidate_count;
	size_t records_seen; 	/* 실제 조회 or 생성 pid record 수 */
	size_t throttle_actions; /* THROTTLED 성공 상태 수 */
	size_t release_actions; 
	size_t kept_throttled;
	size_t skipped_low_score; /* 진입 조건 충족 x -> throttle x수 */
	size_t skipped_cooldown; /* 점수 높지만 cooldown중이라 throttled x수 */
	size_t stale_releases;  /* 후보에서 오래 사라져 stale조건으로 release한 수 */
	size_t errors;

	int last_error;
	char last_error_reason[THROTTLE_CONTROLLER_REASON_MAX];
};

/* 
 * 기본 설정
 * ex) threshold , duration_ms
 */
void throttle_controller_config_default(struct throttle_controller_config *cfg);

/*
 * 통계 구조체를 0으로 초기화
 */
void throttle_controller_stats_reset(struct throttle_controller_stats *stats);

/*
 * pid record init
 */
void throttle_controller_record_reset(struct throttle_record *record);

/*
 * init controller
 */
int throttle_controller_init(struct throttle_controller *controller, const struct throttle_controller_config *cfg);

/*
 * destroy record arr , init controller to 0
 * This function do not revert pid to background
 */
void throttle_controller_destroy(struct throttle_controller *controller);

/*
 * validate candidates -> read clock_ns -> all record seen=false 
 * -> see or create candidate pid records -> store last detector's value
 * -> processing throttle_state -> candidate에 없던 throttled recored stale 검사
 * -> transpos finished cooldown to none 
 */
int throttle_controller_apply_candidates(struct throttle_controller *controller,
					 const struct interference_candidate *candidates,
					 size_t candidate_count,
					 struct throttle_controller_stats *stats);
/*
 * for daemon error of rollback
 */
int throttle_controller_release_all(struct throttle_controller *controller,
				    struct throttle_controller_stats *stats);
/*
 * retrieve pid record
 * controller does not change even caller change record
 * do not return pointer return copy
 */
bool throttle_controller_get_record(const struct throttle_controller *controller,pid_t pid,
				    struct throttle_record *out);
/*
 * return state str
 */
const char *throttle_state_str(enum throttle_state state);
const char *throttle_controller_strerror(int rc);

#endif /* THROTTLE_CONTROLLER_H */
