/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/* Copyright (c) 2020 Facebook */
#ifndef __DAEMON_BPF_H
#define __DAEMON_BPF_H

#define TASK_COMM_LEN	 16

enum event_type {
	EVENT_EXEC = 1,
	EVENT_FORK = 2,
	EVENT_EXIT = 3,
};
struct event {
	int type;

	int pid;    //fork시 부모 pid
	int ppid;   //fork시 부모의 부모 pid

	int child_pid; //fork에서만 의미 있음

	unsigned int uid;
	char comm[TASK_COMM_LEN];
};

#endif /* __DAEMON_BPF_H */
