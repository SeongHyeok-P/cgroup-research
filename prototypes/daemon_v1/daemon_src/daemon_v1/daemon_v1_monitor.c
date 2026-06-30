#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<dirent.h>
#include<ctype.h>
#include<unistd.h>

#define MAX_PID 4194304

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
	
	if(!fgets(buf,size,fp)) {	//read string
		fclose(fp);
		return -1;
	}
	
	buf[strcspn(buf,"\n")] = 0;	//terminate \n
	fclose(fp);
	return 0;
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
		}
	}
	closedir(dir);
}
int main(void) {
	while(1) {
		scan_proc();
		sleep(1);
	}

	return 0;
}
