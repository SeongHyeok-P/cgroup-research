#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<fcntl.h>
#include<string.h>
#include<errno.h>
#include<signal.h>


#define HIGH_CPU_MAX "/sys/fs/cgroup/cgroup_research/High/cpu.max"
#define MID_CPU_MAX "/sys/fs/cgroup/cgroup_research/Middle/cpu.max"
#define LOW_CPU_MAX "/sys/fs/cgroup/cgroup_research/Low/cpu.max"
static int running = 1;
void handle_sigint(int sig) {
	running = 0;
}
int write_file(const char *path, const char *value){
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

int main(void){
	signal(SIGINT, handle_sigint);
	signal(SIGTERM, handle_sigint);

	printf("[daemon] cgroup CPU daemon started\n");
	//High:max , Middle:50% , Low:20%
	
	while(running) {
		write_file(HIGH_CPU_MAX,"max 100000");	
		write_file(MID_CPU_MAX,"50000 100000");
		write_file(LOW_CPU_MAX,"20000 100000");	
		printf("[daemon] applied: High:max  Middle:50%% Low:20%% \n");
		sleep(3);
	}
	//reset cpu
	write_file(HIGH_CPU_MAX,"max 100000");
	write_file(MID_CPU_MAX,"max 100000");
	write_file(LOW_CPU_MAX,"max 100000");
	
	printf("[daemon] restored cpu.max and exited\n");

	return 0;
}

