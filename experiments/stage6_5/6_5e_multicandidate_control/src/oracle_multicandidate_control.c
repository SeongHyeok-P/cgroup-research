#define _GNU_SOURCE

#include "calibration.h"
#include "dram_mapping.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CALIBRATION_PATH \
    "/run/protected-daemon/dram-map.json"

#define CACHELINE_BYTES 64U
#define MIB_BYTES (1024ULL * 1024ULL)

#define AGGRESSOR_STREAMS 16U
#define CANDIDATE_COUNT 5U
#define POOL_COUNT (CANDIDATE_COUNT + 1U)

#define VICTIM_CHUNK_READS 4096U
#define AGGRESSOR_CHUNK_ITERS 1024U

#define WARMUP_SECONDS 1.0

#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_SWAPPED (1ULL << 62)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1ULL)

enum condition {
    CONDITION_NONE = 0,
    CONDITION_UNCONTROLLED,
    CONDITION_LOWEST_ONE,
    CONDITION_HIGHEST_ONE,
    CONDITION_TOP2,
    CONDITION_ALL,
    CONDITION_COUNT
};

struct config {
    size_t pool_mib;
    size_t workset_mib;
    size_t group_classes;

    unsigned int seconds;

    int victim_cpu;
    int aggressor_cpu[CANDIDATE_COUNT];

    unsigned int throttle_quota_us;
    unsigned int throttle_period_us;

    uint64_t seed;

    bool self_test;

    enum condition order[CONDITION_COUNT];
};

struct memory_pool {
    const char *name;

    unsigned char *base;

    size_t bytes;
    size_t page_size;
    size_t page_count;

    uint64_t *pfns;
};

struct line_set {
    struct memory_pool *pool;

    uintptr_t *addresses;
    size_t line_count;
};

struct worker_result {
    int ok;

    double elapsed_sec;

    uint64_t operations;
};

struct condition_result {
    struct worker_result victim;
    struct worker_result background[CANDIDATE_COUNT];

    bool has_background;
};

struct cgroup_context {
    bool ready;

    char run_path[PATH_MAX];
    char background_path[CANDIDATE_COUNT][PATH_MAX];
};

static volatile uintptr_t worker_sink;

static uint64_t rng_next(uint64_t *state)
{
    uint64_t x;

    if (state == NULL) {
        return 0U;
    }

    x = *state;

    if (x == 0U) {
        x = 0x9e3779b97f4a7c15ULL;
    }

    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;

    *state = x;

    return x * 2685821657736338717ULL;
}

static void shuffle_size_t(
    size_t *values,
    size_t count,
    uint64_t seed)
{
    size_t i;
    uint64_t state = seed;

    if (values == NULL || count < 2U) {
        return;
    }

    for (i = count - 1U; i > 0U; --i) {
        size_t j;
        size_t tmp;

        j = (size_t)(rng_next(&state) % (uint64_t)(i + 1U));

        tmp = values[i];
        values[i] = values[j];
        values[j] = tmp;
    }
}

static const char *condition_name(enum condition condition)
{
    switch (condition) {
    case CONDITION_NONE:
        return "none";
    case CONDITION_UNCONTROLLED:
        return "uncontrolled";
    case CONDITION_LOWEST_ONE:
        return "lowest";
    case CONDITION_HIGHEST_ONE:
        return "highest";
    case CONDITION_TOP2:
        return "top2";
    case CONDITION_ALL:
        return "all";
    default:
        return "unknown";
    }
}

static int condition_from_name(
    const char *text,
    enum condition *out)
{
    if (text == NULL || out == NULL) {
        return -1;
    }

    if (strcmp(text, "none") == 0) {
        *out = CONDITION_NONE;
    } else if (strcmp(text, "uncontrolled") == 0) {
        *out = CONDITION_UNCONTROLLED;
    } else if (strcmp(text, "lowest") == 0) {
        *out = CONDITION_LOWEST_ONE;
    } else if (strcmp(text, "highest") == 0) {
        *out = CONDITION_HIGHEST_ONE;
    } else if (strcmp(text, "top2") == 0) {
        *out = CONDITION_TOP2;
    } else if (strcmp(text, "all") == 0) {
        *out = CONDITION_ALL;
    } else {
        return -1;
    }

    return 0;
}

static int parse_order(
    const char *text,
    enum condition order[CONDITION_COUNT])
{
    char buffer[192];
    char *save = NULL;
    char *token;
    bool seen[CONDITION_COUNT] = {false};
    size_t index = 0U;

    if (text == NULL || order == NULL ||
        strlen(text) >= sizeof(buffer)) {
        return -1;
    }

    (void)snprintf(buffer, sizeof(buffer), "%s", text);
    token = strtok_r(buffer, ",", &save);

    while (token != NULL) {
        enum condition condition;

        if (index >= CONDITION_COUNT ||
            condition_from_name(token, &condition) != 0 ||
            seen[condition]) {
            return -1;
        }

        seen[condition] = true;
        order[index++] = condition;
        token = strtok_r(NULL, ",", &save);
    }

    return index == CONDITION_COUNT ? 0 : -1;
}

static int parse_size(
    const char *text,
    size_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    value = strtoull(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value > (unsigned long long)SIZE_MAX) {
        return -1;
    }

    *out = (size_t)value;

    return 0;
}

static int parse_uint(
    const char *text,
    unsigned int *out)
{
    size_t value;

    if (parse_size(text, &value) != 0 ||
        value > (size_t)UINT_MAX) {
        return -1;
    }

    *out = (unsigned int)value;

    return 0;
}

static int parse_int(
    const char *text,
    int *out)
{
    char *end = NULL;
    long value;

    if (text == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value < INT_MIN ||
        value > INT_MAX) {
        return -1;
    }

    *out = (int)value;

    return 0;
}

static int parse_u64(
    const char *text,
    uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    value = strtoull(text, &end, 0);

    if (errno != 0 ||
        end == text ||
        *end != '\0') {
        return -1;
    }

    *out = (uint64_t)value;

    return 0;
}

static void config_default(struct config *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->pool_mib = 2048U;
    cfg->workset_mib = 64U;
    cfg->group_classes = 16U;
    cfg->seconds = 10U;

    cfg->victim_cpu = 14;
    cfg->aggressor_cpu[0] = 12; /* 0% overlap */
    cfg->aggressor_cpu[1] = 10; /* 25% overlap */
    cfg->aggressor_cpu[2] = 8;  /* 50% overlap */
    cfg->aggressor_cpu[3] = 6;  /* 75% overlap */
    cfg->aggressor_cpu[4] = 4;  /* 100% overlap */

    cfg->throttle_quota_us = 50000U;
    cfg->throttle_period_us = 100000U;

    cfg->seed = 20260814ULL;

    cfg->order[0] = CONDITION_NONE;
    cfg->order[1] = CONDITION_UNCONTROLLED;
    cfg->order[2] = CONDITION_LOWEST_ONE;
    cfg->order[3] = CONDITION_HIGHEST_ONE;
    cfg->order[4] = CONDITION_TOP2;
    cfg->order[5] = CONDITION_ALL;
}

static void config_self_test(struct config *cfg)
{
    if (cfg == NULL) {
        return;
    }

    cfg->self_test = true;
    cfg->pool_mib = 512U;
    cfg->workset_mib = 8U;
    cfg->group_classes = 8U;
    cfg->seconds = 2U;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --self-test\n"
            "\n"
            "  %s "
            "[--pool-mib N] "
            "[--workset-mib N] "
            "[--classes N] "
            "[--seconds N] "
            "[--victim-cpu N] "
            "[--o0-cpu N] [--o25-cpu N] [--o50-cpu N] "
            "[--o75-cpu N] [--o100-cpu N] "
            "[--throttle-quota-us N] "
            "[--throttle-period-us N] "
            "[--seed N] "
            "[--order none,uncontrolled,lowest,highest,top2,all]\n"
            "\n"
            "Experiment E: all five background candidates run together.\n"
            "lowest  = throttle only the 0%%-overlap candidate.\n"
            "highest = throttle only the 100%%-overlap candidate.\n"
            "top2    = throttle the 75%% and 100%% candidates.\n"
            "all     = throttle all five candidates.\n",
            program,
            program);
}

static int parse_args(
    int argc,
    char **argv,
    struct config *cfg)
{
    int i;
    bool self_test = false;
    size_t a;
    size_t b;

    if (cfg == NULL) {
        return -1;
    }

