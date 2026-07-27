#define _POSIX_C_SOURCE 200809L

#include "proc_activity.h"

#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define PROC_ACTIVITY_NSEC_PER_SEC 1000000000ULL
#define PROC_ACTIVITY_STAT_VALS_MAX 64U
#define PROC_ACTIVITY_STAT_LINE_MAX 4096U

static int get_time_ns(uint64_t *out_ns)
{
	struct timespec ts;

	if (out_ns == NULL)
		return PROC_ACTIVITY_ERR_INVALID_ARG;

	if (clock_gettime(CLOCK_MONOTONIC,&ts) != 0)
		return PROC_ACTIVITY_ERR_READ;
	
	*out_ns =((uint64_t)ts.tv_sec * PROC_ACTIVITY_NSEC_PER_SEC) +(uint64_t)ts.tv_nsec;
	
	return PROC_ACTIVITY_OK;
}
static double clamp01(double value)
{
	if (value < 0.0) return 0.0;
	if (value > 1.0) return 1.0;

	return value;  
}
static char *skip_spaces(char *p)
{
	while (*p == ' ') p++;

	return p;
}
static int parse_stat_values(char *p,long long *vals,size_t vals_cap, size_t *count_out)
{
	size_t count = 0U;

	if (p == NULL || vals == NULL || count_out == NULL)
		return PROC_ACTIVITY_ERR_INVALID_ARG;

	p = skip_spaces(p);

	while (*p != '\0' && *p != '\n') {
		char *endptr = NULL;
		long long value;

		if (count >= vals_cap)
			break;

		errno = 0;
		value = strtoll(p,&endptr,10);

		if (p == endptr)
			break;

		if (errno == ERANGE)
			return PROC_ACTIVITY_ERR_PARSE;

		vals[count] = value;
		count++;

		p = skip_spaces(endptr);
	}

	*count_out = count;

	return PROC_ACTIVITY_OK;
}

static int checked_u64(long long value, uint64_t *out)
{
	if (out == NULL)
		return PROC_ACTIVITY_ERR_INVALID_ARG;

	if (value < 0)
		return PROC_ACTIVITY_ERR_PARSE;

	*out = (uint64_t)value;

	return PROC_ACTIVITY_OK;
}
void proc_activity_config_default(struct proc_activity_config *cfg)
{
	if (cfg == NULL)
		return;

	cfg->active_cpu_ratio_threshold = 0.02;
	cfg->active_faults_per_sec_threshold = 100.0;
	cfg->min_rss_pages = 256U;

	cfg->major_fault_weight = 8.0;

	cfg->cpu_ratio_full_scale = 0.25;
	cfg->faults_per_sec_full_scale = 1000.0;
	cfg->rss_mib_full_scale = 256.0;
}

void proc_activity_sample_reset(struct proc_activity_sample *sample)
{

	if (sample == NULL)
		return;

	memset(sample,0,sizeof(*sample));
	sample->valid = false;
}

void proc_activity_delta_reset(struct proc_activity_delta *delta)
{
	if (delta == NULL)
		return;

	memset(delta,0,sizeof(*delta));
	delta->valid = false;
	delta->active = false;
	delta->pid_reused = false;
}
/*
 * /proc/pid/stat
 * 1445538 (bank_profile_ove) S 1445537 1445538 1445538 34816 ...
 */
