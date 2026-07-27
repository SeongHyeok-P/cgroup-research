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

#include <inttypes.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include "calibration.h"
#include "dram_mapping.h"


static int daemon_pid = 0;
enum daemon_state {
	DAEMON_STATE_STARTING = 0,
	DAEMON_STATE_CONFIG_READY,
	DAEMON_STATE_CALIBRATING,
	DAEMON_STATE_BANK_READY,
	DAEMON_STATE_BPF_READY,
	DAEMON_STATE_RUNNING,
	DAEMON_STATE_FAILED
};

struct daemon_context {
	enum daemon_state state;

	struct calibration_result calibration;
	struct dram_mapping dram_mapping;

	bool bank_mapping_ready;
};

static struct daemon_context g_ctx;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args) {
    if(level == LIBBPF_DEBUG)
	    return 0;
    return vfprintf(stderr, format, args);
}

static int init_dram_bank_mapping(struct daemon_context *ctx) 
{
	struct calibration_config cfg = {
		.worker_path = "scripts/kk_calibrate.py",
		.kk_main_path = "../Knock-Knock/main",
		.result_path = "/run/protected-daemon/dram-map.json",
		.work_dir = "/run/protected-daemon/kk-calibration",
		.log_path = "/run/protected-daemon/calibration.log",

		.runs = 3U,
		.memory_percent = 25.0,
		.measurements = 500U,
		.timing_rounds = 50U,
		.timeout_sec = 1800U,

		.keep_csv = true
	};

	int rc;
	size_t i;

	if (!ctx)
		return -1;

	ctx->state = DAEMON_STATE_CALIBRATING;
	ctx->bank_mapping_ready = false;

	printf("[INFO] starting DRAM bank calibration...\n");

	rc = calibration_run(&cfg,&ctx->calibration);
	if (rc != CALIBRATION_OK) {
		fprintf(stderr, "[ERROR] calibration failed: %s\n",calibration_strerror(rc));

		if (ctx->calibration.error_message[0] != '\0') {
			fprintf(stderr, "[ERROR] calibration detail: %s\n",ctx->calibration.error_message);
		}

		ctx->state = DAEMON_STATE_FAILED;
		return -1;
	}

	printf("[INFO] calibration success: masks=%zu precision=%.4f recall=%.4f\n",ctx->calibration.bank_mask_count,ctx->calibration.holdout.precision,ctx->calibration.holdout.recall);

	rc = dram_mapping_init_from_calibration(&ctx->dram_mapping,&ctx->calibration);

	if (rc != DRAM_MAPPING_OK) {
		fprintf(stderr, "[ERROR] dram mapping init failed: %s\n",dram_mapping_strerror(rc));

		ctx->state = DAEMON_STATE_FAILED;
		return -1;
	}

	ctx->bank_mapping_ready = true;
	ctx->state = DAEMON_STATE_BANK_READY;

	printf("[INFO] DRAM bank mapping ready: mask_set_id=%s mask_count=%zu\n",ctx->dram_mapping.mask_set_id,ctx->dram_mapping.mask_count);

	for (i = 0; i < ctx->dram_mapping.mask_count; i++) {
		printf("[INFO] bank_mask[%zu]=0x%016" PRIx64 "\n",i,(unsigned long)ctx->dram_mapping.masks[i]);
	}
	return 0;
}


// 이벤트 핸들러: BPF 커널 코드가 Ring Buffer로 보낸 데이터를 처리
static int handle_event(void *ctx, void *data, size_t data_sz)
{
    struct daemon_context *dctx = ctx;
    const struct event *e = data;

    (void)dctx;
    (void)data_sz;

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
    const char *config_path = "config/protected.conf";
    int err;
    
    if(argc >= 2)
	    config_path = argv[1];
    
    memset(&g_ctx,0,sizeof(g_ctx));    
    g_ctx.state = DAEMON_STATE_STARTING;

    daemon_pid = getpid();    
    printf("[INFO] daemon_pid=%d\n", daemon_pid);

    if(policy_load_config(config_path) < 0)
	    return 1;
	
    g_ctx.state = DAEMON_STATE_CONFIG_READY;

    if (init_dram_bank_mapping(&g_ctx) < 0) 
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

    rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, &g_ctx, NULL);

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
