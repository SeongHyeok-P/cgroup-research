#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "daemon_v2.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} rb SEC(".maps");
SEC("tp_btf/sched_process_fork")
int BPF_PROG(handle_fork, struct task_struct *parent, struct task_struct *child)
{
	struct event *e;
	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if(!e)
           return 0;
	e->type = EVENT_FORK;
	e->pid = BPF_CORE_READ(parent, tgid);
	e->ppid = BPF_CORE_READ(parent, real_parent, tgid);
	e->child_pid = BPF_CORE_READ(child, tgid);
	{
		u64 uid_gid = bpf_get_current_uid_gid();
		e->uid = uid_gid & 0xffffffff;
	}
	bpf_get_current_comm(&e->comm, sizeof(e->comm));

	bpf_ringbuf_submit(e,0);
	return 0;
}
SEC("tp_btf/sched_process_exit")
int BPF_PROG(handle_exit, struct task_struct *task)
{
	struct event *e;
	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if(!e)
	   return 0;
	e->type = EVENT_EXIT;
	e->pid = BPF_CORE_READ(task, tgid);
	e->ppid = BPF_CORE_READ(task, real_parent, tgid);
	e->child_pid = 0;
	{
		u64 uid_gid = bpf_get_current_uid_gid();
		e->uid = uid_gid & 0xffffffff;
	}
	bpf_get_current_comm(&e->comm, sizeof(e->comm));
	bpf_ringbuf_submit(e,0);
	return 0;
}
SEC("tp_btf/sched_process_exec")
int BPF_PROG(handle_exec, struct task_struct *task, pid_t old_pid, struct linux_binprm *bprm)
{
    struct event *e;

    e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
    if (!e)
        return 0;
    e->type = EVENT_EXEC;
    e->pid = BPF_CORE_READ(task, tgid);
    e->ppid = BPF_CORE_READ(task, real_parent, tgid);
   {
    u64 uid_gid = bpf_get_current_uid_gid();
    e->uid = uid_gid & 0xffffffff;
   }
   e->child_pid = 0;

    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
