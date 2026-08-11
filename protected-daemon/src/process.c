#define _POSIX_C_SOURCE 200809L

#include<stdio.h>
#include<string.h>
#include<unistd.h>

#include"process.h"

enum process_slot_state {
	PROCESS_SLOT_EMPTY = 0,
	PROCESS_SLOT_OCCUPIED,
	PROCESS_SLOT_TOMBSTONE
};

static struct proc_info proc_table[PROC_MAX];      /* store proc info */
static unsigned char slot_states[PROC_MAX];	   /* store states of proc_table slots */
static size_t occupied_count;			   /* store pid count in proc_table except tombstone */	

static size_t process_hash(pid_t pid)
{
	return (size_t)((uint64_t)pid % (uint64_t)PROC_MAX);
}

/*
 * Lookup stops only at an EMPTY slot.
 * A TOMBSTONE cannot terminate lookup because an entry belonging to the 
 * same probe chain may exist after it
 */
static int find_existing_slot(pid_t pid,size_t *slot_out)
{
	size_t start;
	size_t probe;

	if (pid <= 0 || slot_out == NULL)
		return -1;

	start = process_hash(pid);

	for (probe = 0U; probe < (size_t)PROC_MAX; ++probe) {
		size_t idx = (start + probe) % (size_t)PROC_MAX;

		if (slot_states[idx] == PROCESS_SLOT_EMPTY)
			return 0;

		if (slot_states[idx] == PROCESS_SLOT_OCCUPIED && proc_table[idx].used && proc_table[idx].pid == pid) {
			*slot_out = idx;
			return 1;
		}
	}

	return 0;
}

/*
 * Find an existing PID or a slot where a new PID can be inserted.
 * The first tombstone is remembered, but probing continues so that an
 * existing matching PID later in the chain is never duplicated
 */
static int find_upsert_slot(pid_t pid, size_t *slot_out, int *found_out)
{
	size_t start;
	size_t probe;
	size_t first_tombstone = (size_t)PROC_MAX;

	if (pid <= 0 || slot_out == NULL || found_out == NULL)
		return -1;

	start = process_hash(pid);

	for (probe = 0U; probe < (size_t)PROC_MAX; ++probe) {
		size_t idx = (start + probe) % (size_t)PROC_MAX;

		if (slot_states[idx] == PROCESS_SLOT_OCCUPIED) {
			if (proc_table[idx].used && proc_table[idx].pid == pid) {
				*slot_out = idx;
				*found_out = 1;
				return 0;
			}
			continue;
		}

		if (slot_states[idx] == PROCESS_SLOT_TOMBSTONE) {
			if (first_tombstone == (size_t)PROC_MAX)
				first_tombstone = idx;
			continue;
		}

		/*
		 * EMPTY terminates the probe chain for insertion */
		*slot_out = first_tombstone != (size_t)PROC_MAX ? first_tombstone : idx;

		*found_out = 0;
		return 0;
	}

	/* The table has no EMPTY slot, but a tombstone may still be reusable */
	if (first_tombstone != (size_t)PROC_MAX) {
		*slot_out = first_tombstone;
		*found_out = 0;
		return 0;
	}

	return -1;
}

static struct proc_info *prepare_upsert(pid_t pid,int *found_out)
{
	size_t idx;
	int found;

	if (find_upsert_slot(pid,&idx,&found) != 0)
		return NULL;

	if (!found) {
		if (occupied_count >= (size_t)PROC_MAX)
			return NULL;

		memset(&proc_table[idx],0,sizeof(proc_table[idx]));
		slot_states[idx] = PROCESS_SLOT_OCCUPIED;
		occupied_count++;
	}

	if (found_out != NULL)
		*found_out = found;

	return &proc_table[idx];
}

void process_table_reset(void)
{
	memset(proc_table, 0, sizeof(proc_table));
	memset(slot_states, PROCESS_SLOT_EMPTY,sizeof(slot_states));
	occupied_count = 0U;
}

size_t process_table_count(void)
{
	return occupied_count;
}

size_t process_table_capacity(void)
{
	return (size_t)PROC_MAX;
}

struct proc_info *process_get(pid_t pid)
{
	size_t idx;
	int found;

	found = find_existing_slot(pid,&idx);
	if (found != 1)
		return NULL;

	return &proc_table[idx];
}

struct proc_info *process_upsert_exec(const struct event *e)
{
	struct proc_info *p;
	int found = 0;
	int preserve_is_protected = 0;
	int preserve_inherited_protected = 0;
	enum proc_group preserve_group = GROUP_UNKNOWN;

	if (e == NULL || e->pid <= 0)
		return NULL;

	p = prepare_upsert(e->pid,&found);
	if (p == NULL)
		return NULL;

	/*
	 * Fork creates a pre-exec child record. Exec must refresh process
	 * metadata without dropping protection inherited from its parent
	 * The same rule also preserves protection across a later execve()
	 */
	if (found) {
		preserve_is_protected = p->is_protected;
		preserve_inherited_protected = p->inherited_protected;
		preserve_group = p->group;
	}

	memset(p,0,sizeof(*p));

	p->used = 1;
	p->pid = e->pid;
	p->ppid = e->ppid;
	p->uid = e->uid;
	p->exec_seen = 1;
	p->is_protected = preserve_is_protected;
	p->inherited_protected = preserve_inherited_protected;
	p->group = preserve_group;

