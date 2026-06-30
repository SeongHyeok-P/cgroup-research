#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include "daemon_v2.h"
#include "daemon_v2.skel.h"
#include<time.h>
#include<string.h>

#define PROC_MAX 32768


static int daemon_pid = 0;
static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 100000LL;
}
enum proc_class {
	CLASS_USER,
	CLASS_SYSTEM,
	CLASS_IGNORE,
};
enum proc_group {
	GROUP_USER_HIGH,
	GROUP_USER_IMPORTANT,
	GROUP_USER_MID,
	GROUP_USER_LOW,
	GROUP_SYSTEM_PROTECT,
	GROUP_SYSTEM_CRITICAL,
	GROUP_SYSTEM_SERVICE,
	GROUP_SYSTEM_MAINTENANCE,
	GROUP_UNKNOWN,
};

struct proc_info {
	int used;
	int pid;
	int ppid;
	int uid;
	int score;

	int tier_a_seen;
	int is_system;
	int exited;
	int exec_seen;

	int tty_nr;
	int pgrp;
	int session;
	int is_tty_process;
	int is_shell_child;

	long long created_ms;
	long long last_event_ms;
	long long last_score_update_ms;
	long long last_move_ms;
	enum proc_class class_type;
	enum proc_group group;
	char comm[TASK_COMM_LEN];
};

