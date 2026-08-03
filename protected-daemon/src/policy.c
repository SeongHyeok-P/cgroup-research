#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<ctype.h>
#include"policy.h"

#define MAX_PROTECTED 128

static char protected_comm[MAX_PROTECTED][TASK_COMM_LEN];
static int protected_count = 0;

static void trim_line(char *s)
{
	char *start;
	char *end;

	if (s == NULL)
		return;

	s[strcspn(s, "\r\n")] = '\0';

	start = s;
	while (*start && isspace((unsigned char)*start))
		start++;

	if (start != s)
		memmove(s , start, strlen(start) + 1U);

	end = s + strlen(s);
	while (end > s && isspace((unsigned char)*(end - 1))) {
		*(end - 1) = '\0';
		end--;
	}
}

int policy_load_config(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];

	protected_count = 0;

	if(!f) {
		fprintf(stderr, "[ERROR] failed to open config: %s\n",path);
		return -1;
	}

	while(fgets(line, sizeof(line),f)) {
		trim_line(line);

		if(line[0] == '\0')
			continue;
		if(line[0] == '#')
			continue;

		if(protected_count >= MAX_PROTECTED) {
			fprintf(stderr,"[WARN] protected config full, ignoring: %s\n",line);
			continue;
		}

		snprintf(protected_comm[protected_count],
				sizeof(protected_comm[protected_count]),
				"%s",line);
		printf("[CONFIG] protected comm: %s\n", protected_comm[protected_count]);
		protected_count++;
	}
	fclose(f);
	return 0;
}
int policy_is_ignored(const struct proc_info *p)
{
	if(!p)
		return 1;
	if(p->pid <= 2)
		return 1;
	if(p->comm[0] == '\0')
		return 1;
	return 0;
}

int policy_is_protected(const struct proc_info *p)
{
	if(!p)
		return 0;
	for(int i = 0; i < protected_count; i++) {
		if(!strcmp(p->comm, protected_comm[i]))
			return 1;
	}
	return 0;
}

enum proc_group policy_decide_group(struct proc_info *p)
{
	if(policy_is_ignored(p))
		return GROUP_IGNORED;

	if(p->is_protected || policy_is_protected(p))
		return GROUP_PROTECTED;

	return GROUP_BACKGROUND;
}

