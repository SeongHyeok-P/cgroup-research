#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile uint32_t sink_index = 0;

/*간단한 난수 생성기*/

static uint64_t rng_next(uint64_t *state)
{
	uint64_t x = *state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;

	*state = x;

	return x * 2685821657736338717ULL;
}

static double elapsed_sec(const struct timespec *start,const struct timespec *end) 
{
	return (double)(end->tv_sec - start->tv_sec) + (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr,"사용법: %s <size_MiB> <run_seconds>\n",argv[0]);
		return 1;
	}

	char *endptr = NULL;

	unsigned long size_mib = strtoul(argv[1],&endptr,10);

	if (*endptr != '\0' || size_mib == 0) {
		fprintf(stderr, "잘못된 size_MiB\n");
		return 1;
	}

	endptr = NULL;
	unsigned long run_seconds = strtoul(argv[2],&endptr,10);

	if (*endptr != '\0' || run_seconds == 0) {
		fprintf(stderr, "잘못된 run_seconds\n");
		return 1;
	}

	const size_t bytes = (size_t)size_mib * 1024ULL * 1024ULL;

	const size_t elements = bytes / sizeof(uint32_t);

	uint32_t *next = NULL;
	uint32_t *order = NULL;

	if (posix_memalign((void **)&next,64,elements * sizeof(uint32_t)) != 0) {
		fprintf(stderr, "next allocation failed\n");
		return 1;
	}

	if(posix_memalign((void **)&order,64,elements * sizeof(uint32_t)) != 0) {
		fprintf(stderr, "order allocation failed\n");
		return 1;
	}

	/*순열 초기화*/
	for (size_t i = 0; i < elements; i++) {
		order[i] = (uint32_t)i;
	}

	uint64_t rng_state = 0x123456789abcdefULL;

	for (size_t i = elements - 1; i > 0; i--) {
		size_t j = (size_t)(rng_next(&rng_state) % (i + 1));

		uint32_t tmp = order[i];
		order[i] = order[j];
		order[j] = tmp;
	}

	for (size_t i = 0; i + 1 < elements; i++) {
		next[order[i]] = order[i + 1];

	}
	next[order[elements - 1]] = order[0];

	uint32_t index = order[0];

	free(order);
	order = NULL;

	/* warm up*/
	for (size_t i = 0; i < elements; i++) {
		index = next[index];
	}

	sink_index = index;

	/* SIGCONT 보내면 실제 측정 시작*/

	fprintf(stderr, "READY pid=%d size=%luMiB\n",getpid(),size_mib);

	fflush(stderr);
	raise(SIGSTOP);

	/*실제 perf 측정 대상*/

	/* 컴파일러가 pointer chasing loop를 제거하지 못하도록 */

	volatile uint32_t *vnext = (volatile uint32_t *)next;


	struct timespec start;
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC_RAW,&start) != 0) {
		perror("clock_gettime");
		free(next);
		return 1;
	}

	uint64_t total_reads = 0;

	const uint64_t chunck_reads = 100000;

	for (;;) {
		for (uint64_t i = 0; i < chunck_reads; i++) {
			index = vnext[index];
		}

		total_reads += chunck_reads;

		if (clock_gettime(CLOCK_MONOTONIC_RAW,&now) != 0) {
			perror("clock_gettime");
			free(next);
			return 1;
		}

		if (elapsed_sec(&start,&now) >= (double)run_seconds) {
			break;
		}

	}

	const double sec = elapsed_sec(&start,&now);
	const double ns_per_read = (sec * 1000000000.0) / (double)total_reads;

	printf("size_mib,%lu\n", size_mib);
    printf("elapsed_sec,%.6f\n", sec);
    printf("total_reads,%llu\n",
           (unsigned long long)total_reads);
    printf("ns_per_read,%.3f\n", ns_per_read);


    free(next);

    return 0;
}


