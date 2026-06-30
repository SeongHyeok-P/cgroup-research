#ifndef POLICY_H
#define POLICY_H

#include"process.h"

int policy_load_config(const char *path);
int policy_is_ignored(const struct proc_info *p);
int policy_is_protected(const struct proc_info *p);
enum proc_group policy_decide_group(struct proc_info *p);

#endif
