#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>
#include <stdint.h>
#include<sys/types.h>

#include"protected_daemon.h"
#include "cgroup.h"

/* linux standard max pid count */
#define PROC_MAX 32768U
#define PROC_PATH_MAX 4096U
#define CMDLINE_MAX 4096U

enum process_result {
	PROCESS_OK = 0,
	PROCESS_ERR_INVALID_ARG = -1,
	PROCESS_ERR_NO_SPACE = -2,
	PROCESS_ERR_INCONSISTENT = -3
};

struct proc_info {
	int used;

	pid_t pid;
	pid_t ppid;
	uint32_t uid;

	int exec_seen;
	int exited;
	int is_protected;
	int inherited_protected;

	enum proc_group group;

	char comm[TASK_COMM_LEN];
	char exe_path[PROC_PATH_MAX];
	char cmdline[CMDLINE_MAX];
};

/* Clear every process-table entry and collision-management state */
void process_table_reset(void);

/* Number of currently occupied entries */
size_t process_table_count(void);

/* Fixed table capacity. This is PROC_MAX */
size_t process_table_capacity(void);

struct proc_info *process_get(pid_t pid);
struct proc_info *process_upsert_exec(const struct event *e);
struct proc_info *process_upsert_fork(const struct event *e);
void process_remove(pid_t pid);

int process_read_exe(struct proc_info *p);
int process_read_cmdline(struct proc_info *p);

int process_snapshot(struct proc_info *out, size_t capacity, size_t *out_count);
#endif
