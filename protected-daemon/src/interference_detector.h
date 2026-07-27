#ifndef INTERFERENCE_DETECTOR_H
#define INTERFERENCE_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "bank_profile.h"
#include "dram_mapping.h"
#include "proc_activity.h"

#define INTERFERENCE_DETECTOR_REASON_MAX 256U

#ifdef __cplusplus
extern "C" {
#endif

enum interference_detector_rc {
	INTERFERENCE_DETECTOR_OK = 0,
	INTERFERENCE_DETECTOR_ERR_INVALID_ARG = -1,
	INTERFERENCE_DETECTOR_ERR_NO_MAPPING = -2,
	INTERFERENCE_DETECTOR_ERR_NO_MEMORY = -3,
	INTERFERENCE_DETECTOR_ERR_NO_PROTECTED_PROFILE = -4,
	INTERFERENCE_DETECTOR_ERR_PROFILE = -5,
	INTERFERENCE_DETECTOR_ERR_ACTIVITY = -6
};

enum interference_process_role {
	INTERFERENCE_PROCESS_NORMAL = 0,
	INTERFERENCE_PROCESS_PROTECTED = 1
};

struct interference_process_state {
	pid_t pid;
	enum interference_process_role role;
	bool alive;

	bool has_activity_sample; //이전 proc_activity sample이 있는지
	struct proc_activity_sample activity_sample;  //이전 sample 저장소
};

struct interference_detector_config {
	size_t max_normal_candidates; //active normal 후보 중 bank_profile_build를 실제로 돌릴 최대 개수

	double activity_score_threshold; //이보다 낮은 activity score는 후보에서 제외
	double overlap_score_threshold;  //protected aggregate와 overlap이 이 이상이어야 throttle 후보
	double throttle_score_threshold;  //final score가 이 이상이어야 throttle 후보

	struct proc_activity_config activity_config; //proc_activity 계산 기준
	struct bank_profile_config profile_config;   //bank_profile_build sampling 기준
};

struct interference_candidate {
	pid_t pid;

	double activity_score;
	double cpu_score;
	double fault_score;
	double rss_score;

	double overlap_score;
	double final_score;     // activity_score * overlap_score
	uint64_t common_pages;  // protected 와 normal이 공통으로 겹친 bank_class page수

	bool active;
	bool should_throttle;

	char reason[INTERFERENCE_DETECTOR_REASON_MAX];
};

struct interference_detector_stats {
	size_t process_count;
	size_t protected_processes;
	size_t normal_processes;

	size_t protected_profiles_built;
	size_t activity_samples;
	size_t active_normal_candidates;
	size_t normal_profiles_built;
	size_t output_candidates;

	size_t activity_errors;
	size_t profile_errors;

	int last_error;
	char last_error_reason[INTERFERENCE_DETECTOR_REASON_MAX];
};

void interference_detector_config_default(struct interference_detector_config *cfg);

void interference_process_state_reset(struct interference_process_state *state);

void interference_candidate_reset(struct interference_candidate *candidate);

void interference_detector_stats_reset(struct interference_detector_stats *stats);

/*
 * This function is one interval about detector
 * daemon worker will call this
 */
int interference_detector_run_once(struct interference_process_state *processes,size_t process_count,const struct dram_mapping *mapping,
				   const struct interference_detector_config *cfg,struct interference_candidate *out, size_t out_cap,size_t *out_count,
				   struct interference_detector_stats *stats);

const char *interference_detector_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* INTERFERENCE_DETECTOR_H */
