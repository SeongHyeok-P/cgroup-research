#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>


/*
 * Ctrl+C받으면 종료
 */
static volatile sig_atomic_t stop_requested = 0;

/*
 * 계산 결과 제거 막기
 */
static volatile double final_result;

/*
 * 종료 처리
 */
static void handle_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

int main(int argc, char *argv[]) 
{
	/*
	 * ./mem_hog 384
	 *
	 * 세 배열을 합친 전체 데이터 크기 약 384 Mib
	 */
	if (argc != 2) {
		fprintf(stderr,"사용법: %s <전체_데이터크기_Mib>\n",argv[0]);

		return EXIT_FAILURE;
	}

	uint64_t total_mib = strtoull(argv[1],NULL,0);

	if (total_mib < 3) {
		fprintf(stderr, "전체 데이터 크기는 최소 3Mib이상\n");
		return EXIT_FAILURE;
	}

	/*
	 * 종료 등록
	 */
	if (signal(SIGINT, handle_signal) == SIG_ERR || signal(SIGTERM, handle_signal) == SIG_ERR) {
		perror("signal");
		return EXIT_FAILURE;
	}
	/*
         * 전체 크기를 byte로 변환
         */
        uint64_t total_bytes =
            total_mib * 1024ULL * 1024ULL;


        /*
         * 배열 A, B, C 세 개가 있으므로
         * 전체 메모리를 대략 3등분.
         */
        uint64_t bytes_per_array =
            total_bytes / 3ULL;

	size_t element_count = (size_t)(bytes_per_array / sizeof(double));

	if (element_count == 0) {
		fprintf(stderr, "계산된 배열 크기가 너무 작음\n");

		return EXIT_FAILURE;
	}
	double *a = NULL;
	double *b = NULL;
	double *c = NULL;

	/*
	 * 64bytes align
	 */
	if (posix_memalign((void **)&a,64,element_count * sizeof(double)) != 0) {
		fprintf(stderr, "배열 a 할당 실패\n");
		return EXIT_FAILURE;
	}
	if (posix_memalign((void **)&b,64,element_count * sizeof(double)) != 0) {
		fprintf(stderr, "배열 b 할당 실패\n");
		free(a);
		return EXIT_FAILURE;
	}
	if (posix_memalign((void **)&c,64,element_count * sizeof(double)) != 0) {
		fprintf(stderr, "배열 c 할당 실패\n");
		free(a);
		free(b);
		return EXIT_FAILURE;
	}

	/*
	 * 배열 초기화
	 */
	for (size_t i = 0; i < element_count; i++) {
		a[i] = 1.0;
		b[i] = 2.0 + (double)(i % 97) * 0.001;
		c[i] = 3.0 + (double)(i % 89) * 0.001;

	}

	fprintf(stderr, "REDY pid=%d total=%llu Mib" "each_array=%.1f Mib\n",(int)getpid(),
									     (unsigned long long)total_mib,
									     (double)(element_count * sizeof(double))
									     / (1024.0 * 1024.0));

	/*
	 * 모든 반복이 완전히 똑같은 계산으로 취급되는걸 피함
	 */
	double scalar = 1.000001;

	uint64_t passes = 0;

	while (!stop_requested) {

		for (size_t i = 0; i < element_count; i++) {
			a[i] = b[i] + scalar * c[i];
		}

		scalar += 0.0000001;

		if (scalar > 1.001) {
			scalar = 1.000001;
		}

		passes++;

		size_t sample_index = (size_t)((passes * 4099ULL) % element_count);

		final_result = a[sample_index];
	}

	fprintf(stderr, "STOP pid=%d passes=%llu result=%f\n",(int)getpid(),(unsigned long long)passes,final_result);

	free(a);
	free(b);
	free(c);

	return EXIT_SUCCESS;
}





