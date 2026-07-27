#include <stdio.h>
#include <stdint.h>
#include <cpuid.h>

static const char *cache_type_name(unsigned int type)
{
    switch (type) {
    case 1:
        return "Data";
    case 2:
        return "Instruction";
    case 3:
        return "Unified";
    default:
        return "Unknown";
    }
}

int main(void)
{
    unsigned int max_leaf =
        __get_cpuid_max(0, NULL);

    if (max_leaf < 4) {
        fprintf(stderr,
                "오류: CPUID leaf 4를 지원하지 않습니다.\n");
        return 1;
    }

    printf("CPUID Leaf 04H cache information\n");
    printf("================================\n");

    for (unsigned int subleaf = 0;
         subleaf < 32;
         subleaf++) {

        unsigned int eax;
        unsigned int ebx;
        unsigned int ecx;
        unsigned int edx;

        __cpuid_count(
            4,
            subleaf,
            eax,
            ebx,
            ecx,
            edx
        );

        /*
         * EAX[4:0]
         * 0이면 더 이상 캐시 없음
         */
        unsigned int cache_type =
            eax & 0x1f;

        if (cache_type == 0) {
            break;
        }

        /*
         * EAX[7:5]
         * 캐시 레벨
         */
        unsigned int cache_level =
            (eax >> 5) & 0x7;

        /*
         * EBX[11:0] + 1
         * 캐시 라인 크기
         */
        unsigned int line_size =
            (ebx & 0xfff) + 1;

        /*
         * EBX[21:12] + 1
         * physical line partitions
         */
        unsigned int partitions =
            ((ebx >> 12) & 0x3ff) + 1;

        /*
         * EBX[31:22] + 1
         * associativity ways
         */
        unsigned int ways =
            ((ebx >> 22) & 0x3ff) + 1;

        /*
         * ECX + 1
         * number of sets
         */
        uint64_t sets =
            (uint64_t)ecx + 1ULL;

        /*
         * 전체 캐시 크기
         */
        uint64_t size_bytes =
            (uint64_t)line_size
            * (uint64_t)partitions
            * (uint64_t)ways
            * sets;

        /*
         * EDX[1]
         * inclusive cache 여부
         */
        unsigned int inclusive =
            (edx >> 1) & 1U;

        /*
         * EDX[2]
         * complex cache indexing 여부
         */
        unsigned int complex_indexing =
            (edx >> 2) & 1U;

        printf("\n");
        printf("subleaf             : %u\n", subleaf);
        printf("cache level         : L%u\n", cache_level);
        printf("cache type          : %s\n",
               cache_type_name(cache_type));

        printf("line size           : %u bytes\n",
               line_size);

        printf("partitions          : %u\n",
               partitions);

        printf("ways                 : %u\n",
               ways);

        printf("sets                 : %llu\n",
               (unsigned long long)sets);

        printf("cache size           : %.2f MiB\n",
               (double)size_bytes
               / (1024.0 * 1024.0));

        printf("inclusive            : %s\n",
               inclusive ? "YES" : "NO");

        printf("complex indexing     : %s\n",
               complex_indexing ? "YES" : "NO");

        printf("raw EDX              : 0x%08x\n",
               edx);
    }

    return 0;
}
