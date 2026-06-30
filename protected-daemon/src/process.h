#ifndef PROCESS_H
#define PROCESS_H

#include<sys/types.h>
#include"protected_daemon.h"

#define PROC_MAX 32768
#define PROC_PATH_MAX 4096
#define CMDLINE_MAX 4096

enum proc_group {
	GROUP_UNKNOWN = 0,
	GROUP_PROTECTED,
	GROUP_BACKGROUND,
	GROUP_IGNORED,
};

struct proc_info {
	int used;
	pid_t pid;
	pid_t ppid;
	uid_t uid;

	int exec_seen;
	int exited;

	int is_protected;
	int inherited_protected;

	enum proc_group group;

	char comm[TASK_COMM_LEN];
	char exe_path[PROC_PATH_MAX];
	char cmdline[CMDLINE_MAX];
};

struct proc_info *process_get(pid_t pid);
struct proc_info *process_upsert_exec(const struct event *e);
struct proc_info *process_upsert_fork(const struct event *e);
void process_remove(pid_t pid);

int process_read_exe(struct proc_info *p);
int process_read_cmdline(struct proc_info *p);

#endif
