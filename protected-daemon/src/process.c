#include<stdio.h>
#include<string.h>
#include<unistd.h>
#include<stdlib.h>

#include"process.h"


static struct proc_info proc_table[PROC_MAX];

static int proc_index(pid_t pid)
{
	if(pid < 0)
		pid = -pid;
	return pid % PROC_MAX;
}

struct proc_info *process_get(pid_t pid)
{
	int idx = proc_index(pid);

	if(proc_table[idx].used && proc_table[idx].pid == pid)
		return &proc_table[idx];
	return NULL;
}

struct proc_info *process_upsert_exec(const struct event *e)
{
	int idx = proc_index(e->pid);
	struct proc_info *p = &proc_table[idx];

	memset(p,0,sizeof(*p));

	p->used = 1;
	p->pid = e->pid;
	p->ppid = e->ppid;
	p->uid = e->uid;
	p->exec_seen = 1;
	p->group = GROUP_UNKNOWN;

	snprintf(p->comm,sizeof(p->comm),"%s",e->comm);

	process_read_exe(p);
	process_read_cmdline(p);

	return p;
}

struct proc_info *process_upsert_fork(const struct event *e)
{
	int idx = proc_index(e->child_pid);
	struct proc_info *child = &proc_table[idx];
	struct proc_info *parent = process_get(e->pid);

	memset(child,0,sizeof(*child));

	child->used = 1;
	child->pid = e->child_pid;
	child->ppid = e->pid;
	child->uid = e->uid;
	child->group = GROUP_UNKNOWN;

	snprintf(child->comm, sizeof(child->comm),"%s",e->comm);

	if(parent && parent->used && parent->is_protected) {
		child->is_protected = 1;
		child->inherited_protected = 1;
		child->group = GROUP_PROTECTED;
	}
	return child;
}

void process_remove(pid_t pid)
{
	int idx = proc_index(pid);

	if(proc_table[idx].used && proc_table[idx].pid == pid)
		memset(&proc_table[idx],0,sizeof(proc_table[idx]));
}

int process_read_exe(struct proc_info *p)
{
	char path[256];
	ssize_t len;

	if(!p)
		return -1;
	snprintf(path, sizeof(path), "/proc/%d/exe",p->pid);

	len = readlink(path, p->exe_path,sizeof(p->exe_path) -1);
	if(len < 0) {
		p->exe_path[0] = '\0';
		return -1;
	}

	p->exe_path[len] = '\0';
	return 0;
}

int process_read_cmdline(struct proc_info *p)
{
	char path[256];
	FILE *f;
	size_t n;

	if(!p)
		return -1;

	snprintf(path,sizeof(path),"/proc/%d/cmdline",p->pid);
	
	f = fopen(path,"r");
	if(!f) {
		p->cmdline[0] = '\0';
		return -1;
	}

	n = fread(p->cmdline, 1, sizeof(p->cmdline) -1,f);
	fclose(f);

	if(n == 0) {
		p->cmdline[0] = '\0';
		return -1;
	}

	p->cmdline[n] = '\0';

	for (size_t i = 0; i < n; i++) {
		if(p->cmdline[i] == '\0')
			p->cmdline[i] = ' ';
	}
	return 0;
}

