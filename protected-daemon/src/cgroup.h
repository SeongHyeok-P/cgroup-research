#ifndef CGROUP_H
#define CGROUP_H

#include<sys/types.h>
#include"process.h"

int cgroup_move_pid(pid_t pid, enum proc_group group);
const char *cgroup_group_name(enum proc_group group);

#endif