int proc_activity_sample_now(pid_t pid, struct proc_activity_sample *out)
{
	char stat_path[256];
	char line[PROC_ACTIVITY_STAT_LINE_MAX];
	FILE *stat_file;
	int n;
	long page_size;
	long clock_ticks;
	int rc;
	char *lparen;
	char *rparen;
	size_t comm_len;
	char *p;
	long long vals[PROC_ACTIVITY_STAT_VALS_MAX];
	size_t val_count = 0U;

	if (out == NULL)
		return PROC_ACTIVITY_ERR_INVALID_ARG;
	if (pid <= 0)
		return PROC_ACTIVITY_ERR_INVALID_ARG;

	proc_activity_sample_reset(out);

	page_size = sysconf(_SC_PAGESIZE);
	clock_ticks = sysconf(_SC_CLK_TCK);

	if (page_size <= 0 || clock_ticks <= 0)
		return PROC_ACTIVITY_ERR_SYSCONF;

	rc = get_time_ns(&out->timestamp_ns);
	if (rc != PROC_ACTIVITY_OK)
		return rc;
	
	n = snprintf(stat_path,sizeof(stat_path),"/proc/%ld/stat",(long)pid);
	if (n <= 0 || (size_t)n >= sizeof(stat_path))
		return PROC_ACTIVITY_ERR_INVALID_ARG;

	stat_file = fopen(stat_path,"r");
	if (stat_file == NULL)
		return PROC_ACTIVITY_ERR_OPEN;

	if (fgets(line,sizeof(line),stat_file) == NULL) {
		fclose(stat_file);
		return PROC_ACTIVITY_ERR_READ;
	}

	lparen = strchr(line,'(');
	rparen = strrchr(line,')');

	if (lparen == NULL || rparen == NULL || lparen >= rparen)
		return PROC_ACTIVITY_ERR_PARSE;

	comm_len = (size_t)(rparen - lparen - 1);
	if (comm_len >= sizeof(out->comm))
		comm_len = sizeof(out->comm) - 1U;
	
	memcpy(out->comm,lparen + 1,comm_len);
	out->comm[comm_len] = '\0';

	p = skip_spaces(rparen + 1);

	if (*p == '\0'|| *p == '\n') 
		return PROC_ACTIVITY_ERR_PARSE;
	out->state = *p;
	p++;

	/*
	 * state 뒤 숫자들은 field 4부터 시작
	 *
	 * vals[0]  = field 4  = ppid
	 * vals[6]  = field 10 = minflt
     	* vals[8]  = field 12 = majflt
     	* vals[10] = field 14 = utime
     	* vals[11] = field 15 = stime
     	* vals[16] = field 20 = num_threads
     	* vals[18] = field 22 = starttime
     	* vals[19] = field 23 = vsize
     	* vals[20] = field 24 = rss
     	*/
	rc = parse_stat_values(p,vals,21U,&val_count);
	if (rc != PROC_ACTIVITY_OK)
		return rc;

	if (val_count < 21U) 
		return PROC_ACTIVITY_ERR_PARSE;

	out->pid = pid;
	out->clock_ticks_per_sec = clock_ticks;
	out->page_size = (uint64_t)page_size;

	rc = checked_u64(vals[6], &out->minflt);
	if (rc != PROC_ACTIVITY_OK)
		return rc;

	rc = checked_u64(vals[8],&out->majflt);
	if (rc != PROC_ACTIVITY_OK)
		return rc;
	rc = checked_u64(vals[10],&out->utime_ticks);
	if (rc != PROC_ACTIVITY_OK)
		return rc;

	rc = checked_u64(vals[11],&out->stime_ticks);
	if (rc != PROC_ACTIVITY_OK)
		return rc;

	rc = checked_u64(vals[18],&out->starttime_ticks);
	if (rc != PROC_ACTIVITY_OK)
		return rc;

	rc = checked_u64(vals[19],&out->vsize_bytes);
	if (rc != PROC_ACTIVITY_OK)
		return rc;

	out->num_threads = (int64_t)vals[16];
	out->rss_pages = (int64_t)vals[20];
	out->valid = true;

	return PROC_ACTIVITY_OK;
}

