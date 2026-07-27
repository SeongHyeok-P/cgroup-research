#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include <limits.h>


/* 컴파일러가 최종 계산 결과를 어차피 안쓰잖아 하고 제거하지 못하도록 하는 변수 */
static volatile uint32_t final_result;

/* 접근 순서를 뒤섞기 위한 난수 생성기
*  배열 순서를 섞는 용도
*/
static uint64_t rng_state = 0x123456789abcdefULL;

static uint64_t next_random(void)
{
	uint64_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;

	rng_state = x;

	return x;
}

/*
 * 현재 시간을 나노초 단위로 반환
 * CLOCK_MONOTONIC_RAW:
 * 시스템 시각 변경이나 보정의 영향을 피하면서 
 * 경과 시간을 재기 위한 시계
 */
static uint64_t now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}

	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char *argv[])
{
	/*
	 * ./measure_mem 256 5000000 10
	 *
	 * 256 = 데이터 크기 256Mib
	 * 5000000 = 한 회차당 읽기 500만 번
	 * 10 = 총 10회 반복
	 */
	if (argc != 4) {
		fprintf(stderr, "사용법: %s <데이터크기_Mib> <회차당_읽기횟수> <회차수>\n", argv[0]);
		return EXIT_FAILURE;
	}

	uint64_t size_mib = strtoull(argv[1], NULL, 10);
	
	uint64_t  reads_per_round = strtoull(argv[2], NULL, 10);

	int rounds = atoi(argv[3]);

	/*
	 * 잘못 입력 방지
	 */
	if (size_mib == 0 || reads_per_round == 0 || rounds <= 0) {
		fprintf(stderr, "모든 인자는 1이상.\n");
		return EXIT_FAILURE;
	}

	/*
	 * Mib를 byte로 변환 
	 * 1 Mib = 1024 * 1024 bytes
	 */
	uint64_t bytes = size_mib * 1024ULL * 1024ULL;

	/*
	 * 배열 원소 하나는 uint32_t
	 * 4bytes
	 */
	uint64_t element_count64 = bytes / sizeof(uint32_t);

	/*
	 * 배열 위치 번호를 uint32_t로 저장
	 */
	if (element_count64 < 2 || element_count64 > UINT32_MAX) {
		fprintf(stderr, "데이터 크기가 프로그램 지원 범위 밖\n");

		return EXIT_FAILURE;
	}

	size_t element_count = (size_t)element_count64;

	/*
	 * next배열: 각 위치에서 다음에 어디로 갈지 저장 
	 * ex) next[10] = 532 -> next[532] = 91 -> ..
	 */
	uint32_t *next = NULL;

	if (posix_memalign((void **)&next,64,element_count * sizeof(uint32_t)) != 0) {
	       fprintf(stderr, "next 배열 메모리 할당 실패\n");
		return EXIT_FAILURE;
	}

	/*
	 * 배열 위치 순서를 섞기 위한 임시 배열
	 */
	uint32_t *order = malloc(element_count * sizeof(uint32_t));
	

	if (order == NULL) {

		fprintf(stderr, "order 메모리 할당 실패\n");

		free(next);
		return EXIT_FAILURE;
	}

	for (size_t i = 0; i < element_count; i++) {
		order[i] = (uint32_t)i;
	}

	/*
	 * 무작위로 순서 섞기
	 */
	for (size_t i = element_count - 1; i > 0; i--) {
		size_t j = (size_t)(next_random() % (i + 1));

		uint32_t temp = order[i];

		order[i] = order[j];
		order[j] = temp;
	}

	/*
	 * 고리 연결
	 */
	for (size_t i = 0; i < element_count - 1; i++) {
		next[order[i]] = order[i+1];
	}

	/*
	 * 마지막 위치 처음과 연결
	 */
	next[order[element_count - 1]] = order[0];

	free(order);

	/*
	 * 측정 전에 한번 실행
	 */
	uint32_t index = 0;

	for (size_t i = 0; i < element_count; i++) {
		index = next[index];
	}
	final_result = index;

	/*
	 * 출력
	 */
	printf("round,ns_per_read\n");
	/*
	 * 측정
	 */
	for (int round = 1; round <= rounds; round++) {
		uint64_t start = now_ns();

		for (uint64_t i = 0; i < reads_per_round; i++) {
			index = next[index];
		}

		uint64_t end = now_ns();

		final_result = index;

		double ns_per_read = (double)(end - start) / (double)reads_per_round;

		printf("%d, %.3f\n",round,ns_per_read);
	}
	free(next);

	return EXIT_SUCCESS;
}



