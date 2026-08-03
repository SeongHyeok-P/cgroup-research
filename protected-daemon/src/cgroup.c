#define _POSIX_C_SOURCE 200809L

#include "cgroup.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static char g_cgroup_base[CGROUP_PATH_MAX] = CGROUP_DEFAULT_BASE;

static const char *group_to_dir_name(enum proc_group group)
{
	switch(group) {
		case GROUP_PROTECTED:
			return "protected";
		case GROUP_BACKGROUND:
			return "background";
		case GROUP_THROTTLED:
			return "throttled";
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
		case GROUP_THROTTLED:
			return "throttled";
		case GROUP_IGNORED:
			return "ignored";
		case GROUP_UNKNOWN:
			return "unknown";
		default:
			return "invalid";
	}
}

int cgroup_set_base_path(const char *base_path)
{
	int written;

	if (base_path == NULL || base_path[0] == '\0')
		return -1;

	written = snprintf(g_cgroup_base,sizeof(g_cgroup_base),"%s",base_path);
	if (written < 0 || (size_t)written >= sizeof(g_cgroup_base))
		return -1;

	return 0;
}

const char *cgroup_get_base_path(void)
{
	return g_cgroup_base;
}

int cgroup_group_dir(enum proc_group group, char *out, size_t out_size)
{
	const char *dir_name;
	int written;

	if (out == NULL || out_size == 0U)
		return -1;

	dir_name = group_to_dir_name(group);
	if (dir_name == NULL)
		return -1;

	written = snprintf(out,out_size,"%s/%s",g_cgroup_base,dir_name);
	if (written < 0 || (size_t)written >= out_size)
		return -1;

	return 0;
}

int cgroup_control_path(enum proc_group group,const char *control_file, char *out, size_t out_size)
{
	char dir[CGROUP_PATH_MAX];
	int written;

	if (control_file == NULL || control_file[0] == '\0')
		return -1;

	if (strchr(control_file,'/') != NULL)
		return -1;

	if (cgroup_group_dir(group,dir,sizeof(dir)) != 0)
		return -1;

	written = snprintf(out,out_size,"%s/%s",dir,control_file);
	if (written < 0 || (size_t)written >= out_size)
		return -1;

	return 0;
}

int cgroup_write_control(enum proc_group group,const char *control_file,const char *value)
{
	char path[CGROUP_PATH_MAX];
	FILE *f;

	if (value == NULL)
		return -1;

	if (cgroup_control_path(group,control_file,path,sizeof(path)) != 0) {
		fprintf(stderr,"[CGROUP] invalid control path: group=%s file=%s\n",cgroup_group_name(group),control_file != NULL ? control_file : "(null)");
		return -1;
	}

	f = fopen(path,"w");
	if (f == NULL) {
		fprintf(stderr, "[CGROUP] failed to open %s: %s\n",path,strerror(errno));
		return -1;
	}

	if (fprintf(f,"%s\n",value) < 0) {
		fprintf(stderr,"[CGROUP] failed to write %s to %s: %s\n",value,path,strerror(errno));
		fclose(f);
		return -1;
	}

	if (fclose(f) != 0) {
		fprintf(stderr,"[CGROUP] failed to close %s: %s\n",path,strerror(errno));
		return -1;
	}

	return 0;
}

int cgroup_move_pid(pid_t pid, enum proc_group group)
{
	char pid_text[CGROUP_VALUE_MAX];
	int written;

	if (pid <= 0)
		return -1;

	written = snprintf(pid_text,sizeof(pid_text),"%ld",(long)pid);
	if (written < 0 || (size_t)written >= sizeof(pid_text)) 
		return -1;

	if (cgroup_write_control(group,"cgroup.procs",pid_text) != 0) {
		fprintf(stderr,"[CGROUP] failed to move pid=%ld to %s\n",(long)pid,cgroup_group_name(group));
		return -1;
	}

	printf("[CGROUP] moved pid=%ld to %s\n",(long)pid,cgroup_group_name(group));

	return 0;
}

int cgroup_set_cpu_max(enum proc_group group,const char *value)
{
	return cgroup_write_control(group, "cpu.max",value);
}

int cgroup_set_memory_low(enum proc_group group, const char *value)
{
	return cgroup_write_control(group,"memory.low",value);
}

int cgroup_set_memory_high(enum proc_group group, const char *value)
{
	return cgroup_write_control(group,"memory.high",value);
}

int cgroup_set_memory_max(enum proc_group group, const char *value)
{
	return cgroup_write_control(group,"memory.max",value);
}