int proc_activity_compute_delta(const struct proc_activity_sample *old_sample, const struct proc_activity_sample *new_sample, 
				const struct proc_activity_config *cfg, struct proc_activity_delta *out)
{
	uint64_t elapsed_ns;
	uint64_t old_cpu_ticks;
	uint64_t new_cpu_ticks;
	long clock_ticks;
	double rss_bytes;
	bool rss_large_enough;
	bool cpu_active;
	bool fault_active;
	bool state_active;

	if (old_sample == NULL || new_sample == NULL || cfg == NULL || out == NULL)
		return PROC_ACTIVITY_ERR_INVALID_ARG;

	proc_activity_delta_reset(out);

	if (!old_sample->valid || !new_sample->valid)
		return PROC_ACTIVITY_ERR_INVALID_ARG;

	if (old_sample->pid != new_sample->pid)
		return PROC_ACTIVITY_ERR_INVALID_ARG;

	out->pid = new_sample->pid;

	if (old_sample->starttime_ticks != new_sample->starttime_ticks) {
		out->pid_reused = true;
		snprintf(out->reason,sizeof(out->reason),"pid reused: starttime changed");
		return PROC_ACTIVITY_ERR_PID_REUSED;
	}

	if (old_sample->timestamp_ns >= new_sample->timestamp_ns)
		return PROC_ACTIVITY_ERR_NON_MONOTONIC_TIME;

	elapsed_ns = new_sample->timestamp_ns - old_sample->timestamp_ns;
	out->elapsed_sec = (double)elapsed_ns / (double)PROC_ACTIVITY_NSEC_PER_SEC;

	if (out->elapsed_sec <= 0.0)
		return PROC_ACTIVITY_ERR_NON_MONOTONIC_TIME;

	old_cpu_ticks = old_sample->utime_ticks + old_sample->stime_ticks;
	new_cpu_ticks = new_sample->utime_ticks + old_sample->stime_ticks;

	if (new_cpu_ticks < old_cpu_ticks)
		return PROC_ACTIVITY_ERR_PARSE;

	if (new_sample->minflt < old_sample->minflt || new_sample->majflt < old_sample->majflt)
		return PROC_ACTIVITY_ERR_PARSE;

	clock_ticks = new_sample->clock_ticks_per_sec;
	if (clock_ticks <= 0)
		clock_ticks = old_sample->clock_ticks_per_sec;

	if (clock_ticks <= 0)
		return PROC_ACTIVITY_ERR_SYSCONF;

	out->cpu_ticks_delta = new_cpu_ticks - old_cpu_ticks;
	out->cpu_seconds = (double)out->cpu_ticks_delta / (double)clock_ticks;
	out->cpu_ratio = out->cpu_seconds / out->elapsed_sec;

	out->minflt_delta = new_sample->minflt - old_sample->minflt;
	out->majflt_delta = new_sample->majflt - old_sample->majflt;

	out->weighted_faults = (double)out->minflt_delta + (cfg->major_fault_weight * (double)out->majflt_delta);

	out->faults_per_sec = out->weighted_faults / out->elapsed_sec;

	if (new_sample->rss_pages > 0 && new_sample->page_size > 0U) {
		rss_bytes = (double)new_sample->rss_pages * (double)new_sample->page_size;
		out->rss_mib = rss_bytes / (1024.0 * 1024.0);
	}
	else {
		out->rss_mib = 0.0;
	}

	out->cpu_score = clamp01(out->cpu_ratio / cfg->cpu_ratio_full_scale);

	out->fault_score = clamp01(out->faults_per_sec / cfg->faults_per_sec_full_scale);

	out->rss_score = clamp01(out->rss_mib / cfg->rss_mib_full_scale);

	out->total_score = (0.6 * out->cpu_score) + (0.3 * out->fault_score) + (0.1 * out->rss_score);

	rss_large_enough = new_sample->rss_pages >= (int64_t)cfg->min_rss_pages;

	cpu_active = out->cpu_ratio >= cfg->active_cpu_ratio_threshold;

	fault_active = out->faults_per_sec >= cfg->active_faults_per_sec_threshold;

	state_active = new_sample->state == 'R' || new_sample->state == 'D';

	if (new_sample->state == 'Z') {
		out->active = false;
		snprintf(out->reason,sizeof(out->reason),"inactive: zombie state=Z rss=%.2fMiB",out->rss_mib);
	}
	else if (!rss_large_enough) {
		out->active = false;
		snprintf(out->reason,sizeof(out->reason),"inactive: rss too small rss_pages=%lld threshold=%llu",(long long)new_sample->rss_pages,(unsigned long long)cfg->min_rss_pages);
	}
	else if (cpu_active || fault_active || state_active) {
		out->active = true;
		snprintf(out->reason,sizeof(out->reason),"active: cpu=%.4f faults/s=%.2f rss=%.2fMiB state=%c",out->cpu_ratio,out->faults_per_sec,out->rss_mib,new_sample->state);
	}
	else {
		out->active = false;
		snprintf(out->reason,sizeof(out->reason),"inactive: below threshold cpu=%.4f faults/s=%.2f rss=%.2fMiB state=%c",out->cpu_ratio,out->faults_per_sec,out->rss_mib,new_sample->state);
	}

	out->valid = true;

	return PROC_ACTIVITY_OK;
}

const char *proc_activity_strerror(int rc)
{
	switch (rc) {
	    case PROC_ACTIVITY_OK:
        	return "ok";
	    case PROC_ACTIVITY_ERR_INVALID_ARG:
	        return "invalid argument";
	    case PROC_ACTIVITY_ERR_OPEN:
	        return "failed to open proc stat";
	    case PROC_ACTIVITY_ERR_READ:
	        return "failed to read proc stat or clock";
	    case PROC_ACTIVITY_ERR_PARSE:
	        return "failed to parse proc stat";
	    case PROC_ACTIVITY_ERR_SYSCONF:
	        return "failed to read system configuration";
	    case PROC_ACTIVITY_ERR_PID_REUSED:
	        return "pid reused";
	    case PROC_ACTIVITY_ERR_NON_MONOTONIC_TIME:
	        return "non-monotonic timestamp";
	    default:
	        return "unknown proc_activity error";
    }
}