    config_default(cfg);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0) {
            self_test = true;
        }
    }

    if (self_test) {
        config_self_test(cfg);
    }

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0) {
            continue;
        }

        if (strcmp(argv[i], "--pool-mib") == 0) {
            if (++i >= argc || parse_size(argv[i], &cfg->pool_mib) != 0 ||
                cfg->pool_mib == 0U) return -1;
            continue;
        }
        if (strcmp(argv[i], "--workset-mib") == 0) {
            if (++i >= argc || parse_size(argv[i], &cfg->workset_mib) != 0 ||
                cfg->workset_mib == 0U) return -1;
            continue;
        }
        if (strcmp(argv[i], "--classes") == 0) {
            if (++i >= argc || parse_size(argv[i], &cfg->group_classes) != 0 ||
                cfg->group_classes == 0U) return -1;
            continue;
        }
        if (strcmp(argv[i], "--seconds") == 0) {
            if (++i >= argc || parse_uint(argv[i], &cfg->seconds) != 0 ||
                cfg->seconds == 0U) return -1;
            continue;
        }
        if (strcmp(argv[i], "--victim-cpu") == 0) {
            if (++i >= argc || parse_int(argv[i], &cfg->victim_cpu) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--o0-cpu") == 0) {
            if (++i >= argc || parse_int(argv[i], &cfg->aggressor_cpu[0]) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--o25-cpu") == 0) {
            if (++i >= argc || parse_int(argv[i], &cfg->aggressor_cpu[1]) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--o50-cpu") == 0) {
            if (++i >= argc || parse_int(argv[i], &cfg->aggressor_cpu[2]) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--o75-cpu") == 0) {
            if (++i >= argc || parse_int(argv[i], &cfg->aggressor_cpu[3]) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--o100-cpu") == 0) {
            if (++i >= argc || parse_int(argv[i], &cfg->aggressor_cpu[4]) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--throttle-quota-us") == 0) {
            if (++i >= argc || parse_uint(argv[i], &cfg->throttle_quota_us) != 0 ||
                cfg->throttle_quota_us == 0U) return -1;
            continue;
        }
        if (strcmp(argv[i], "--throttle-period-us") == 0) {
            if (++i >= argc || parse_uint(argv[i], &cfg->throttle_period_us) != 0 ||
                cfg->throttle_period_us == 0U) return -1;
            continue;
        }
        if (strcmp(argv[i], "--seed") == 0) {
            if (++i >= argc || parse_u64(argv[i], &cfg->seed) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--order") == 0) {
            if (++i >= argc || parse_order(argv[i], cfg->order) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        return -1;
    }

    if (cfg->group_classes % 4U != 0U ||
        cfg->throttle_quota_us > cfg->throttle_period_us) {
        return -1;
    }

    for (a = 0U; a < CANDIDATE_COUNT; ++a) {
        if (cfg->aggressor_cpu[a] == cfg->victim_cpu) {
            return -1;
        }
        for (b = a + 1U; b < CANDIDATE_COUNT; ++b) {
            if (cfg->aggressor_cpu[a] == cfg->aggressor_cpu[b]) {
                return -1;
            }
        }
    }

    return 0;
}

static double elapsed_sec(
    const struct timespec *start,
    const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) /
               1000000000.0;
}

static int pin_to_cpu(int cpu)
{
    cpu_set_t set;

    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        return -1;
    }

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return -1;
    }

    return 0;
}

static int load_mapping(
    struct calibration_result *calibration,
    struct dram_mapping *mapping)
{
    int rc;
    size_t i;

    if (calibration == NULL || mapping == NULL) {
        return -1;
    }

    calibration_result_reset(calibration);
    dram_mapping_reset(mapping);

    rc = calibration_load_result(
        DEFAULT_CALIBRATION_PATH,
        calibration);

    if (rc != CALIBRATION_OK) {
        fprintf(stderr,
                "[ERROR] calibration_load_result: %s\n",
                calibration_strerror(rc));
        return -1;
    }

    rc = dram_mapping_init_from_calibration(
        mapping,
        calibration);

    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr,
                "[ERROR] dram_mapping_init_from_calibration: %s\n",
                dram_mapping_strerror(rc));
        return -1;
    }

    printf("[MAPPING] calibration=%s\n",
           DEFAULT_CALIBRATION_PATH);
    printf("[MAPPING] mask_set_id=%s\n",
           mapping->mask_set_id);
    printf("[MAPPING] mask_count=%zu\n",
           mapping->mask_count);

    for (i = 0U; i < mapping->mask_count; ++i) {
        printf("[MAPPING] mask[%zu]=0x%016" PRIx64 "\n",
               i,
               mapping->masks[i]);
    }

    return 0;
}

static int read_one_pfn(
    int pagemap_fd,
    uintptr_t virtual_address,
    size_t page_size,
    uint64_t *pfn_out)
{
    uint64_t entry;
    uint64_t vpn;
    off_t offset;
    ssize_t n;

    if (pfn_out == NULL || page_size == 0U) {
        return -1;
    }

    vpn = (uint64_t)(
        virtual_address / (uintptr_t)page_size);

    offset = (off_t)(vpn * sizeof(entry));

    n = pread(
        pagemap_fd,
        &entry,
        sizeof(entry),
        offset);

    if (n != (ssize_t)sizeof(entry)) {
        return -1;
    }

    if ((entry & PAGEMAP_PRESENT) == 0U) {
        return -2;
    }

    if ((entry & PAGEMAP_SWAPPED) != 0U) {
        return -3;
    }

    *pfn_out = entry & PAGEMAP_PFN_MASK;

    if (*pfn_out == 0U) {
        return -4;
    }

    return 0;
}

static void pool_destroy(struct memory_pool *pool)
{
    if (pool == NULL) {
        return;
    }

    free(pool->pfns);
    pool->pfns = NULL;

    if (pool->base != NULL &&
        pool->base != MAP_FAILED) {
        (void)munmap(pool->base, pool->bytes);
    }

    memset(pool, 0, sizeof(*pool));
}

static int pool_create(
    struct memory_pool *pool,
    const char *name,
    size_t pool_mib,
    size_t page_size,
    uint64_t fault_seed)
{
    int pagemap_fd = -1;
    volatile unsigned char *fault_ptr;
    size_t i;

    uint64_t valid = 0U;
    uint64_t not_present = 0U;
    uint64_t swapped = 0U;
    uint64_t unavailable = 0U;

    if (pool == NULL ||
        name == NULL ||
        pool_mib == 0U ||
        page_size == 0U) {
        return -1;
    }

    memset(pool, 0, sizeof(*pool));

    if (pool_mib >
        SIZE_MAX / (size_t)MIB_BYTES) {
        return -1;
    }

    pool->name = name;
    pool->bytes =
        pool_mib * (size_t)MIB_BYTES;
    pool->page_size = page_size;

    if (pool->bytes % page_size != 0U) {
        return -1;
    }

    pool->page_count =
        pool->bytes / page_size;

    pool->base = mmap(
        NULL,
        pool->bytes,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS,
        -1,
        0);

    if (pool->base == MAP_FAILED) {
        pool->base = NULL;
        perror("mmap");
        return -1;
    }

    if (madvise(
            pool->base,
            pool->bytes,
            MADV_NOHUGEPAGE) != 0) {
        perror("madvise(MADV_NOHUGEPAGE)");
        pool_destroy(pool);
        return -1;
    }

    pool->pfns = calloc(
        pool->page_count,
        sizeof(pool->pfns[0]));

    if (pool->pfns == NULL) {
        pool_destroy(pool);
        return -1;
    }

    /*
     * Allocate real physical pages before pagemap classification.
     *
     * MAP_SHARED is deliberate:
     * children never obtain private COW PFNs later.
     */
    fault_ptr =
        (volatile unsigned char *)pool->base;

    for (i = 0U; i < pool->page_count; ++i) {
        fault_ptr[i * page_size] =
            (unsigned char)(
                (i ^ (size_t)fault_seed) & 0xffU);
    }

    pagemap_fd = open(
        "/proc/self/pagemap",
        O_RDONLY | O_CLOEXEC);

    if (pagemap_fd < 0) {
        perror("open(/proc/self/pagemap)");
        pool_destroy(pool);
        return -1;
    }

    for (i = 0U; i < pool->page_count; ++i) {
        uintptr_t va;
        uint64_t pfn;
        int rc;

        va = (uintptr_t)(
            pool->base + i * page_size);

        rc = read_one_pfn(
            pagemap_fd,
            va,
            page_size,
            &pfn);

        if (rc == 0) {
            pool->pfns[i] = pfn;
            ++valid;
        } else if (rc == -2) {
            ++not_present;
        } else if (rc == -3) {
            ++swapped;
        } else {
            ++unavailable;
        }
    }

    (void)close(pagemap_fd);

    printf("[POOL:%s] mmap=%p bytes=%zu pages=%zu\n",
           pool->name,
           (void *)pool->base,
           pool->bytes,
           pool->page_count);

    printf("[PAGEMAP:%s] valid=%" PRIu64
           " not_present=%" PRIu64
           " swapped=%" PRIu64
           " pfn_unavailable=%" PRIu64 "\n",
           pool->name,
           valid,
           not_present,
           swapped,
           unavailable);

    if (valid != (uint64_t)pool->page_count) {
        fprintf(stderr,
                "[ERROR] pool %s does not have 100%% valid PFNs\n",
                pool->name);
        pool_destroy(pool);
        return -1;
    }

    return 0;
}

static int class_space_from_mapping(
    const struct dram_mapping *mapping,
    size_t *class_space_out)
{
    uint64_t space64;

    if (mapping == NULL ||
        class_space_out == NULL ||
        !dram_mapping_is_ready(mapping)) {
        return -1;
    }

    /*
     * This oracle experiment only needs the recovered
     * class signature space.  Current machine uses 7 masks.
     *
     * Reject pathological huge spaces rather than allocating
     * unbounded histogram arrays.
     */
    if (mapping->mask_count > 20U) {
        fprintf(stderr,
                "[ERROR] mask_count=%zu is too large for oracle histogram\n",
                mapping->mask_count);
        return -1;
    }

    space64 = 1ULL << mapping->mask_count;

    if (space64 > (uint64_t)SIZE_MAX) {
        return -1;
    }

    *class_space_out = (size_t)space64;

    return 0;
}

static int build_offset_classes(
    const struct dram_mapping *mapping,
    size_t page_size,
    uint64_t **offset_classes_out,
    size_t *lines_per_page_out)
{
    size_t lines_per_page;
    uint64_t *offset_classes;
    size_t line;
    size_t i;

    if (mapping == NULL ||
        offset_classes_out == NULL ||
        lines_per_page_out == NULL) {
        return -1;
    }

    if (page_size % CACHELINE_BYTES != 0U) {
        return -1;
    }

    lines_per_page =
        page_size / CACHELINE_BYTES;

    offset_classes = calloc(
        lines_per_page,
        sizeof(offset_classes[0]));

    if (offset_classes == NULL) {
        return -1;
    }

    printf("[OFFSET] page_size=%zu cacheline=%u lines_per_page=%zu\n",
           page_size,
           CACHELINE_BYTES,
           lines_per_page);

    for (i = 0U; i < mapping->mask_count; ++i) {
        uint64_t offset_bits =
            mapping->masks[i] &
            ((uint64_t)page_size - 1ULL);

        printf("[OFFSET] mask[%zu]_page_offset_bits="
               "0x%03" PRIx64 "\n",
               i,
               offset_bits);
    }

    for (line = 0U;
         line < lines_per_page;
         ++line) {
        uint64_t offset =
            (uint64_t)(
                line * CACHELINE_BYTES);

        offset_classes[line] =
            dram_mapping_bank_class_fast(
                mapping,
                offset);
    }

    *offset_classes_out = offset_classes;
    *lines_per_page_out = lines_per_page;

    return 0;
}

static int count_line_classes(
    const struct memory_pool *pool,
    const struct dram_mapping *mapping,
    const uint64_t *offset_classes,
    size_t lines_per_page,
    uint64_t *counts,
    size_t class_space)
{
    size_t page;
    size_t line;

    if (pool == NULL ||
        mapping == NULL ||
        offset_classes == NULL ||
        counts == NULL) {
        return -1;
    }

    memset(
        counts,
        0,
        class_space * sizeof(counts[0]));

    for (page = 0U;
         page < pool->page_count;
         ++page) {
        uint64_t base_pa;
        uint64_t base_class;

        base_pa =
            pool->pfns[page] *
            (uint64_t)pool->page_size;

        base_class =
            dram_mapping_bank_class_fast(
                mapping,
                base_pa);

        /*
         * page base is 4 KiB aligned.
         *
         * parity(base | offset)
         *   = parity(base) XOR parity(offset)
         *
         * because base has all page-offset bits zero.
         */
        for (line = 0U;
             line < lines_per_page;
             ++line) {
            uint64_t cls =
                base_class ^
                offset_classes[line];

            if (cls >= (uint64_t)class_space) {
                return -1;
            }

            ++counts[cls];
        }
    }

    return 0;
}

static void shuffle_u64(
    uint64_t *values,
    size_t count,
    uint64_t seed)
{
    size_t i;
    uint64_t state = seed;

    if (values == NULL || count < 2U) {
        return;
    }

    for (i = count - 1U; i > 0U; --i) {
        size_t j;
        uint64_t tmp;

        j = (size_t)(
            rng_next(&state) %
            (uint64_t)(i + 1U));

        tmp = values[i];
        values[i] = values[j];
        values[j] = tmp;
    }
}

static int choose_class_sets(
    const uint64_t *const pool_counts[POOL_COUNT],
    size_t class_space,
    size_t group_classes,
    size_t quota_lines,
    uint64_t seed,
    uint64_t *victim_classes,
    uint64_t *disjoint_classes)
{
    uint64_t *candidates;
    size_t candidate_count = 0U;
    size_t cls;
    size_t p;
    size_t i;

    if (pool_counts == NULL ||
        victim_classes == NULL ||
        disjoint_classes == NULL ||
        group_classes == 0U ||
        quota_lines == 0U) {
        return -1;
    }

    for (p = 0U; p < POOL_COUNT; ++p) {
        if (pool_counts[p] == NULL) {
            return -1;
        }
    }

    candidates = calloc(class_space, sizeof(candidates[0]));
    if (candidates == NULL) {
        return -1;
    }

    /* Every selected class must have enough lines in all six pools. */
    for (cls = 0U; cls < class_space; ++cls) {
        bool eligible = true;
        for (p = 0U; p < POOL_COUNT; ++p) {
            if (pool_counts[p][cls] < (uint64_t)quota_lines) {
                eligible = false;
                break;
            }
        }
        if (eligible) {
            candidates[candidate_count++] = (uint64_t)cls;
        }
    }

    printf("[SELECT] common_eligible_classes=%zu\n", candidate_count);

    if (candidate_count < group_classes * 2U) {
        fprintf(stderr,
                "[ERROR] need at least %zu common eligible classes, have %zu\n",
                group_classes * 2U,
                candidate_count);
        free(candidates);
        return -1;
    }

    shuffle_u64(candidates, candidate_count, seed);

    for (i = 0U; i < group_classes; ++i) {
        victim_classes[i] = candidates[i];
        disjoint_classes[i] = candidates[group_classes + i];
    }

    free(candidates);
    return 0;
}

static int collect_line_set(
    struct line_set *set,
    struct memory_pool *pool,
    const struct dram_mapping *mapping,
    const uint64_t *offset_classes,
    size_t lines_per_page,
    size_t class_space,
    const uint64_t *classes,
    size_t class_count,
    size_t quota_lines,
    uint64_t seed)
{
    int *class_to_slot = NULL;
    size_t *filled = NULL;
    size_t *page_order = NULL;
    size_t *line_order = NULL;

    size_t page_pos;
    size_t line_pos;
    size_t slot;
    size_t remaining;

    if (set == NULL ||
        pool == NULL ||
        mapping == NULL ||
        offset_classes == NULL ||
        classes == NULL ||
        class_count == 0U ||
        quota_lines == 0U) {
        return -1;
    }

    memset(set, 0, sizeof(*set));

    if (class_count >
        SIZE_MAX / quota_lines) {
        return -1;
    }

    set->line_count =
        class_count * quota_lines;

    set->pool = pool;

    set->addresses = calloc(
        set->line_count,
        sizeof(set->addresses[0]));

    class_to_slot = malloc(
        class_space *
        sizeof(class_to_slot[0]));

    filled = calloc(
        class_count,
        sizeof(filled[0]));

    page_order = malloc(
        pool->page_count *
        sizeof(page_order[0]));

    line_order = malloc(
        lines_per_page *
        sizeof(line_order[0]));

    if (set->addresses == NULL ||
        class_to_slot == NULL ||
        filled == NULL ||
        page_order == NULL ||
        line_order == NULL) {
        goto fail;
    }

    for (slot = 0U;
         slot < class_space;
         ++slot) {
        class_to_slot[slot] = -1;
    }

    for (slot = 0U;
         slot < class_count;
         ++slot) {
        if (classes[slot] >=
            (uint64_t)class_space) {
            goto fail;
        }

        class_to_slot[classes[slot]] =
            (int)slot;
    }

    for (page_pos = 0U;
         page_pos < pool->page_count;
         ++page_pos) {
        page_order[page_pos] = page_pos;
    }

    for (line_pos = 0U;
         line_pos < lines_per_page;
         ++line_pos) {
        line_order[line_pos] = line_pos;
    }

    shuffle_size_t(
        page_order,
        pool->page_count,
        seed ^ 0x9e3779b97f4a7c15ULL);

    shuffle_size_t(
        line_order,
        lines_per_page,
        seed ^ 0xd1b54a32d192ed03ULL);

    remaining = class_count;

    for (page_pos = 0U;
         page_pos < pool->page_count &&
         remaining > 0U;
         ++page_pos) {
        size_t page =
            page_order[page_pos];

        uint64_t base_pa =
            pool->pfns[page] *
            (uint64_t)pool->page_size;

        uint64_t base_class =
            dram_mapping_bank_class_fast(
                mapping,
                base_pa);

        for (line_pos = 0U;
             line_pos < lines_per_page;
             ++line_pos) {
            size_t line =
                line_order[line_pos];

            uint64_t cls =
                base_class ^
                offset_classes[line];

            int selected_slot;

            if (cls >=
                (uint64_t)class_space) {
                goto fail;
            }

            selected_slot =
                class_to_slot[cls];

            if (selected_slot < 0) {
                continue;
            }

            slot =
                (size_t)selected_slot;

            if (filled[slot] >=
                quota_lines) {
                continue;
            }

            set->addresses[
                slot * quota_lines +
                filled[slot]] =
                (uintptr_t)(
                    pool->base +
                    page *
                        pool->page_size +
                    line *
                        CACHELINE_BYTES);

            ++filled[slot];

            if (filled[slot] ==
                quota_lines) {
                --remaining;
            }
        }
    }

    if (remaining != 0U) {
        fprintf(stderr,
                "[ERROR] failed to collect enough lines from pool %s\n",
                pool->name);
        goto fail;
    }

    free(class_to_slot);
    free(filled);
    free(page_order);
    free(line_order);

    return 0;

fail:
    free(class_to_slot);
    free(filled);
    free(page_order);
    free(line_order);

    free(set->addresses);
    memset(set, 0, sizeof(*set));

    return -1;
}

static void line_set_destroy(
    struct line_set *set)
{
    if (set == NULL) {
        return;
    }

    free(set->addresses);
    memset(set, 0, sizeof(*set));
}

static int address_bank_class(
    const struct line_set *set,
    const struct dram_mapping *mapping,
    uintptr_t address,
    uint64_t *class_out)
{
    uintptr_t base;
    uintptr_t delta;

    size_t page;
    size_t offset;

    uint64_t pa;

    if (set == NULL ||
        set->pool == NULL ||
        mapping == NULL ||
        class_out == NULL) {
        return -1;
    }

    base =
        (uintptr_t)set->pool->base;

    if (address < base ||
        address >=
            base + set->pool->bytes) {
        return -1;
    }

    delta = address - base;

    page =
        (size_t)(
            delta /
            set->pool->page_size);

    offset =
        (size_t)(
            delta %
            set->pool->page_size);

    if (offset % CACHELINE_BYTES != 0U) {
        return -1;
    }

    pa =
        set->pool->pfns[page] *
            (uint64_t)set->pool->page_size +
        (uint64_t)offset;

    *class_out =
        dram_mapping_bank_class_fast(
            mapping,
            pa);

    return 0;
}

static int build_histogram(
    const struct line_set *set,
    const struct dram_mapping *mapping,
    uint64_t *histogram,
    size_t class_space)
{
    size_t i;

    if (set == NULL ||
        mapping == NULL ||
        histogram == NULL) {
        return -1;
    }

    memset(
        histogram,
        0,
        class_space *
            sizeof(histogram[0]));

    for (i = 0U;
         i < set->line_count;
         ++i) {
        uint64_t cls;

        if (address_bank_class(
                set,
                mapping,
                set->addresses[i],
                &cls) != 0) {
            return -1;
        }

        if (cls >=
            (uint64_t)class_space) {
            return -1;
        }

        ++histogram[cls];
    }

    return 0;
}

static double histogram_jaccard(
    const uint64_t *a,
    const uint64_t *b,
    size_t class_space,
    uint64_t *intersection_out,
    uint64_t *union_out)
{
    uint64_t intersection = 0U;
    uint64_t union_count = 0U;

    size_t i;

    for (i = 0U;
         i < class_space;
         ++i) {
        uint64_t minimum =
            a[i] < b[i] ?
                a[i] :
                b[i];

        uint64_t maximum =
            a[i] > b[i] ?
                a[i] :
                b[i];

        intersection += minimum;
        union_count += maximum;
    }

    if (intersection_out != NULL) {
        *intersection_out =
            intersection;
    }

    if (union_out != NULL) {
        *union_out =
            union_count;
    }

    if (union_count == 0U) {
        return 0.0;
    }

    return (double)intersection /
           (double)union_count;
}

static size_t histogram_unique_classes(
    const uint64_t *histogram,
    size_t class_space)
{
    size_t unique = 0U;
    size_t i;

    for (i = 0U;
         i < class_space;
         ++i) {
        if (histogram[i] != 0U) {
            ++unique;
        }
    }

    return unique;
}


static size_t histogram_shared_classes(
    const uint64_t *a,
    const uint64_t *b,
    size_t class_space)
{
    size_t shared = 0U;
    size_t i;

    for (i = 0U; i < class_space; ++i) {
        if (a[i] != 0U && b[i] != 0U) {
            ++shared;
        }
    }
    return shared;
}

static size_t candidate_shared_classes(
    size_t candidate,
    size_t group_classes)
{
    switch (candidate) {
    case 0U: return 0U;
    case 1U: return group_classes / 4U;
    case 2U: return group_classes / 2U;
    case 3U: return group_classes * 3U / 4U;
    case 4U: return group_classes;
    default: return 0U;
    }
}

static const char *candidate_name(size_t candidate)
{
    static const char *const names[CANDIDATE_COUNT] = {
        "o0", "o25", "o50", "o75", "o100"
    };
    return candidate < CANDIDATE_COUNT ? names[candidate] : "unknown";
}

static int make_permutation(
    size_t count,
    uint64_t seed,
    size_t **out)
{
    size_t *permutation;
    size_t i;

    if (count == 0U ||
        out == NULL) {
        return -1;
    }

    permutation = malloc(
        count *
        sizeof(permutation[0]));

    if (permutation == NULL) {
        return -1;
    }

    for (i = 0U;
         i < count;
         ++i) {
        permutation[i] = i;
    }

    shuffle_size_t(
        permutation,
        count,
        seed);

    *out = permutation;

    return 0;
}

static int build_single_chain(
    const struct line_set *set,
    const size_t *permutation,
    uintptr_t *root_out)
{
    size_t i;

    if (set == NULL ||
        permutation == NULL ||
        root_out == NULL ||
        set->line_count == 0U) {
        return -1;
    }

    for (i = 0U;
         i < set->line_count;
         ++i) {
        uintptr_t current;
        uintptr_t next;

        current =
            set->addresses[
                permutation[i]];

        next =
            set->addresses[
                permutation[
                    (i + 1U) %
                    set->line_count]];

        *(uintptr_t *)current =
            next;
    }

    *root_out =
        set->addresses[
            permutation[0]];

    return 0;
}

static int build_multi_chain(
    const struct line_set *set,
    const size_t *permutation,
    size_t stream_count,
    uintptr_t *roots)
{
    size_t stream;

    if (set == NULL ||
        permutation == NULL ||
        roots == NULL ||
        stream_count == 0U ||
        set->line_count < stream_count) {
        return -1;
    }

    for (stream = 0U;
         stream < stream_count;
         ++stream) {
        size_t position;
        uintptr_t first;
        uintptr_t previous;

        first =
            set->addresses[
                permutation[stream]];

        roots[stream] = first;
        previous = first;

        position =
            stream + stream_count;

        while (position <
               set->line_count) {
            uintptr_t current =
                set->addresses[
                    permutation[position]];

            *(uintptr_t *)previous =
                current;

            previous = current;

            position +=
                stream_count;
        }

        *(uintptr_t *)previous =
            first;
    }

    return 0;
}

static int victim_loop(
    uintptr_t root,
    double seconds,
    struct worker_result *result)
{
    struct timespec start;
    struct timespec now;

    uintptr_t pointer = root;

    uint64_t operations = 0U;

    if (root == 0U ||
        result == NULL ||
        seconds <= 0.0) {
        return -1;
    }

    memset(result, 0, sizeof(*result));

    if (clock_gettime(
            CLOCK_MONOTONIC_RAW,
            &start) != 0) {
        return -1;
    }

    for (;;) {
        unsigned int i;

        for (i = 0U;
             i < VICTIM_CHUNK_READS;
             ++i) {
            pointer =
                *(volatile uintptr_t *)
                    (uintptr_t)pointer;
        }

        operations +=
            VICTIM_CHUNK_READS;

        if (clock_gettime(
                CLOCK_MONOTONIC_RAW,
                &now) != 0) {
            return -1;
        }

        if (elapsed_sec(
                &start,
                &now) >= seconds) {
            break;
        }
    }

    worker_sink ^= pointer;

    result->ok = 1;
    result->elapsed_sec =
        elapsed_sec(&start, &now);
    result->operations =
        operations;

    return 0;
}

static int aggressor_loop(
    const uintptr_t roots[AGGRESSOR_STREAMS],
    double seconds,
    struct worker_result *result)
{
    struct timespec start;
    struct timespec now;

    uintptr_t pointers[AGGRESSOR_STREAMS];

    uint64_t operations = 0U;

    size_t stream;

    if (roots == NULL ||
        result == NULL ||
        seconds <= 0.0) {
        return -1;
    }

    memcpy(
        pointers,
        roots,
        sizeof(pointers));

    memset(result, 0, sizeof(*result));

    if (clock_gettime(
            CLOCK_MONOTONIC_RAW,
            &start) != 0) {
        return -1;
    }

    for (;;) {
        unsigned int iteration;

        for (iteration = 0U;
             iteration <
                 AGGRESSOR_CHUNK_ITERS;
             ++iteration) {
            for (stream = 0U;
                 stream <
                     AGGRESSOR_STREAMS;
                 ++stream) {
                pointers[stream] =
                    *(volatile uintptr_t *)
                        (uintptr_t)
                            pointers[stream];
            }
        }

        operations +=
            (uint64_t)
                AGGRESSOR_CHUNK_ITERS *
            (uint64_t)
                AGGRESSOR_STREAMS;

        if (clock_gettime(
                CLOCK_MONOTONIC_RAW,
                &now) != 0) {
            return -1;
        }

        if (elapsed_sec(
                &start,
                &now) >= seconds) {
            break;
        }
    }

    for (stream = 0U;
         stream < AGGRESSOR_STREAMS;
         ++stream) {
        worker_sink ^=
            pointers[stream];
    }

    result->ok = 1;
    result->elapsed_sec =
        elapsed_sec(&start, &now);
    result->operations =
        operations;

    return 0;
}

static int write_full(
    int fd,
    const void *buffer,
    size_t bytes)
{
    const unsigned char *p =
        buffer;

    while (bytes > 0U) {
        ssize_t n =
            write(fd, p, bytes);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (n == 0) {
            return -1;
        }

        p += (size_t)n;
        bytes -= (size_t)n;
    }

    return 0;
}

static int read_full(
    int fd,
    void *buffer,
    size_t bytes)
{
    unsigned char *p =
        buffer;

    while (bytes > 0U) {
        ssize_t n =
            read(fd, p, bytes);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (n == 0) {
            return -1;
        }

        p += (size_t)n;
        bytes -= (size_t)n;
    }

    return 0;
}

static int read_text_file(
    const char *path,
    char *buffer,
    size_t buffer_size)
{
    int fd;
    ssize_t n;

    if (path == NULL ||
        buffer == NULL ||
        buffer_size < 2U) {
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    n = read(fd, buffer, buffer_size - 1U);
    (void)close(fd);

    if (n < 0) {
        return -1;
    }

    buffer[(size_t)n] = '\0';
    return 0;
}

static bool whitespace_word_present(
    const char *text,
    const char *word)
{
    size_t word_len;
    const char *p;

    if (text == NULL || word == NULL) {
        return false;
    }

    word_len = strlen(word);
    p = text;

    while (*p != '\0') {
        const char *start;
        size_t len;

        while (*p == ' ' || *p == '\t' ||
               *p == '\n' || *p == '\r') {
            ++p;
        }

        start = p;

        while (*p != '\0' &&
               *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r') {
            ++p;
        }

        len = (size_t)(p - start);

        if (len == word_len &&
            strncmp(start, word, word_len) == 0) {
            return true;
        }
    }

    return false;
}

static int write_text_path(
    const char *path,
    const char *text)
{
    int fd;
    size_t len;

    if (path == NULL || text == NULL) {
        return -1;
    }

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    len = strlen(text);

    if (write_full(fd, text, len) != 0) {
        (void)close(fd);
        return -1;
    }

    (void)close(fd);
    return 0;
}

static int cgroup_write_file(
    const char *cgroup_path,
    const char *file_name,
    const char *text)
{
    char path[PATH_MAX];
    int n;

    n = snprintf(
        path,
        sizeof(path),
        "%s/%s",
        cgroup_path,
        file_name);

    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }

    return write_text_path(path, text);
}

static int cgroup_move_pid(
    const char *cgroup_path,
    pid_t pid)
{
    char text[64];
    int n;

    n = snprintf(
        text,
        sizeof(text),
        "%d\n",
        (int)pid);

    if (n < 0 || (size_t)n >= sizeof(text)) {
        return -1;
    }

    return cgroup_write_file(
        cgroup_path,
        "cgroup.procs",
        text);
}

static int cgroup_set_cpu_max(
    const char *cgroup_path,
    bool throttled,
    unsigned int quota_us,
    unsigned int period_us)
{
    char text[128];
    int n;

    if (throttled) {
        n = snprintf(
            text,
            sizeof(text),
            "%u %u\n",
            quota_us,
            period_us);
    } else {
        n = snprintf(
            text,
            sizeof(text),
            "max %u\n",
            period_us);
    }

    if (n < 0 || (size_t)n >= sizeof(text)) {
        return -1;
    }

    return cgroup_write_file(
        cgroup_path,
        "cpu.max",
        text);
}

static void cgroup_context_destroy(
    struct cgroup_context *ctx)
{
    size_t i;

    if (ctx == NULL || !ctx->ready) {
        return;
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        (void)rmdir(ctx->background_path[i]);
    }
    (void)rmdir(ctx->run_path);
    memset(ctx, 0, sizeof(*ctx));
}

static int cgroup_context_create(
    struct cgroup_context *ctx)
{
    char root_subtree[4096];
    char child_controllers[4096];
    char path[PATH_MAX];
    int n;
    size_t i;

    if (ctx == NULL) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));

    if (read_text_file(
            "/sys/fs/cgroup/cgroup.subtree_control",
            root_subtree,
            sizeof(root_subtree)) != 0) {
        perror("read cgroup.subtree_control");
        return -1;
    }

    if (!whitespace_word_present(root_subtree, "cpu")) {
        fprintf(stderr,
                "[ERROR] cpu controller is not enabled in "
                "/sys/fs/cgroup/cgroup.subtree_control\n");
        fprintf(stderr,
                "[HINT] enable +cpu in the parent cgroup before this experiment\n");
        return -1;
    }

    n = snprintf(ctx->run_path,
                 sizeof(ctx->run_path),
                 "/sys/fs/cgroup/oracle_6_5e_%d",
                 (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(ctx->run_path)) {
        return -1;
    }

    if (mkdir(ctx->run_path, 0755) != 0) {
        perror("mkdir run cgroup");
        return -1;
    }
    ctx->ready = true;

    n = snprintf(path, sizeof(path), "%s/cgroup.controllers", ctx->run_path);
    if (n < 0 || (size_t)n >= sizeof(path) ||
        read_text_file(path, child_controllers, sizeof(child_controllers)) != 0) {
        goto fail;
    }

    if (!whitespace_word_present(child_controllers, "cpu")) {
        fprintf(stderr,
                "[ERROR] cpu controller is unavailable to experiment cgroup\n");
        goto fail;
    }

    if (cgroup_write_file(ctx->run_path,
                          "cgroup.subtree_control",
                          "+cpu\n") != 0) {
        perror("enable cpu controller");
        goto fail;
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        n = snprintf(ctx->background_path[i],
                     sizeof(ctx->background_path[i]),
                     "%s/%s",
                     ctx->run_path,
                     candidate_name(i));
        if (n < 0 || (size_t)n >= sizeof(ctx->background_path[i])) {
            goto fail;
        }
        if (mkdir(ctx->background_path[i], 0755) != 0) {
            perror("mkdir candidate cgroup");
            goto fail;
        }
        printf("[CGROUP] %s=%s\n",
               candidate_name(i),
               ctx->background_path[i]);
    }

    printf("[CGROUP] root=%s\n", ctx->run_path);
    return 0;

fail:
    cgroup_context_destroy(ctx);
    return -1;
}

static pid_t spawn_victim(
    int cpu,
    unsigned int seconds,
    uintptr_t root,
    int *read_fd_out)
{
    int pipefd[2];
    pid_t pid;

    if (read_fd_out == NULL) {
        return -1;
    }

    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid = fork();

    if (pid < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        struct worker_result warmup;
        struct worker_result result;

        (void)close(pipefd[0]);

        if (pin_to_cpu(cpu) != 0) {
            _exit(101);
        }

        if (raise(SIGSTOP) != 0) {
            _exit(102);
        }

        if (victim_loop(
                root,
                WARMUP_SECONDS,
                &warmup) != 0) {
            _exit(103);
        }

        if (victim_loop(
                root,
                (double)seconds,
                &result) != 0) {
            _exit(104);
        }

        if (write_full(
                pipefd[1],
                &result,
                sizeof(result)) != 0) {
            _exit(105);
        }

        (void)close(pipefd[1]);

        _exit(0);
    }

    (void)close(pipefd[1]);

    *read_fd_out =
        pipefd[0];

    return pid;
}

static pid_t spawn_aggressor(
    int cpu,
    unsigned int seconds,
    const uintptr_t roots[AGGRESSOR_STREAMS],
    int *read_fd_out)
{
    int pipefd[2];
    pid_t pid;

    if (roots == NULL ||
        read_fd_out == NULL) {
        return -1;
    }

    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid = fork();

    if (pid < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        struct worker_result warmup;
        struct worker_result result;

        (void)close(pipefd[0]);

        if (pin_to_cpu(cpu) != 0) {
            _exit(111);
        }

        if (raise(SIGSTOP) != 0) {
            _exit(112);
        }

        if (aggressor_loop(
                roots,
                WARMUP_SECONDS,
                &warmup) != 0) {
            _exit(113);
        }

        if (aggressor_loop(
                roots,
                (double)seconds,
                &result) != 0) {
            _exit(114);
        }

        if (write_full(
                pipefd[1],
                &result,
                sizeof(result)) != 0) {
            _exit(115);
        }

        (void)close(pipefd[1]);

        _exit(0);
    }

    (void)close(pipefd[1]);

    *read_fd_out =
        pipefd[0];

    return pid;
}

static int wait_until_stopped(pid_t pid)
{
    int status;

    for (;;) {
        pid_t rc =
            waitpid(
                pid,
                &status,
                WUNTRACED);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (WIFSTOPPED(status)) {
            return 0;
        }

        return -1;
    }
}

static int wait_success(pid_t pid)
{
    int status;

    for (;;) {
        pid_t rc =
            waitpid(pid, &status, 0);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (WIFEXITED(status) &&
            WEXITSTATUS(status) == 0) {
            return 0;
        }

        if (WIFEXITED(status)) {
            fprintf(stderr,
                    "[ERROR] worker pid=%d exit=%d\n",
                    (int)pid,
                    WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr,
                    "[ERROR] worker pid=%d signal=%d\n",
                    (int)pid,
                    WTERMSIG(status));
        }

        return -1;
    }
}

static int run_condition(
    enum condition condition,
    const struct config *cfg,
    const struct cgroup_context *cgroups,
    uintptr_t victim_root,
    uintptr_t background_roots[CANDIDATE_COUNT][AGGRESSOR_STREAMS],
    struct condition_result *out)
{
    pid_t victim_pid = -1;
    pid_t bg_pid[CANDIDATE_COUNT];
    int victim_fd = -1;
    int bg_fd[CANDIDATE_COUNT];
    bool throttled[CANDIDATE_COUNT] = {false};
    size_t i;

    if (cfg == NULL || cgroups == NULL || !cgroups->ready ||
        background_roots == NULL || out == NULL) {
        return -1;
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        bg_pid[i] = -1;
        bg_fd[i] = -1;
    }

    memset(out, 0, sizeof(*out));
    out->has_background = condition != CONDITION_NONE;

    if (condition == CONDITION_LOWEST_ONE) {
        throttled[0] = true;
    } else if (condition == CONDITION_HIGHEST_ONE) {
        throttled[4] = true;
    } else if (condition == CONDITION_TOP2) {
        throttled[3] = true;
        throttled[4] = true;
    } else if (condition == CONDITION_ALL) {
        for (i = 0U; i < CANDIDATE_COUNT; ++i) {
            throttled[i] = true;
        }
    }

    if (out->has_background) {
        for (i = 0U; i < CANDIDATE_COUNT; ++i) {
            if (cgroup_set_cpu_max(
                    cgroups->background_path[i],
                    throttled[i],
                    cfg->throttle_quota_us,
                    cfg->throttle_period_us) != 0) {
                perror("write cpu.max");
                goto fail;
            }
        }
    }

    victim_pid = spawn_victim(cfg->victim_cpu,
                              cfg->seconds,
                              victim_root,
                              &victim_fd);
    if (victim_pid < 0 || wait_until_stopped(victim_pid) != 0) {
        goto fail;
    }

    if (out->has_background) {
        for (i = 0U; i < CANDIDATE_COUNT; ++i) {
            bg_pid[i] = spawn_aggressor(cfg->aggressor_cpu[i],
                                        cfg->seconds,
                                        background_roots[i],
                                        &bg_fd[i]);
            if (bg_pid[i] < 0 || wait_until_stopped(bg_pid[i]) != 0) {
                goto fail;
            }

            if (cgroup_move_pid(cgroups->background_path[i], bg_pid[i]) != 0) {
                perror("move worker to cgroup");
                goto fail;
            }

            printf("[CONTROL] %s_cpu_max=%s\n",
                   candidate_name(i),
                   throttled[i] ? "throttled" : "unrestricted");
        }

        for (i = 0U; i < CANDIDATE_COUNT; ++i) {
            if (kill(bg_pid[i], SIGCONT) != 0) {
                goto fail;
            }
        }
    }

    if (kill(victim_pid, SIGCONT) != 0) {
        goto fail;
    }

    if (wait_success(victim_pid) != 0) {
        goto fail;
    }
    victim_pid = -1;

    if (out->has_background) {
        for (i = 0U; i < CANDIDATE_COUNT; ++i) {
            if (wait_success(bg_pid[i]) != 0) {
                goto fail;
            }
            bg_pid[i] = -1;
        }
    }

    if (read_full(victim_fd, &out->victim, sizeof(out->victim)) != 0) {
        goto fail;
    }
    (void)close(victim_fd);
    victim_fd = -1;

    if (out->has_background) {
        for (i = 0U; i < CANDIDATE_COUNT; ++i) {
            if (read_full(bg_fd[i],
                          &out->background[i],
                          sizeof(out->background[i])) != 0) {
                goto fail;
            }
            (void)close(bg_fd[i]);
            bg_fd[i] = -1;
        }
    }

    return 0;

fail:
    if (victim_pid > 0) {
        (void)kill(victim_pid, SIGCONT);
        (void)kill(victim_pid, SIGKILL);
        (void)waitpid(victim_pid, NULL, 0);
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        if (bg_pid[i] > 0) {
            (void)kill(bg_pid[i], SIGCONT);
            (void)kill(bg_pid[i], SIGKILL);
            (void)waitpid(bg_pid[i], NULL, 0);
        }
        if (bg_fd[i] >= 0) {
            (void)close(bg_fd[i]);
        }
    }

    if (victim_fd >= 0) {
        (void)close(victim_fd);
    }

    return -1;
}

static int verify_pool_pfns(
    const struct memory_pool *pool,
    uint64_t *moved_out,
    uint64_t *unreadable_out)
{
    int fd;
    size_t i;

    uint64_t moved = 0U;
    uint64_t unreadable = 0U;

    if (pool == NULL ||
        moved_out == NULL ||
        unreadable_out == NULL) {
        return -1;
    }

    fd = open(
        "/proc/self/pagemap",
        O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return -1;
    }

    for (i = 0U;
         i < pool->page_count;
         ++i) {
        uintptr_t va =
            (uintptr_t)(
                pool->base +
                i * pool->page_size);

        uint64_t pfn;

        if (read_one_pfn(
                fd,
                va,
                pool->page_size,
                &pfn) != 0) {
            ++unreadable;
            continue;
        }

        if (pfn !=
            pool->pfns[i]) {
            ++moved;
        }
    }

    (void)close(fd);

    *moved_out = moved;
    *unreadable_out = unreadable;

    return 0;
}

int main(int argc, char **argv)
{
    struct config cfg;
    struct calibration_result calibration;
    struct dram_mapping mapping;
    struct cgroup_context cgroups;

    struct memory_pool victim_pool;
    struct memory_pool background_pool[CANDIDATE_COUNT];
    struct line_set victim_set;
    struct line_set background_set[CANDIDATE_COUNT];

    uint64_t *offset_classes = NULL;
    uint64_t *pool_counts[POOL_COUNT] = {NULL};
    const uint64_t *pool_counts_const[POOL_COUNT];
    uint64_t *victim_classes = NULL;
    uint64_t *disjoint_classes = NULL;
    uint64_t *background_classes[CANDIDATE_COUNT] = {NULL};
    uint64_t *victim_hist = NULL;
    uint64_t *background_hist[CANDIDATE_COUNT] = {NULL};

    size_t *permutation = NULL;
    uintptr_t victim_root = 0U;
    uintptr_t background_roots[CANDIDATE_COUNT][AGGRESSOR_STREAMS];
    struct condition_result results[CONDITION_COUNT];

    long page_size_long;
    size_t page_size;
    size_t class_space;
    size_t lines_per_page;
    size_t workset_lines;
    size_t quota_lines;
    size_t i;
    size_t j;

    uint64_t overlap_intersection[CANDIDATE_COUNT] = {0U};
    uint64_t overlap_union[CANDIDATE_COUNT] = {0U};
    double overlap_jaccard[CANDIDATE_COUNT] = {0.0};
    size_t overlap_shared[CANDIDATE_COUNT] = {0U};

    memset(&cgroups, 0, sizeof(cgroups));
    memset(&victim_pool, 0, sizeof(victim_pool));
    memset(background_pool, 0, sizeof(background_pool));
    memset(&victim_set, 0, sizeof(victim_set));
    memset(background_set, 0, sizeof(background_set));
    memset(background_roots, 0, sizeof(background_roots));
    memset(results, 0, sizeof(results));

    if (parse_args(argc, argv, &cfg) != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        fprintf(stderr, "[ERROR] invalid page size\n");
        return EXIT_FAILURE;
    }
    page_size = (size_t)page_size_long;

    printf("============================================================\n");
    printf("Experiment E: multi-candidate selective control v1\n");
    printf("============================================================\n");
    printf("[CONFIG] self_test=%s\n", cfg.self_test ? "true" : "false");
    printf("[CONFIG] page_size=%zu\n", page_size);
    printf("[CONFIG] pool_mib_each=%zu\n", cfg.pool_mib);
    printf("[CONFIG] pool_count=%u\n", (unsigned int)POOL_COUNT);
    printf("[CONFIG] pool_total_mib=%zu\n", cfg.pool_mib * (size_t)POOL_COUNT);
    printf("[CONFIG] workset_mib_each=%zu\n", cfg.workset_mib);
    printf("[CONFIG] group_classes=%zu\n", cfg.group_classes);
    printf("[CONFIG] seconds_per_condition=%u\n", cfg.seconds);
    printf("[CONFIG] victim_cpu=%d\n", cfg.victim_cpu);
    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        printf("[CONFIG] %s_cpu=%d\n", candidate_name(i), cfg.aggressor_cpu[i]);
    }
    printf("[CONFIG] throttle_cpu_max=%u %u\n",
           cfg.throttle_quota_us,
           cfg.throttle_period_us);
    printf("[CONFIG] seed=%" PRIu64 "\n", cfg.seed);
    printf("[CONFIG] order=");
    for (i = 0U; i < CONDITION_COUNT; ++i) {
        printf("%s%s", i == 0U ? "" : ",", condition_name(cfg.order[i]));
    }
    printf("\n");

    if (load_mapping(&calibration, &mapping) != 0 ||
        class_space_from_mapping(&mapping, &class_space) != 0 ||
        build_offset_classes(&mapping,
                             page_size,
                             &offset_classes,
                             &lines_per_page) != 0) {
        goto fail;
    }

    printf("[MAPPING] possible_class_space=%zu\n", class_space);

    if (cfg.workset_mib > SIZE_MAX / (size_t)MIB_BYTES) {
        goto fail;
    }
    {
        size_t workset_bytes = cfg.workset_mib * (size_t)MIB_BYTES;
        if (workset_bytes % CACHELINE_BYTES != 0U) {
            fprintf(stderr, "[ERROR] workset size must be cache-line aligned\n");
            goto fail;
        }
        workset_lines = workset_bytes / CACHELINE_BYTES;
    }

    if (workset_lines % cfg.group_classes != 0U ||
        cfg.group_classes % 4U != 0U) {
        fprintf(stderr,
                "[ERROR] workset must divide evenly and classes must be divisible by 4\n");
        goto fail;
    }
    quota_lines = workset_lines / cfg.group_classes;
    printf("[SELECT] workset_lines=%zu\n", workset_lines);
    printf("[SELECT] quota_lines_per_class=%zu\n", quota_lines);

    /* Six independent pools: victim + five background candidates. */
    if (pool_create(&victim_pool,
                    "victim",
                    cfg.pool_mib,
                    page_size,
                    cfg.seed ^ 0x1111111111111111ULL) != 0) {
        goto fail;
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        uint64_t fault_seed = cfg.seed ^
            (0x2222222222222222ULL +
             (uint64_t)i * 0x1111111111111111ULL);
        if (pool_create(&background_pool[i],
                        candidate_name(i),
                        cfg.pool_mib,
                        page_size,
                        fault_seed) != 0) {
            goto fail;
        }
    }

    for (i = 0U; i < POOL_COUNT; ++i) {
        pool_counts[i] = calloc(class_space, sizeof(pool_counts[i][0]));
        if (pool_counts[i] == NULL) {
            goto fail;
        }
        pool_counts_const[i] = pool_counts[i];
    }

    victim_classes = calloc(cfg.group_classes, sizeof(victim_classes[0]));
    disjoint_classes = calloc(cfg.group_classes, sizeof(disjoint_classes[0]));
    if (victim_classes == NULL || disjoint_classes == NULL) {
        goto fail;
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        background_classes[i] = calloc(cfg.group_classes,
                                       sizeof(background_classes[i][0]));
        if (background_classes[i] == NULL) {
            goto fail;
        }
    }

    printf("[CLASSIFY] counting cache-line classes\n");
    if (count_line_classes(&victim_pool,
                           &mapping,
                           offset_classes,
                           lines_per_page,
                           pool_counts[0],
                           class_space) != 0) {
        goto fail;
    }
    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        if (count_line_classes(&background_pool[i],
                               &mapping,
                               offset_classes,
                               lines_per_page,
                               pool_counts[i + 1U],
                               class_space) != 0) {
            goto fail;
        }
    }

    if (choose_class_sets(pool_counts_const,
                          class_space,
                          cfg.group_classes,
                          quota_lines,
                          cfg.seed,
                          victim_classes,
                          disjoint_classes) != 0) {
        goto fail;
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        size_t shared = candidate_shared_classes(i, cfg.group_classes);
        for (j = 0U; j < cfg.group_classes; ++j) {
            background_classes[i][j] =
                j < shared ? victim_classes[j] : disjoint_classes[j - shared];
        }
        printf("[CLASS-SET] %s shared_classes=%zu/%zu\n",
               candidate_name(i),
               shared,
               cfg.group_classes);
    }

    if (collect_line_set(&victim_set,
                         &victim_pool,
                         &mapping,
                         offset_classes,
                         lines_per_page,
                         class_space,
                         victim_classes,
                         cfg.group_classes,
                         quota_lines,
                         cfg.seed ^ 0x4444444444444444ULL) != 0) {
        goto fail;
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        uint64_t select_seed = cfg.seed ^
            (0x5555555555555555ULL +
             (uint64_t)i * 0x0101010101010101ULL);
        if (collect_line_set(&background_set[i],
                             &background_pool[i],
                             &mapping,
                             offset_classes,
                             lines_per_page,
                             class_space,
                             background_classes[i],
                             cfg.group_classes,
                             quota_lines,
                             select_seed) != 0) {
            goto fail;
        }
    }

    printf("[SELECT] victim_lines=%zu\n", victim_set.line_count);
    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        printf("[SELECT] %s_lines=%zu\n",
               candidate_name(i),
               background_set[i].line_count);
    }

    victim_hist = calloc(class_space, sizeof(victim_hist[0]));
    if (victim_hist == NULL ||
        build_histogram(&victim_set,
                        &mapping,
                        victim_hist,
                        class_space) != 0) {
        goto fail;
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        size_t expected_shared = candidate_shared_classes(i, cfg.group_classes);
        uint64_t expected_intersection =
            (uint64_t)expected_shared * (uint64_t)quota_lines;
        uint64_t expected_union =
            (uint64_t)(2U * cfg.group_classes - expected_shared) *
            (uint64_t)quota_lines;

        background_hist[i] = calloc(class_space,
                                    sizeof(background_hist[i][0]));
        if (background_hist[i] == NULL ||
            build_histogram(&background_set[i],
                            &mapping,
                            background_hist[i],
                            class_space) != 0) {
            goto fail;
        }

        overlap_jaccard[i] = histogram_jaccard(
            victim_hist,
            background_hist[i],
            class_space,
            &overlap_intersection[i],
            &overlap_union[i]);
        overlap_shared[i] = histogram_shared_classes(
            victim_hist,
            background_hist[i],
            class_space);

        printf("[OVERLAP] candidate=%s shared_classes=%zu/%zu "
               "shared_fraction=%.6f intersection=%" PRIu64 " "
               "union=%" PRIu64 " weighted_jaccard=%.6f\n",
               candidate_name(i),
               overlap_shared[i],
               cfg.group_classes,
               (double)overlap_shared[i] / (double)cfg.group_classes,
               overlap_intersection[i],
               overlap_union[i],
               overlap_jaccard[i]);

        if (histogram_unique_classes(background_hist[i], class_space) !=
                cfg.group_classes ||
            overlap_shared[i] != expected_shared ||
            overlap_intersection[i] != expected_intersection ||
            overlap_union[i] != expected_union) {
            fprintf(stderr,
                    "[ERROR] actual overlap does not match requested level for %s\n",
                    candidate_name(i));
            goto fail;
        }
    }

    if (make_permutation(workset_lines,
                         cfg.seed ^ 0x7777777777777777ULL,
                         &permutation) != 0 ||
        build_single_chain(&victim_set,
                           permutation,
                           &victim_root) != 0) {
        goto fail;
    }

    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        if (build_multi_chain(&background_set[i],
                              permutation,
                              AGGRESSOR_STREAMS,
                              background_roots[i]) != 0) {
            goto fail;
        }
    }

    if (cgroup_context_create(&cgroups) != 0) {
        goto fail;
    }

    printf("\nRESULT_CSV_HEADER,condition,victim_elapsed_sec,victim_reads,"
           "victim_ns_per_read,o0_Mload_per_sec,o25_Mload_per_sec,"
           "o50_Mload_per_sec,o75_Mload_per_sec,o100_Mload_per_sec,"
           "total_bg_Mload_per_sec\n");

    for (i = 0U; i < CONDITION_COUNT; ++i) {
        enum condition condition = cfg.order[i];
        struct condition_result result;
        double victim_ns;
        double bg_mload[CANDIDATE_COUNT] = {0.0};
        double total_bg = 0.0;

        printf("\n============================================================\n");
        printf("[RUN] condition=%s seconds=%u\n",
               condition_name(condition),
               cfg.seconds);
        printf("[RUN] victim_cpu=%d\n", cfg.victim_cpu);
        if (condition != CONDITION_NONE) {
            for (j = 0U; j < CANDIDATE_COUNT; ++j) {
                printf("[RUN] %s_cpu=%d\n",
                       candidate_name(j),
                       cfg.aggressor_cpu[j]);
            }
        }
        printf("============================================================\n");

        if (run_condition(condition,
                          &cfg,
                          &cgroups,
                          victim_root,
                          background_roots,
                          &result) != 0) {
            goto fail;
        }
        results[condition] = result;

        victim_ns = result.victim.elapsed_sec * 1000000000.0 /
                    (double)result.victim.operations;

        if (result.has_background) {
            for (j = 0U; j < CANDIDATE_COUNT; ++j) {
                bg_mload[j] =
                    (double)result.background[j].operations /
                    result.background[j].elapsed_sec /
                    1000000.0;
                total_bg += bg_mload[j];
            }
        }

        printf("[RESULT] condition=%s\n", condition_name(condition));
        printf("[RESULT] victim_elapsed_sec=%.6f\n", result.victim.elapsed_sec);
        printf("[RESULT] victim_reads=%" PRIu64 "\n", result.victim.operations);
        printf("[RESULT] victim_ns_per_read=%.3f\n", victim_ns);
        if (result.has_background) {
            for (j = 0U; j < CANDIDATE_COUNT; ++j) {
                printf("[RESULT] %s_Mload_per_sec=%.3f\n",
                       candidate_name(j), bg_mload[j]);
            }
            printf("[RESULT] total_bg_Mload_per_sec=%.3f\n", total_bg);
        }

        printf("RESULT_CSV,%s,%.6f,%" PRIu64 ",%.3f,"
               "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
               condition_name(condition),
               result.victim.elapsed_sec,
               result.victim.operations,
               victim_ns,
               bg_mload[0], bg_mload[1], bg_mload[2],
               bg_mload[3], bg_mload[4], total_bg);
    }

    {
        double latency[CONDITION_COUNT];
        double total_bg[CONDITION_COUNT] = {0.0};
        double none_ns;
        double uncontrolled_ns;
        double span;
        size_t c;

        for (c = 0U; c < CONDITION_COUNT; ++c) {
            latency[c] = results[c].victim.elapsed_sec * 1000000000.0 /
                         (double)results[c].victim.operations;
            if (results[c].has_background) {
                for (j = 0U; j < CANDIDATE_COUNT; ++j) {
                    total_bg[c] +=
                        (double)results[c].background[j].operations /
                        results[c].background[j].elapsed_sec /
                        1000000.0;
                }
            }
        }

        none_ns = latency[CONDITION_NONE];
        uncontrolled_ns = latency[CONDITION_UNCONTROLLED];
        span = uncontrolled_ns - none_ns;

        printf("\n[COMPARE] uncontrolled_vs_none_latency_pct=%.3f\n",
               (uncontrolled_ns / none_ns - 1.0) * 100.0);

        for (c = CONDITION_LOWEST_ONE; c <= CONDITION_ALL; ++c) {
            double recovery = 0.0;
            double retention = 0.0;
            if (span > 0.0) {
                recovery = (uncontrolled_ns - latency[c]) / span * 100.0;
            }
            if (total_bg[CONDITION_UNCONTROLLED] > 0.0) {
                retention = total_bg[c] /
                            total_bg[CONDITION_UNCONTROLLED] * 100.0;
            }
            printf("[COMPARE] %s_vs_none_latency_pct=%.3f\n",
                   condition_name((enum condition)c),
                   (latency[c] / none_ns - 1.0) * 100.0);
            printf("[COMPARE] %s_recovery_pct=%.3f\n",
                   condition_name((enum condition)c), recovery);
            printf("[COMPARE] %s_bg_retention_pct=%.3f\n",
                   condition_name((enum condition)c), retention);
        }

        printf("[COMPARE] highest_vs_lowest_victim_latency_pct=%.3f\n",
               (latency[CONDITION_HIGHEST_ONE] /
                    latency[CONDITION_LOWEST_ONE] - 1.0) * 100.0);
        if (total_bg[CONDITION_LOWEST_ONE] > 0.0) {
            printf("[COMPARE] highest_vs_lowest_bg_throughput_pct=%.3f\n",
                   (total_bg[CONDITION_HIGHEST_ONE] /
                        total_bg[CONDITION_LOWEST_ONE] - 1.0) * 100.0);
        }
    }

    {
        uint64_t moved;
        uint64_t unreadable;
        if (verify_pool_pfns(&victim_pool, &moved, &unreadable) != 0) {
            goto fail;
        }
        printf("[VERIFY] victim_pfn_moved=%" PRIu64
               " unreadable=%" PRIu64 "\n", moved, unreadable);
        if (moved != 0U || unreadable != 0U) {
            goto fail;
        }

        for (i = 0U; i < CANDIDATE_COUNT; ++i) {
            if (verify_pool_pfns(&background_pool[i], &moved, &unreadable) != 0) {
                goto fail;
            }
            printf("[VERIFY] %s_pfn_moved=%" PRIu64
                   " unreadable=%" PRIu64 "\n",
                   candidate_name(i), moved, unreadable);
            if (moved != 0U || unreadable != 0U) {
                goto fail;
            }
        }
    }

    if (cfg.self_test) {
        printf("\n============================================================\n");
        printf("[SELF-TEST PASS]\n");
        printf("  - calibration loaded\n");
        printf("  - bank class computed per 64-byte cache line\n");
        printf("  - victim and five candidates use independent physical pools\n");
        printf("  - actual 0/25/50/75/100 overlap levels were verified\n");
        printf("  - pointer metadata lives inside selected lines\n");
        printf("  - five background workers used separate cgroups\n");
        printf("  - none/uncontrolled/lowest/highest/top2/all completed\n");
        printf("  - PFNs remained stable\n");
        printf("\nNOTE: latency/throughput ordering is not a self-test criterion.\n");
        printf("============================================================\n");
    }

    cgroup_context_destroy(&cgroups);
    free(permutation);
    free(victim_hist);
    free(victim_classes);
    free(disjoint_classes);
    free(offset_classes);
    for (i = 0U; i < POOL_COUNT; ++i) free(pool_counts[i]);
    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        free(background_classes[i]);
        free(background_hist[i]);
        line_set_destroy(&background_set[i]);
        pool_destroy(&background_pool[i]);
    }
    line_set_destroy(&victim_set);
    pool_destroy(&victim_pool);
    return EXIT_SUCCESS;

fail:
    cgroup_context_destroy(&cgroups);
    free(permutation);
    free(victim_hist);
    free(victim_classes);
    free(disjoint_classes);
    free(offset_classes);
    for (i = 0U; i < POOL_COUNT; ++i) free(pool_counts[i]);
    for (i = 0U; i < CANDIDATE_COUNT; ++i) {
        free(background_classes[i]);
        free(background_hist[i]);
        line_set_destroy(&background_set[i]);
        pool_destroy(&background_pool[i]);
    }
    line_set_destroy(&victim_set);
    pool_destroy(&victim_pool);
    return EXIT_FAILURE;
}