	if (!found)
		p->group = GROUP_UNKNOWN;

	(void)snprintf(p->comm,sizeof(p->comm),"%s",e->comm);

	(void)process_read_exe(p);
	(void)process_read_cmdline(p);

	return p;
}

struct proc_info *process_upsert_fork(const struct event *e)
{
	struct proc_info *parent;
	struct proc_info *child;

	if (e == NULL || e->pid <= 0 || e->child_pid <= 0)
		return NULL;

	parent = process_get(e->pid);
	child = prepare_upsert(e->child_pid,NULL);
	if (child == NULL)
		return NULL;

	/* A fork event creates a fresh process identity for child_pid. */
	memset(child,0,sizeof(*child));

	child->used = 1;
	child->pid = e->child_pid;
	child->ppid = e->pid;
	child->uid = e->uid;
	child->group = GROUP_UNKNOWN;

	(void)snprintf(child->comm, sizeof(child->comm),"%s",e->comm);

	if(parent && parent->used && parent->is_protected) {
		child->is_protected = 1;
		child->inherited_protected = 1;
		child->group = GROUP_PROTECTED;
	}
	return child;
}

void process_remove(pid_t pid)
{
	size_t idx;
	int found;

	found = find_existing_slot(pid, &idx);
	if (found != 1)
		return;

	memset(&proc_table[idx],0,sizeof(proc_table[idx]));
	slot_states[idx] = PROCESS_SLOT_TOMBSTONE;

	if (occupied_count > 0U)
		occupied_count--;
}

int process_read_exe(struct proc_info *p)
{
	char path[256];
	ssize_t len;
	int written;

	if(p == NULL || p->pid <= 0)
		return -1;

	written = snprintf(path, sizeof(path), "/proc/%ld/exe",(long)p->pid);
	if (written <= 0 || (size_t)written >= sizeof(path)) {
		p->exe_path[0] = '\0';
		return -1;
	}

	len = readlink(path, p->exe_path,sizeof(p->exe_path) -1U);
	if(len < 0) {
		p->exe_path[0] = '\0';
		return -1;
	}

	p->exe_path[(size_t)len] = '\0';
	return 0;
}

int process_read_cmdline(struct proc_info *p)
{
	char path[256];
	FILE *f;
	size_t n;
	size_t i;
	int written;

	if(p == NULL || p->pid <= 0)
		return -1;

	written = snprintf(path,sizeof(path),"/proc/%ld/cmdline",(long)p->pid);
	if (written <= 0 || (size_t)written >= sizeof(path)) {
		p->cmdline[0] = '\0';
		return -1;
	}
	
	f = fopen(path,"r");
	if(f == NULL) {
		p->cmdline[0] = '\0';
		return -1;
	}

	n = fread(p->cmdline, 1U, sizeof(p->cmdline) -1U,f);
	if (fclose(f) != 0) {
		p->cmdline[0] = '\0';
		return -1;
	}

	if(n == 0U) {
		p->cmdline[0] = '\0';
		return -1;
	}

	p->cmdline[n] = '\0';
	
	/* /proc/[pid]/cmdline separates arguments with NULL bytes */
	for (i = 0U; i < n; i++) {
		if(p->cmdline[i] == '\0')
			p->cmdline[i] = ' ';
	}
	return 0;
}

int process_snapshot(struct proc_info *out, size_t capacity, size_t *out_count)
{
    size_t i;
    size_t actual_count = 0U;
    size_t copied_count = 0U;

    if (out_count == NULL)
        return PROCESS_ERR_INVALID_ARG;

    /*
     * 실패하는 모든 경우에 caller가 이전 out_count 값을
     * 잘못 사용하지 않도록 먼저 0으로 만든다.
     */
    *out_count = 0U;

    if (out == NULL)
        return PROCESS_ERR_INVALID_ARG;

    /*
     * First pass:
     * 실제 OCCUPIED entry 수를 확인하면서
     * process table의 내부 일관성도 검사한다.
     *
     * 아직 out[]에는 아무것도 쓰지 않는다.
     */
    for (i = 0U; i < (size_t)PROC_MAX; i++) {
        if (slot_states[i] == PROCESS_SLOT_OCCUPIED) {
            if (!proc_table[i].used || proc_table[i].pid <= 0)
                return PROCESS_ERR_INCONSISTENT;

            actual_count++;
            continue;
        }

        /*
         * EMPTY 또는 TOMBSTONE인데 used=1이라면
         * slot state와 proc_table 내용이 서로 모순이다.
         */
        if (proc_table[i].used)
            return PROCESS_ERR_INCONSISTENT;
    }

    /*
     * 우리가 따로 관리하는 occupied_count와
     * 실제 table 내용이 일치하는지 검증한다.
     */
    if (actual_count != occupied_count)
        return PROCESS_ERR_INCONSISTENT;

    /*
     * 부분 snapshot은 허용하지 않는다.
     */
    if (capacity < actual_count)
        return PROCESS_ERR_NO_SPACE;

    /*
     * Second pass:
     * 전체 snapshot이 buffer에 들어간다는 것이 확인된 후
     * 실제 데이터를 복사한다.
     */
    for (i = 0U; i < (size_t)PROC_MAX; i++) {
        if (slot_states[i] != PROCESS_SLOT_OCCUPIED)
            continue;

        out[copied_count++] = proc_table[i];
    }

    *out_count = copied_count;

    return PROCESS_OK;
}