static struct proc_info proc_table[PROC_MAX];
static int read_proc_stat_basic(struct proc_info *p) {
	char path[256];
	char buf[1024];
	snprintf(path,sizeof(path), "/proc/%d/stat", p->pid);
	FILE *f = fopen(path,"r");
	if(!f)
		return -1;
	if(!fgets(buf,sizeof(buf),f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	/*
	 * /proc/[pid]/stat format;
	 * pid(comm) state ppid pgrp session tty_nr ...
	 * comm can contain spaces, so find last ')' first
	 */
	char *rparen = strrchr(buf,')');
	if(!rparen)
		return -1;
	char state;
	int ppid, pgrp, session, tty_nr;
	/*
	 * after ')': space state space ppid pgrp session tty_nr...
	 */
	int n = sscanf(rparen+2, "%c %d %d %d %d",&state, &ppid, &pgrp, &session, &tty_nr);
	if(n != 5) return -1;

	p->ppid = ppid;
	p->pgrp = pgrp;
	p->session = session;
	p->tty_nr = tty_nr;
	p->is_tty_process = (tty_nr != 0);

	return 0;
}
static int is_shell_comm(const char *comm){
	if(!comm) return 0;
	if(!strcmp(comm,"bash")) return 1;
	if(!strcmp(comm,"sh")) return 1;
	if(!strcmp(comm,"zsh")) return 1;
	if(!strcmp(comm,"fish")) return 1;

	return 0;
}
static int parent_is_shell(int pid) {
	char path[256];
	char comm[TASK_COMM_LEN]={0};
	snprintf(path, sizeof(path),"/proc/%d/comm",pid);

	FILE *f = fopen(path,"r");
	if(!f) return 0;

	if(!fgets(comm,sizeof(comm),f)) {
		fclose(f);
		return 0;
	}
	fclose(f);

	comm[strcspn(comm,"\n")] = '\0';
	return is_shell_comm(comm);
}
static int detect_tier_a_terminal_exec(struct proc_info *p) {
	if(p->class_type != CLASS_USER) return 0;
	if(!p->is_tty_process) return 0;
	if(parent_is_shell(p->ppid)) return 1;

	return 0;

}
static enum proc_class classify_proc_basic(const struct event *e) {
	/*
	 * ignore:
	 * -PID 1,2같은 핵심 프로세스
	 * -daemon자기 자신
	 *  -systemd 자체
	 */
	if(e->pid <= 2)
		return CLASS_IGNORE;
	if(e->pid == daemon_pid)
		return CLASS_IGNORE;
	if(e->ppid == daemon_pid)
		return CLASS_IGNORE;
	if(e->comm[0] == '\0')
		return CLASS_IGNORE;
	if(!strcmp(e->comm , "systemd"))
		return CLASS_IGNORE;
	/*
	 *System:
	 * -root 권한 프로세스
	 */
	if(e->uid == 0)
		return CLASS_SYSTEM;
	/*
	 * User:
	 * -일반 사용자 프로세스
	 */
	return CLASS_USER;
}
static int proc_index(int pid) {
	if(pid < 0)
	   pid = -pid;
	return pid % PROC_MAX;
}
static struct proc_info *get_proc(int pid) {
	int idx = proc_index(pid);

	if(proc_table[idx].used && proc_table[idx].pid != pid) {
		//초기 충돌시 덮어씀. 나중에 hash_table로 교체
	}
	return &proc_table[idx];
}
static struct proc_info *upsert_proc(const struct event *e) {
	int idx = proc_index(e->pid);
	struct proc_info *p = &proc_table[idx];
	long long now = now_ms();

	if(!p->used || p->pid != e->pid) {
		p->used = 1;
		p->pid = e->pid;
		p->created_ms = now;
		p->score = 40;
		p->tier_a_seen = 0;
		p->group = GROUP_UNKNOWN;
		p->last_score_update_ms = now;
		p->last_move_ms = 0;
	}

	p->ppid = e->ppid;
	p->uid = e->uid;
	p->last_event_ms = now;

	snprintf(p->comm, sizeof(p->comm), "%s", e->comm);
	return p;
}
static void remove_proc(int pid) {
	int idx = proc_index(pid);
	if(proc_table[idx].used && proc_table[idx].pid == pid) {
		proc_table[idx].used = 0;
	}
}
static void apply_decay(struct proc_info *p) {
	long long now = now_ms();
	if(!p->last_score_update_ms) {
		p->last_score_update_ms = now;
		return;
	}
	long long elapsed_ms = now - p->last_score_update_ms;
	int elapsed_sec = elapsed_ms / 1000;
	if(elapsed_sec <= 0)
		return;
	int decay = elapsed_sec * 5;
	if(p->score > decay)
		p->score -= decay;
	else
		p->score = 0;
	p->last_score_update_ms += elapsed_sec * 1000;
}
static void score_exec(struct proc_info *p) {
	long long now = now_ms();
	if(p->score < 40)
		p->score = 40;

	p->last_event_ms = now;
	p->last_score_update_ms = now;
	//Tier A terminal foreground-like exec 
	if(detect_tier_a_terminal_exec(p)) {
		p->tier_a_seen = 1;
		p->score += 40;

		if(p->score > 100)
			p->score = 100;
		printf("└─ Tier A: terminal shell exec detected\n");
	}
}
static void score_fork(const struct event *e) {
	long long now = now_ms();

	struct proc_info *parent = get_proc(e->pid);
	struct proc_info *child = get_proc(e->child_pid);

	child->used = 1;
	child->exec_seen = 0;
	child->pid = e->child_pid;
	child->ppid = e->pid;
	child->uid = e->uid;
	child->created_ms = now;
	child->last_event_ms = now;
	child->last_score_update_ms = now;

	if(parent->used && parent->pid == e->pid) {
		child->score = parent->score > 30 ? parent->score - 10 : 30;
		child->tier_a_seen = parent->tier_a_seen;
		child->class_type = parent->class_type;
	}
	else {
		child->score = 30;
		child->tier_a_seen = 0;
		child->class_type = classify_proc_basic(e);
	}
	child->group = GROUP_UNKNOWN;
	snprintf(child->comm, sizeof(child->comm), "%s", e->comm);
}
static void score_exit(const struct event *e) {
	remove_proc(e->pid);
}
static enum proc_group decide_user_group(struct proc_info *p) {
	if(p->score >= 80) {
		if(p->tier_a_seen)
			return GROUP_USER_HIGH;
	else
		return GROUP_USER_IMPORTANT;
	}
	if(p->score >= 50) 
		return GROUP_USER_IMPORTANT;
	if(p->score >= 20)
		return GROUP_USER_MID;
	return GROUP_USER_LOW;
}
static const char *group_to_path(enum proc_group group) {
	switch(group) {
		case GROUP_USER_HIGH:
			return "/sys/fs/cgroup/cgroup_research/User/High/cgroup.procs";
		case GROUP_USER_IMPORTANT:
			return "/sys/fs/cgroup/cgroup_research/User/Important/cgroup.procs";
		case GROUP_USER_MID:
			return "/sys/fs/cgroup/cgroup_research/User/Mid/cgroup.procs";
		case GROUP_USER_LOW:
			return "/sys/fs/cgroup/cgroup_research/User/Low/cgroup.procs";
		case GROUP_SYSTEM_SERVICE:
			return "/sys/fs/cgroup/cgroup_research/System/Service/cgroup.procs";
		default:
			return NULL;
	}
}
static enum proc_group decide_system_group(struct proc_info *p) {
	return GROUP_SYSTEM_SERVICE;
}
static int move_to_cgroups(struct proc_info *p, enum proc_group new_group) {
	long long now = now_ms();
	if(p->group == new_group)
		return 0;
	if(p->last_move_ms && now - p->last_move_ms < 1000)
		return 0;
	const char *path = group_to_path(new_group);
	if(!path)
		return -1;
	FILE *f = fopen(path,"a");
	if(!f) {
		printf(" └─ Cgroup Error: cannot open %s\n", path);
		return -1;
	}
	if(fprintf(f,"%d\n",p->pid) < 0) {
		fclose(f);
		printf(" └─ Cgroup Error: failed to move pid=%d\n",p->pid);
		return -1;
	}
	fclose(f);
	p->group = new_group;
	p->last_move_ms = now;
	printf("  └─ moved pid=%d score=%d to %s\n",
        p->pid, p->score, path);

	return 0;
}
static void print_cmdline(int pid) {
	char proc_path[256];
	char proc_cmdline[256] = {0};
	snprintf(proc_path, sizeof(proc_path), "/proc/%d/cmdline",pid);

	FILE *proc_f = fopen(proc_path, "r");
	if(!proc_f) {
		printf("  └─ Failed to read %s\n", proc_path);
	        return;
	}
	size_t bytes_read = fread(proc_cmdline,1,sizeof(proc_cmdline) - 1, proc_f);
	fclose(proc_f);
	
	if(bytes_read > 0) {
		for(size_t i = 0; i < bytes_read; i++) {
			if(proc_cmdline[i] == '\0')
			   proc_cmdline[i] = ' ';
		}
		 printf("  └─ Command Line: %s\n", proc_cmdline);
	}	 
	else  printf("  └─ Command Line: (empty or process already exited)\n");
}
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
	    struct proc_info *p = upsert_proc(e);
	    
	    p->exec_seen = 1;
	    p->class_type = classify_proc_basic(e);
	    
	    read_proc_stat_basic(p);
	    printf("\n[EXEC] pid=%d ppid=%d uid=%u comm=%s class=%d score=%d tty=%d pgrp=%d session=%d\n",
                     e->pid, p->ppid, e->uid, e->comm,
	             p->class_type, p->score,
          	     p->tty_nr, p->pgrp, p->session);

	    if(p->class_type == CLASS_IGNORE) {
		     printf("  └─ ignored process\n");
		     return 0;
	    }	    

	    print_cmdline(e->pid);
	    
	    if(p->class_type == CLASS_SYSTEM) {
		    enum proc_group g = decide_system_group(p);
		    move_to_cgroups(p,g);
		    return 0;
	    }

	    if(p->class_type == CLASS_USER) {
		    score_exec(p);
		    enum proc_group g = decide_user_group(p);
	            move_to_cgroups(p,g);
		    return 0;
	    }
    }
    else if(e->type == EVENT_FORK) {
	    printf("\n[FORK] parent=%d child=%d uid=%u comm=%s\n",
               e->pid, e->child_pid, e->uid, e->comm);

            score_fork(e);
    }
    else if(e->type == EVENT_EXIT) {
	    printf("\n[EXIT] pid=%d uid=%u comm=%s\n",
               e->pid, e->uid, e->comm);
  
            score_exit(e);
    }
    return 0;
}
static void periodic_decay_and_move(void) {
	for(int i = 0; i < PROC_MAX; i++) {
		struct proc_info *p = &proc_table[i];

		if(!p->used) continue;
		if(!p->exec_seen) continue;
		if(p->class_type == CLASS_IGNORE)
			continue;
		if(p->class_type == CLASS_SYSTEM) {
			/*
			 * 지금 단계에서는 System 프로세스는 decay정책 적용안함
			 * 나중에 System 정책 만들때 따로 처리
			 */
			continue;
		}

		int old_score = p->score;
		enum proc_group old_group = p->group;

		apply_decay(p);
		
		enum proc_group new_group = decide_user_group(p);

	       printf("[TICK] pid=%d comm=%s score=%d->%d group=%d->%d age=%lldms\n",
               p->pid,
               p->comm,
               old_score,
               p->score,
               old_group,
               new_group,
               now_ms() - p->created_ms);

		if(new_group != old_group) {
			move_to_cgroups(p,new_group);
		}
	}
}
int main(int argc, char **argv)
{
    struct ring_buffer *rb = NULL;
    struct daemon_v2_bpf *skel;
    int err;
    
    daemon_pid = getpid();    
    printf("[INFO] daemon_pid=%d\n", daemon_pid);
    libbpf_set_print(libbpf_print_fn);

    skel = daemon_v2_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "Failed to open and load BPF skeleton\n");
        return 1;
    }

    err = daemon_v2_bpf__attach(skel);
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

    printf("Daemon is running. Tracing events...\n");
   
    long long last_tick_ms = now_ms();
    while (1) {
        err = ring_buffer__poll(rb, 100 /* timeout, ms */);
        if (err < 0) {
            printf("Error polling ring buffer: %d\n", err);
            break;
        }
	long long now = now_ms();
	if(now - last_tick_ms >= 1000) {
		periodic_decay_and_move();
		last_tick_ms = now;
	}
    }

cleanup:
    ring_buffer__free(rb);
    daemon_v2_bpf__destroy(skel);
    return -err;
}
