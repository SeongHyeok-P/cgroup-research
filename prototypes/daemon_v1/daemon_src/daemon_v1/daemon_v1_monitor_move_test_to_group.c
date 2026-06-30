#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<dirent.h>
#include<ctype.h>
#include<unistd.h>
#include<fcntl.h>
#include<errno.h>

#define MAX_PID 4194304

#define CGROUP_BASE "/sys/fs/cgroup/cgroup_research"

#define HIGH_PROCS CGROUP_BASE "/High/cgroup.procs"
#define MID_PROCS CGROUP_BASE "/Middle/cgroup.procs"
#define LOW_PROCS CGROUP_BASE "/Low/cgroup.procs"

#define HIGH_CPU_MAX CGROUP_BASE "/High/cpu.max"
#define MID_CPU_MAX CGROUP_BASE "/Middle/cpu.max"
#define LOW_CPU_MAX CGROUP_BASE "/Low/cpu.max"

static char seen[MAX_PID];
//only number
int is_num(const char *s) {
        for(int i = 0; s[i]; i++) {
                if(!isdigit((unsigned char)s[i])) return 0;
        }
        return 1;
}
//read process name
int read_comm(int pid, char *buf, size_t size) {
        char path[256];
        snprintf(path, sizeof(path), "/proc/%d/comm",pid);

        FILE *fp = fopen(path,"r");
        if(!fp) return -1;

        if(!fgets(buf,size,fp)) {       //read string
                fclose(fp);
                return -1;
        }

        buf[strcspn(buf,"\n")] = 0;     //terminate \n
        fclose(fp);
        return 0;
}
//write cpu.max
int write_file(const char *path, const char *value) {
	int fd = open(path,O_WRONLY);
	if(fd < 0) {
		perror(path);
		return -1;
	}

	ssize_t len = strlen(value);
	ssize_t written = write(fd,value,len);

	if(written != len) {
		perror("write");
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}
int move_pid_to_cgroup(int pid, const char *procs_path){
	char pid_str[32];
	snprintf(pid_str,sizeof(pid_str),"%d\n",pid);

	return write_file(procs_path,pid_str);
}
void classify_and_move(int pid, const char *comm){
	if(strcmp(comm, "t1_high") == 0) {
		if(move_pid_to_cgroup(pid,HIGH_PROCS) == 0) {
			printf("[move] pid=%d comm=%s -> High\n",pid,comm);
		}
	}
	else if(strcmp(comm, "t1_middle") == 0) {
		if(move_pid_to_cgroup(pid,MID_PROCS) == 0) {
			printf("[move] pid=%d comm=%s -> Middle\n",pid,comm);
		}
	}
	else if(strcmp(comm, "t1_low") == 0) {
		if(move_pid_to_cgroup(pid,LOW_PROCS) == 0) {
			printf("[move] pid=%d comm=%s -> Low\n",pid,comm);
		}
	}
}
void scan_proc(void) {
        DIR *dir = opendir("/proc");
        if(!dir) {
                perror("opendir /proc");
                return;
        }

        struct dirent *entry;
        while((entry = readdir(dir)) != NULL) {
                if(!is_num(entry->d_name)) continue;

                int pid = atoi(entry->d_name);
                if(pid <= 0 || pid >= MAX_PID) continue;

                if(seen[pid]) continue;
                seen[pid] = 1;

                char comm[256];
                if(read_comm(pid,comm,sizeof(comm)) == 0) {
                        printf("[new process] pid=%d comm=%s\n",pid,comm);
			classify_and_move(pid,comm);
                }
        }
        closedir(dir);
}
void apply_cpu_policy(void) {
	write_file(HIGH_CPU_MAX,"max 100000");
	write_file(MID_CPU_MAX,"50000 100000");
	write_file(LOW_CPU_MAX,"20000 100000");
	
	printf("[policy] High=max , MID=50%%, LOW=20%%\n");
}
int main(void) {
	printf("[daemon] proc cgroup daemon started\n");
	apply_cpu_policy();

        while(1) {
                scan_proc();
                usleep(200000);
        }

        return 0;
}
