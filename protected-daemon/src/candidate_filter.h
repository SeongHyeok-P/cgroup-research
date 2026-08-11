#ifndef CANDIDATE_FILTER_H
#define CANDIDATE_FILTER_H

#include <stdbool.h>
#include <sys/types.h>

#include "process.h"

enum candidate_filter_status {
	CANDIDATE_FILTER_OK = 0,
	CANDIDATE_FILTER_ERR_INVALID_ARG = -1
};

enum candidate_filter_kind {
	CANDIDATE_FILTER_EXCLUDED = 0,
	CANDIDATE_FILTER_PROTECTED,
	CANDIDATE_FILTER_BACKGROUND
};

enum candidate_filter_reason {
	CANDIDATE_FILTER_REASON_NONE = 0,
	CANDIDATE_FILTER_REASON_INVALID_PROCESS,
	CANDIDATE_FILTER_REASON_DAEMON_SELF,
	CANDIDATE_FILTER_REASON_SYSTEM_SLICE,
	CANDIDATE_FILTER_REASON_CGROUP_UNAVAILABLE
};

struct candidate_filter_result {
	enum candidate_filter_kind kind;
	enum candidate_filter_reason reason;
};

/*
 * Returns true only for the system.slice subtree:
 *
 * /system.slice
 * /system.slice/example.service
 *
 * Similar names such as /system.slice2 are not matched.
 */
bool candidate_filter_path_is_system_slice(const char *cgroup_v2_path);

/*
 * Pure classification helper for a process whose cgroup v2 path is
 * already known.
 *
 * Priority:
 * 1. daemon/self -> excluded
 * 2. protected -> protected
 * 3. unavailable cgroup path -> excluded
 * 4. system.slice -> excluded
 * 5. otherwise -> background candidate
 */
int candidate_filter_classify_path(const struct proc_info *p, pid_t daemon_pid,const char *cgroup_v2_path,
				   struct candidate_filter_result *result);

/*
 * Production helper. Reads /proc/<pid>/cgroup, extracts the cgroup-v2
 * membership path ("0::$PATH"), then applies the same classification.
 *
 * A disappearing process / unreadable cgroup file is treated as a
 * non-fatal exclusion for the current detector cycle.
 */
int candidate_filter_classify_pid(const struct proc_info *p, pid_t daemon_pid, struct candidate_filter_result *result);

const char *candidate_filter_kind_name(enum candidate_filter_kind kind);
const char *candidate_filter_reason_name(enum candidate_filter_reason reason);

#endif
