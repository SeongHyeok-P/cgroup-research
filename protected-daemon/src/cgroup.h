#ifndef CGROUP_H
#define CGROUP_H

#include <stddef.h>
#include <sys/types.h>

#define CGROUP_DEFAULT_BASE "/sys/fs/cgroup/cgroup_research"
#define CGROUP_PATH_MAX 512U
#define CGROUP_VALUE_MAX 128U

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Static policy groups.
 * GROUP_PROTECTED  : protected.conf matched tasks.
 * GROUP_BACKGROUND : default normal/background tasks.
 * GROUP_THROTTLED  : temporary runtime group selected by throttle_controller
 * GROUP_IGNORED    : tasks the daemon should not manage
 * GROUP_UNKNOWN    : unset/error state.
 */
enum proc_group {
	GROUP_PROTECTED = 0,
	GROUP_BACKGROUND,
	GROUP_THROTTLED,
	GROUP_IGNORED,
	GROUP_UNKNOWN
};

const char *cgroup_group_name(enum proc_group group);

/*
 * Default base is CGROUP_DEFAULT_BASE
 * Tests may override it with a temporary fake cgroup hierarchy
 */
int cgroup_set_base_path(const char *base_path);
const char *cgroup_get_base_path(void);

/*
 * Build paths under the configured cgroup base
 */
int cgroup_group_dir(enum proc_group group, char *out, size_t out_size);
int cgroup_control_path(enum proc_group group, const char *control_file,char *out, size_t out_size);

/*
 * Generic cgroup v2 control writer
 * The value is written followed by a newline.
 */
int cgroup_write_control(enum proc_group group,const char *control_file, const char *value);

/*
 * Move pid by writing it to cgroup.procs of the selected group
 */
int cgroup_move_pid(pid_t pid, enum proc_group group);

/*
 * Convenience wrappers for controls used by this research prototype.
 */
int cgroup_set_cpu_max(enum proc_group group, const char *value);
int cgroup_set_memory_low(enum proc_group group, const char *value);
int cgroup_set_memory_high(enum proc_group group, const char *value);
int cgroup_set_memory_max(enum proc_group group, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* CGROUP_H */
