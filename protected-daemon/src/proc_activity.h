#ifndef PROC_ACTIVITY_H
#define PROC_ACTIVITY_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define PROC_ACTIVITY_COMM_MAX 64U
#define PROC_ACTIVITY_REASON_MAX 128U

enum proc_activity_rc {
	PROC_ACTIVITY_OK = 0,
	PROC_ACTIVITY_ERR_INVALID_ARG = -1, 		// NULL or Invalid argument
	PROC_ACTIVITY_ERR_OPEN = -2,	    		// failed to open /proc/[pid]/stat
	PROC_ACTIVITY_ERR_READ = -3,	    		// failed to read /proc/[pid]/stat
	PROC_ACTIVITY_ERR_PARSE = -4,	    		// failed to parse /proc/[pid]/stat	
	PROC_ACTIVITY_ERR_SYSCONF = -5,     		// failed to gain page size of time
	PROC_ACTIVITY_ERR_PID_REUSED = -6,  		// same pid but process changed
	PROC_ACTIVITY_ERR_NON_MONOTONIC_TIME = -7	// new time stamp <= old time stamp
};

/*
 *
 * To Judge active
 */

struct proc_activity_config {
	double active_cpu_ratio_threshold;		// To see this active when it use cpu more than threshold
	double active_faults_per_sec_threshold;		// To see this active when it's fault rate increase than threshold per sec
	uint64_t min_rss_pages;				// To exclude on candidate when process rss is too small

	double major_fault_weight;			// weight to see major fault more heavier than minor fault

	double cpu_ratio_full_scale;			// To normalize cpu score 0 - 1
	double faults_per_sec_full_scale;		// To normalize fault score 0 - 1
	double rss_mib_full_scale;			// To normalize rss score 0 - 1
};

/*
 * /proc/pid/stat에서 읽어온 한 시점의 상태
 */

struct proc_activity_sample {
	pid_t pid;				// pid
	char comm[PROC_ACTIVITY_COMM_MAX];	// name of process
	char state;				// process states like R,S,D,Z

	bool valid;				// sample이 정상적으로 채워 졌는지

	uint64_t timestamp_ns;			// 이 sample을 뜬 시간 CLOCK_MONOTONIC 기준
	long clock_ticks_per_sec;		// 1초당 tick수 (_SC_CLK_TCK)
	uint64_t page_size;

	uint64_t minflt;			// minor fault 누적값
	uint64_t majflt;			// major fault 누적값

	uint64_t utime_ticks;			// user mode cpu time 누적 tick
	uint64_t stime_ticks;			// kernel mode cpu time 누적 tick

	uint64_t starttime_ticks;		// 프로세스 시작 시간 pid reused 확인용
	uint64_t vsize_bytes;			// virtual memory size
	int64_t rss_pages;			// 실제 ram에 올라온 page 수 
	int64_t num_threads;			// thread 수
};

/*
 * compare old sample and new sample
 */

struct proc_activity_delta {
	pid_t pid;

	bool valid;
	bool active;
	bool pid_reused;

	double elapsed_sec;		// old 와 new사이 시간차이

	uint64_t cpu_ticks_delta;	// utime + stime 증가량
	double cpu_seconds;		// cpu tick 증가량을 초로 변환한 값
	double cpu_ratio;		// elapse time 대비 cpu 사용 비율

	uint64_t minflt_delta;		// fault 증가량
	uint64_t majflt_delta;		// ""
	double weighted_faults;		// minor fault + major_fault_weight * major_fault
	double faults_per_sec;		// 초당 weighted fault 증가량

	double rss_mib;			// 현재 rss를 mib로 환산

	double cpu_score;		// 0-1 정규화 점수
	double fault_score;		// ""
	double rss_score;		// ""
	double total_score;		// activity 종합 점수

	char reason[PROC_ACTIVITY_REASON_MAX];  //최종적으로 active 후보인지(candidate)
};

void proc_activity_config_default(struct proc_activity_config *cfg);

void proc_activity_sample_reset(struct proc_activity_sample *sample);

void proc_activity_delta_reset(struct proc_activity_delta *delta);

int proc_activity_sample_now(pid_t pid, struct proc_activity_sample *out);

int proc_activity_compute_delta(const struct proc_activity_sample *old_sample, const struct proc_activity_sample *new_sample,
	       			const struct proc_activity_config *cfg, struct proc_activity_delta *out);

const char *proc_activity_strerror(int rc);

#endif



	

