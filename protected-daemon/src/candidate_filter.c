#define _POSIX_C_SOURCE 200809L

#include "candidate_filter.h"

#include <stdio.h>
#include <string.h>

#define CGROUP_LINE_MAX 4096U
#define CGROUP_PATH_MAX_LOCAL 4096U

static void set_result(struct candidate_filter_result *result,enum candidate_filter_kind kind,
		       enum candidate_filter_reason reason)
{
	result->kind = kind;
	result->reason = reason;
}

static int process_record_is_valid(const struct proc_info *p)
{
	if (p == NULL)
		return 0;

	if (!p->used || p->pid <= 0)
		return 0;

	return 1;
}

static int process_record_is_protected(const struct proc_info *p)
{
	if (p == NULL)
		return 0;

	if (p->is_protected || p->inherited_protected)
		return 1;

	if (p->group == GROUP_PROTECTED)
		return 1;

	return 0;
}

static int read_cgroup_v2_path(pid_t pid, char *out,size_t out_size)
{
	char proc_path[64];
	char line[CGROUP_LINE_MAX];
	FILE *f;
	int written;

	if (pid <= 0 || out == NULL || out_size == 0U)
		return -1;

	out[0] = '\0';

	written = snprintf(proc_path,sizeof(proc_path),"/proc/%ld/cgroup",(long)pid);
	if (written <= 0 || (size_t)written >= sizeof(proc_path))
		return -1;

	f = fopen(proc_path,"r");
	if (f == NULL)
		return -1;

	while (fgets(line,sizeof(line),f) != NULL) {
		const char prefix[] = "0::";
		const char *path;
		size_t len;

		if (strncmp(line,prefix,sizeof(prefix) - 1U) != 0)
			continue;

		path = line + (sizeof(prefix) - 1U);
		len = strcspn(path,"\r\n");

		if (len == 0U || len >= out_size) {
			(void)fclose(f);
			return -1;
		}

		memcpy(out,path,len);
		out[len] = '\0';

		if (fclose(f) != 0) {
			out[0] = '\0';
			return -1;
		}

		return 0;
	}

	(void)fclose(f);
	return -1;
}

bool candidate_filter_path_is_system_slice(const char *cgroup_v2_path)
{
	static const char system_slice[] = "/system.slice";
	size_t prefix_len;

	if (cgroup_v2_path == NULL)
		return false;

	prefix_len = sizeof(system_slice) - 1U;

	if (strncmp(cgroup_v2_path,system_slice,prefix_len) != 0)
		return false;

	return cgroup_v2_path[prefix_len] == '\0' || cgroup_v2_path[prefix_len] == '/';
}
int candidate_filter_classify_path(const struct proc_info *p,pid_t daemon_pid,const char *cgroup_v2_path,
				   struct candidate_filter_result *result)
{
	if (result == NULL)
		return CANDIDATE_FILTER_ERR_INVALID_ARG;

	set_result(result,CANDIDATE_FILTER_EXCLUDED,CANDIDATE_FILTER_REASON_INVALID_PROCESS);

	if (!process_record_is_valid(p))
		return CANDIDATE_FILTER_OK;

	/* 
	 * The daemon must never become its own detector/control target,
	 * even if its comm accidentally matches a protected rule.
	 */
	if (daemon_pid > 0 && p->pid == daemon_pid) {
		set_result(result,CANDIDATE_FILTER_EXCLUDED,CANDIDATE_FILTER_REASON_DAEMON_SELF);
		return CANDIDATE_FILTER_OK;
	}

	/*
	 * Protected classification has priority over system.slice.
	 * A protected workload must still be monitored even if it is
	 * launched as a systemd system service.
	 */
	if (process_record_is_protected(p)) {
		        set_result(result, CANDIDATE_FILTER_PROTECTED, CANDIDATE_FILTER_REASON_NONE);
        	return CANDIDATE_FILTER_OK;
    	}

    	if (cgroup_v2_path == NULL || cgroup_v2_path[0] == '\0') {
        	set_result(result,CANDIDATE_FILTER_EXCLUDED, CANDIDATE_FILTER_REASON_CGROUP_UNAVAILABLE);
	        return CANDIDATE_FILTER_OK;
    	}

    	if (candidate_filter_path_is_system_slice(cgroup_v2_path)) {
        	set_result(result,CANDIDATE_FILTER_EXCLUDED,CANDIDATE_FILTER_REASON_SYSTEM_SLICE);
	        return CANDIDATE_FILTER_OK;
    	}

    	set_result(result, CANDIDATE_FILTER_BACKGROUND, CANDIDATE_FILTER_REASON_NONE);

	return CANDIDATE_FILTER_OK;
}
int candidate_filter_classify_pid(const struct proc_info *p,pid_t daemon_pid,struct candidate_filter_result *result)
{
	char cgroup_path[CGROUP_PATH_MAX_LOCAL];

	if (result == NULL)
		return CANDIDATE_FILTER_ERR_INVALID_ARG;

	set_result(result,CANDIDATE_FILTER_EXCLUDED,CANDIDATE_FILTER_REASON_INVALID_PROCESS);

	if (!process_record_is_valid(p))
		return CANDIDATE_FILTER_OK;

	if (daemon_pid > 0 && p->pid == daemon_pid) {
		set_result(result,CANDIDATE_FILTER_EXCLUDED,CANDIDATE_FILTER_REASON_DAEMON_SELF);
		return CANDIDATE_FILTER_OK;
	}

	/*
	 * Do this before touching /proc/<pid>/cgroup so protected tasks
	 * remain on the protected path even when they live in system.slice.
	 */
	if (process_record_is_protected(p)) {
		set_result(result,CANDIDATE_FILTER_PROTECTED,CANDIDATE_FILTER_REASON_NONE);
		return CANDIDATE_FILTER_OK;
	}

	if (read_cgroup_v2_path(p->pid,cgroup_path,sizeof(cgroup_path)) != 0) {
		/*
		 * This is expected when a process exits between snapshot
		 * creation and filtering. Do not fail the whole detector cycle.
		 */
		set_result(result,CANDIDATE_FILTER_EXCLUDED,CANDIDATE_FILTER_REASON_CGROUP_UNAVAILABLE);
		return CANDIDATE_FILTER_OK;
	}

	return candidate_filter_classify_path(p,daemon_pid,cgroup_path,result);
}

const char *candidate_filter_kind_name(enum candidate_filter_kind kind)
{
	switch (kind) {
		case CANDIDATE_FILTER_EXCLUDED:
			return "excluded";
		case CANDIDATE_FILTER_PROTECTED:
			return "protected";
		case CANDIDATE_FILTER_BACKGROUND:
			return "background";
		default:
			return "unknown";
	}
}

const char *candidate_filter_reason_name(enum candidate_filter_reason reason)
{
	switch (reason) {
		case CANDIDATE_FILTER_REASON_NONE:
			return "none";
		case CANDIDATE_FILTER_REASON_INVALID_PROCESS:
			return "invalid-process";
		case CANDIDATE_FILTER_REASON_DAEMON_SELF:
			return "daemon-self";
		case CANDIDATE_FILTER_REASON_SYSTEM_SLICE:
			return "system.slice";
		case CANDIDATE_FILTER_REASON_CGROUP_UNAVAILABLE:
			return "cgroup-unavailable";
		default:
			return "unknown";
	}
}

