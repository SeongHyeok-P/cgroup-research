#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>


/*
 * 프로그램 종료 요청을 받았는지 표시
 */

static volatile sig_atomic_t stop_requested = 0;

/*
 * 컴파일러가 계산 결과를 제거하지 못하도록
 */

static volatile uint64_t final_result;

/*
 * Ctrl + C 받으면 무한반복 종료
 */

static void handle_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

int main(void)
{
	/*
	 * 종료 신호 처리
	 */

	if (signal(SIGINT, handle_signal) == SIG_ERR || signal(SIGTERM, handle_signal) == SIG_ERR) {
		perror("signal");
		return EXIT_FAILURE;
	}

	/*
	 * 작은 변수 몇개만 사용 큰배열 X
	 */
	uint64_t x = 0x123456789abcdef0ULL;
	uint64_t y = 0xfedcba9876543211ULL;
	uint64_t z = 0x9e3779b97f4a7c15ULL;

	/*
	 * 종료 요청 올 때까지 계산 반복
	 */

	while (!stop_requested) {

		for (uint64_t i = 0; i < 10000000ULL; i++) {
			 x ^= x << 13;
	                 x ^= x >> 7;
	                 x ^= x << 17;

            		 y += x * 0x9e3779b97f4a7c15ULL;
	                 y ^= y >> 11;

 	                 z += y ^ x;
		         z = (z << 9) | (z >> 55);

		         x += z;
		}

		/*
		 * 계산 결과 저장
		 */
		final_result = x ^ y ^ z;
	}

	printf("final_result = %llu\n",(unsigned long long)final_result);

	return EXIT_SUCCESS;
}

