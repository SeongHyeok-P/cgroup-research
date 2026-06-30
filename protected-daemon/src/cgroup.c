#include<stdio.h>
#include"cgroup.h"

#define CGROUP_BASE "/sys/fs/cgroup/cgroup_research"

static const char *group_to_path(enum proc_group group)
{
	switch(group) {
		case GROUP_PROTECTED:
			return CGROUP_BASE "/protected/cgroup.procs";
		case GROUP_BACKGROUND:
			return CGROUP_BASE "/background/cgroup.procs";
		default:
			return NULL;
	}
}

const char *cgroup_group_name(enum proc_group group)
{
	switch(group) {
		case GROUP_PROTECTED:
			return "protected";
		case GROUP_BACKGROUND:
			return "background";
		case GROUP_IGNORED:
			return "ignored";
		case GROUP_UNKNOWN:
			return "unknown";
		default:
			return "invalid";
	}
}

int cgroup_move_pid(pid_t pid, enum proc_group group)
{
	const char *path = group_to_path(group);
	FILE *f;

	if(!path)
		return -1;
	f = fopen(path,"w");
	if(!f) {
		fprintf(stderr, "[CGROUP] failed to open %s\n",path);
		return -1;
	}

	if(fprintf(f,"%d\n",pid) < 0) {
		fclose(f);
		fprintf(stderr, "[CGROUP] failed to move pid=%d to %s\n",pid,path);
		return -1;
	}
	fclose(f);
	printf("[CGROUP] moved pid=%d to %s\n",pid,cgroup_group_name(group));
	return 0;
}


