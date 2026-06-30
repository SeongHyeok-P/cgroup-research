#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include "protected_daemon.h"
#include "protected_daemon.skel.h"
#include"process.h"
#include"policy.h"
#include"cgroup.h"



static int daemon_pid = 0;
static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args) {
    if(level == LIBBPF_DEBUG)
	    return 0;
    return vfprintf(stderr, format, args);
}

// 이벤트 핸들러: BPF 커널 코드가 Ring Buffer로 보낸 데이터를 처리
static int handle_event(void *ctx, void *data, size_t data_sz)
{
    const struct event *e = data;
    if(e->type == EVENT_EXEC) {
	    struct proc_info *p = process_upsert_exec(e);
	    enum proc_group g;

	    if(!p)
		    return 0;
	    if(p->pid == daemon_pid || p->ppid == daemon_pid)
		    return 0;
	    p->exec_seen = 1;

	    if(policy_is_protected(p))
		    p->is_protected = 1;

	    g = policy_decide_group(p);
	    p->group = g;

	    printf("[EXEC] pid=%d ppid=%d uid=%u comm=%s group=%s\n",
			    p->pid,p->ppid,p->uid,p->comm,cgroup_group_name(g));

	    if(g == GROUP_PROTECTED)
		    cgroup_move_pid(p->pid, GROUP_PROTECTED);

	    return 0;
    }
    if(e->type == EVENT_FORK) {
	    struct proc_info *parent;
	    struct proc_info *child;

	    child = process_upsert_fork(e);
	    parent = process_get(e->pid);

	    if(!child)
		    return 0;
	    printf("[FORK] parent=%d child=%d comm=%s\n",
			    e->pid,e->child_pid,e->comm);
	    if(parent && parent->used && parent->is_protected) {
		    child->is_protected = 1;
		    child->inherited_protected = 1;
		    child->group = GROUP_PROTECTED;

		    printf(" └─ inherited protected: child=%d\n",child->pid);
		    cgroup_move_pid(child->pid,GROUP_PROTECTED);
	    }
	    return 0;
    }
    if(e->type == EVENT_EXIT) {
	    printf("[EXIT] pid=%d comm=%s\n", e->pid, e->comm);
	    process_remove(e->pid);
	    return 0;
    }
    return 0;
}
int main(int argc, char **argv)
{
    struct ring_buffer *rb = NULL;
    struct protected_daemon_bpf *skel;
    const char *config_path = "../config/protected.conf";
    int err;
    
    if(argc >= 2)
	    config_path = argv[1];

    daemon_pid = getpid();    
    printf("[INFO] daemon_pid=%d\n", daemon_pid);

    if(policy_load_config(config_path) < 0)
	    return 1;

    libbpf_set_print(libbpf_print_fn);

    skel = protected_daemon_bpf__open_and_load();

    if (!skel) {
        fprintf(stderr, "Failed to open and load BPF skeleton\n");
        return 1;
    }

    err = protected_daemon_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Failed to attach BPF skeleton\n");
        goto cleanup;
    }

    rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, NULL, NULL);

    if (!rb) {
        err = -1;
        fprintf(stderr, "Failed to create ring buffer\n");
        goto cleanup;
    }

    printf("[INFO] daemon is running...\n");
   

    while (1) {
        err = ring_buffer__poll(rb, 100 /* timeout, ms */);
        if (err < 0) {
            printf("Error polling ring buffer: %d\n", err);
            break;
        }
    }

cleanup:
    ring_buffer__free(rb);
    protected_daemon_bpf__destroy(skel);
    return -err;
}
