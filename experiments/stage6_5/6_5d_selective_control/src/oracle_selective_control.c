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

#define VICTIM_CHUNK_READS 4096U
#define AGGRESSOR_CHUNK_ITERS 1024U

#define WARMUP_SECONDS 1.0

#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_SWAPPED (1ULL << 62)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1ULL)

enum condition {
    CONDITION_NONE = 0,
    CONDITION_UNCONTROLLED,
    CONDITION_SELECTIVE,
    CONDITION_ALL,
    CONDITION_COUNT
};

struct config {
    size_t pool_mib;
    size_t workset_mib;
    size_t group_classes;

    unsigned int seconds;

    int victim_cpu;
    int high_aggressor_cpu;
    int low_aggressor_cpu;

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
    struct worker_result high;
    struct worker_result low;

    bool has_background;
};

struct cgroup_context {
    bool ready;

    char run_path[PATH_MAX];
    char high_path[PATH_MAX];
    char low_path[PATH_MAX];
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

    case CONDITION_SELECTIVE:
        return "selective";

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
        return 0;
    }

    if (strcmp(text, "uncontrolled") == 0) {
        *out = CONDITION_UNCONTROLLED;
        return 0;
    }

    if (strcmp(text, "selective") == 0) {
        *out = CONDITION_SELECTIVE;
        return 0;
    }

    if (strcmp(text, "all") == 0) {
        *out = CONDITION_ALL;
        return 0;
    }

    return -1;
}

static int parse_order(
    const char *text,
    enum condition order[CONDITION_COUNT])
{
    char buffer[128];
    char *save = NULL;
    char *token;
    bool seen[CONDITION_COUNT] = {false};
    size_t index = 0U;

    if (text == NULL || order == NULL) {
        return -1;
    }

    if (strlen(text) >= sizeof(buffer)) {
        return -1;
    }

    (void)snprintf(buffer, sizeof(buffer), "%s", text);

    token = strtok_r(buffer, ",", &save);

    while (token != NULL) {
        enum condition condition;

        if (index >= CONDITION_COUNT) {
            return -1;
        }

        if (condition_from_name(token, &condition) != 0) {
            return -1;
        }

        if (seen[condition]) {
            return -1;
        }

        seen[condition] = true;
        order[index++] = condition;

        token = strtok_r(NULL, ",", &save);
    }

    if (index != CONDITION_COUNT) {
        return -1;
    }

    return 0;
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
    cfg->high_aggressor_cpu = 12;
    cfg->low_aggressor_cpu = 10;

    cfg->throttle_quota_us = 50000U;
    cfg->throttle_period_us = 100000U;

    cfg->seed = 20260814ULL;

    cfg->order[0] = CONDITION_NONE;
    cfg->order[1] = CONDITION_UNCONTROLLED;
    cfg->order[2] = CONDITION_SELECTIVE;
    cfg->order[3] = CONDITION_ALL;
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
            "[--high-cpu N] "
            "[--low-cpu N] "
            "[--throttle-quota-us N] "
            "[--throttle-period-us N] "
            "[--seed N] "
            "[--order none,uncontrolled,selective,all]\n",
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
            if (++i >= argc ||
                parse_size(argv[i], &cfg->pool_mib) != 0 ||
                cfg->pool_mib == 0U) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--workset-mib") == 0) {
            if (++i >= argc ||
                parse_size(argv[i], &cfg->workset_mib) != 0 ||
                cfg->workset_mib == 0U) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--classes") == 0) {
            if (++i >= argc ||
                parse_size(argv[i], &cfg->group_classes) != 0 ||
                cfg->group_classes == 0U) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--seconds") == 0) {
            if (++i >= argc ||
                parse_uint(argv[i], &cfg->seconds) != 0 ||
                cfg->seconds == 0U) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--victim-cpu") == 0) {
            if (++i >= argc ||
                parse_int(argv[i], &cfg->victim_cpu) != 0) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--high-cpu") == 0) {
            if (++i >= argc ||
                parse_int(argv[i], &cfg->high_aggressor_cpu) != 0) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--low-cpu") == 0) {
            if (++i >= argc ||
                parse_int(argv[i], &cfg->low_aggressor_cpu) != 0) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--throttle-quota-us") == 0) {
            if (++i >= argc ||
                parse_uint(argv[i], &cfg->throttle_quota_us) != 0 ||
                cfg->throttle_quota_us == 0U) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--throttle-period-us") == 0) {
            if (++i >= argc ||
                parse_uint(argv[i], &cfg->throttle_period_us) != 0 ||
                cfg->throttle_period_us == 0U) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--seed") == 0) {
            if (++i >= argc ||
                parse_u64(argv[i], &cfg->seed) != 0) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "--order") == 0) {
            if (++i >= argc ||
                parse_order(argv[i], cfg->order) != 0) {
                return -1;
            }

            continue;
        }

        if (strcmp(argv[i], "-h") == 0 ||
            strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }

        return -1;
    }

    if (cfg->victim_cpu == cfg->high_aggressor_cpu ||
        cfg->victim_cpu == cfg->low_aggressor_cpu ||
        cfg->high_aggressor_cpu == cfg->low_aggressor_cpu ||
        cfg->throttle_quota_us > cfg->throttle_period_us) {
        return -1;
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
    const uint64_t *victim_counts,
    const uint64_t *high_counts,
    const uint64_t *low_counts,
    size_t class_space,
    size_t group_classes,
    size_t quota_lines,
    uint64_t seed,
    uint64_t *victim_classes,
    uint64_t *low_classes)
{
    uint64_t *candidates;
    size_t candidate_count = 0U;
    size_t cls;
    size_t i;

    if (victim_counts == NULL ||
        high_counts == NULL ||
        low_counts == NULL ||
        victim_classes == NULL ||
        low_classes == NULL ||
        group_classes == 0U ||
        quota_lines == 0U) {
        return -1;
    }

    candidates = calloc(
        class_space,
        sizeof(candidates[0]));

    if (candidates == NULL) {
        return -1;
    }

    /*
     * IMPORTANT:
     *
     * victim/high/low class selection all comes from the SAME
     * eligibility set.
     *
     * This removes the old bias:
     *
     *   high classes -> very dense classes
     *   low classes  -> sparse classes
     */
    for (cls = 0U;
         cls < class_space;
         ++cls) {
        if (victim_counts[cls] >=
                (uint64_t)quota_lines &&
            high_counts[cls] >=
                (uint64_t)quota_lines &&
            low_counts[cls] >=
                (uint64_t)quota_lines) {
            candidates[candidate_count++] =
                (uint64_t)cls;
        }
    }

    printf("[SELECT] common_eligible_classes=%zu\n",
           candidate_count);

    if (candidate_count <
        group_classes * 2U) {
        fprintf(stderr,
                "[ERROR] need at least %zu common eligible classes, have %zu\n",
                group_classes * 2U,
                candidate_count);
        free(candidates);
        return -1;
    }

    shuffle_u64(
        candidates,
        candidate_count,
        seed);

    for (i = 0U;
         i < group_classes;
         ++i) {
        victim_classes[i] =
            candidates[i];

        low_classes[i] =
            candidates[group_classes + i];
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
    if (ctx == NULL || !ctx->ready) {
        return;
    }

    (void)rmdir(ctx->high_path);
    (void)rmdir(ctx->low_path);
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

    n = snprintf(
        ctx->run_path,
        sizeof(ctx->run_path),
        "/sys/fs/cgroup/oracle_6_5d_%d",
        (int)getpid());

    if (n < 0 || (size_t)n >= sizeof(ctx->run_path)) {
        return -1;
    }

    n = snprintf(
        ctx->high_path,
        sizeof(ctx->high_path),
        "%s/high",
        ctx->run_path);

    if (n < 0 || (size_t)n >= sizeof(ctx->high_path)) {
        return -1;
    }

    n = snprintf(
        ctx->low_path,
        sizeof(ctx->low_path),
        "%s/low",
        ctx->run_path);

    if (n < 0 || (size_t)n >= sizeof(ctx->low_path)) {
        return -1;
    }

    if (mkdir(ctx->run_path, 0755) != 0) {
        perror("mkdir run cgroup");
        return -1;
    }

    ctx->ready = true;

    n = snprintf(
        path,
        sizeof(path),
        "%s/cgroup.controllers",
        ctx->run_path);

    if (n < 0 || (size_t)n >= sizeof(path) ||
        read_text_file(
            path,
            child_controllers,
            sizeof(child_controllers)) != 0) {
        goto fail;
    }

    if (!whitespace_word_present(child_controllers, "cpu")) {
        fprintf(stderr,
                "[ERROR] cpu controller is unavailable to experiment cgroup\n");
        goto fail;
    }

    if (cgroup_write_file(
            ctx->run_path,
            "cgroup.subtree_control",
            "+cpu\n") != 0) {
        perror("enable cpu controller");
        goto fail;
    }

    if (mkdir(ctx->high_path, 0755) != 0) {
        perror("mkdir high cgroup");
        goto fail;
    }

    if (mkdir(ctx->low_path, 0755) != 0) {
        perror("mkdir low cgroup");
        goto fail;
    }

    printf("[CGROUP] root=%s\n", ctx->run_path);
    printf("[CGROUP] high=%s\n", ctx->high_path);
    printf("[CGROUP] low=%s\n", ctx->low_path);

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
    const uintptr_t high_roots[AGGRESSOR_STREAMS],
    const uintptr_t low_roots[AGGRESSOR_STREAMS],
    struct condition_result *out)
{
    pid_t victim_pid = -1;
    pid_t high_pid = -1;
    pid_t low_pid = -1;

    int victim_fd = -1;
    int high_fd = -1;
    int low_fd = -1;

    bool high_throttled;
    bool low_throttled;

    if (cfg == NULL ||
        cgroups == NULL ||
        !cgroups->ready ||
        out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    out->has_background =
        condition != CONDITION_NONE;

    high_throttled =
        condition == CONDITION_SELECTIVE ||
        condition == CONDITION_ALL;

    low_throttled =
        condition == CONDITION_ALL;

    if (out->has_background) {
        if (cgroup_set_cpu_max(
                cgroups->high_path,
                high_throttled,
                cfg->throttle_quota_us,
                cfg->throttle_period_us) != 0 ||
            cgroup_set_cpu_max(
                cgroups->low_path,
                low_throttled,
                cfg->throttle_quota_us,
                cfg->throttle_period_us) != 0) {
            perror("write cpu.max");
            goto fail;
        }
    }

    victim_pid = spawn_victim(
        cfg->victim_cpu,
        cfg->seconds,
        victim_root,
        &victim_fd);

    if (victim_pid < 0 ||
        wait_until_stopped(victim_pid) != 0) {
        goto fail;
    }

    if (out->has_background) {
        high_pid = spawn_aggressor(
            cfg->high_aggressor_cpu,
            cfg->seconds,
            high_roots,
            &high_fd);

        if (high_pid < 0 ||
            wait_until_stopped(high_pid) != 0) {
            goto fail;
        }

        low_pid = spawn_aggressor(
            cfg->low_aggressor_cpu,
            cfg->seconds,
            low_roots,
            &low_fd);

        if (low_pid < 0 ||
            wait_until_stopped(low_pid) != 0) {
            goto fail;
        }

        /*
         * Move stopped workers before they execute any hot-loop work.
         */
        if (cgroup_move_pid(
                cgroups->high_path,
                high_pid) != 0 ||
            cgroup_move_pid(
                cgroups->low_path,
                low_pid) != 0) {
            perror("move worker to cgroup");
            goto fail;
        }

        printf("[CONTROL] high_cpu_max=%s\n",
               high_throttled ?
                   "throttled" :
                   "unrestricted");

        printf("[CONTROL] low_cpu_max=%s\n",
               low_throttled ?
                   "throttled" :
                   "unrestricted");

        if (kill(high_pid, SIGCONT) != 0 ||
            kill(low_pid, SIGCONT) != 0) {
            goto fail;
        }
    }

    /*
     * Victim follows immediately.  Every worker performs the same
     * one-second wall-clock warmup before its measured interval.
     */
    if (kill(victim_pid, SIGCONT) != 0) {
        goto fail;
    }

    if (wait_success(victim_pid) != 0) {
        goto fail;
    }
    victim_pid = -1;

    if (out->has_background) {
        if (wait_success(high_pid) != 0) {
            goto fail;
        }
        high_pid = -1;

        if (wait_success(low_pid) != 0) {
            goto fail;
        }
        low_pid = -1;
    }

    if (read_full(
            victim_fd,
            &out->victim,
            sizeof(out->victim)) != 0) {
        goto fail;
    }

    (void)close(victim_fd);
    victim_fd = -1;

    if (out->has_background) {
        if (read_full(
                high_fd,
                &out->high,
                sizeof(out->high)) != 0 ||
            read_full(
                low_fd,
                &out->low,
                sizeof(out->low)) != 0) {
            goto fail;
        }

        (void)close(high_fd);
        high_fd = -1;

        (void)close(low_fd);
        low_fd = -1;
    }

    return 0;

fail:
    if (victim_pid > 0) {
        (void)kill(victim_pid, SIGCONT);
        (void)kill(victim_pid, SIGKILL);
        (void)waitpid(victim_pid, NULL, 0);
    }

    if (high_pid > 0) {
        (void)kill(high_pid, SIGCONT);
        (void)kill(high_pid, SIGKILL);
        (void)waitpid(high_pid, NULL, 0);
    }

    if (low_pid > 0) {
        (void)kill(low_pid, SIGCONT);
        (void)kill(low_pid, SIGKILL);
        (void)waitpid(low_pid, NULL, 0);
    }

    if (victim_fd >= 0) {
        (void)close(victim_fd);
    }

    if (high_fd >= 0) {
        (void)close(high_fd);
    }

    if (low_fd >= 0) {
        (void)close(low_fd);
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
    struct memory_pool high_pool;
    struct memory_pool low_pool;

    struct line_set victim_set;
    struct line_set high_set;
    struct line_set low_set;

    uint64_t *offset_classes = NULL;

    uint64_t *victim_counts = NULL;
    uint64_t *high_counts = NULL;
    uint64_t *low_counts = NULL;

    uint64_t *victim_classes = NULL;
    uint64_t *low_classes = NULL;

    uint64_t *victim_hist = NULL;
    uint64_t *high_hist = NULL;
    uint64_t *low_hist = NULL;

    size_t *permutation = NULL;

    uintptr_t victim_root = 0U;

    uintptr_t high_roots[
        AGGRESSOR_STREAMS];

    uintptr_t low_roots[
        AGGRESSOR_STREAMS];

    struct condition_result results[
        CONDITION_COUNT];

    long page_size_long;
    size_t page_size;

    size_t class_space;
    size_t lines_per_page;

    size_t workset_lines;
    size_t quota_lines;

    uint64_t high_intersection;
    uint64_t high_union;

    uint64_t low_intersection;
    uint64_t low_union;

    double high_jaccard;
    double low_jaccard;

    size_t i;

    memset(&cgroups, 0, sizeof(cgroups));

    memset(
        &victim_pool,
        0,
        sizeof(victim_pool));

    memset(
        &high_pool,
        0,
        sizeof(high_pool));

    memset(
        &low_pool,
        0,
        sizeof(low_pool));

    memset(
        &victim_set,
        0,
        sizeof(victim_set));

    memset(
        &high_set,
        0,
        sizeof(high_set));

    memset(
        &low_set,
        0,
        sizeof(low_set));

    memset(
        high_roots,
        0,
        sizeof(high_roots));

    memset(
        low_roots,
        0,
        sizeof(low_roots));

    if (parse_args(
            argc,
            argv,
            &cfg) != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    page_size_long =
        sysconf(_SC_PAGESIZE);

    if (page_size_long <= 0) {
        fprintf(stderr,
                "[ERROR] invalid page size\n");
        return EXIT_FAILURE;
    }

    page_size =
        (size_t)page_size_long;

    printf("============================================================\n");
    printf("Oracle DRAM bank-overlap experiment v2\n");
    printf("============================================================\n");

    printf("[CONFIG] self_test=%s\n",
           cfg.self_test ?
               "true" :
               "false");

    printf("[CONFIG] page_size=%zu\n",
           page_size);

    printf("[CONFIG] pool_mib_each=%zu\n",
           cfg.pool_mib);

    printf("[CONFIG] pool_total_mib=%zu\n",
           cfg.pool_mib * 3U);

    printf("[CONFIG] workset_mib_each=%zu\n",
           cfg.workset_mib);

    printf("[CONFIG] group_classes=%zu\n",
           cfg.group_classes);

    printf("[CONFIG] seconds_per_condition=%u\n",
           cfg.seconds);

    printf("[CONFIG] victim_cpu=%d\n",
           cfg.victim_cpu);

    printf("[CONFIG] high_aggressor_cpu=%d\n",
           cfg.high_aggressor_cpu);

    printf("[CONFIG] low_aggressor_cpu=%d\n",
           cfg.low_aggressor_cpu);

    printf("[CONFIG] throttle_cpu_max=%u %u\n",
           cfg.throttle_quota_us,
           cfg.throttle_period_us);

    printf("[CONFIG] seed=%" PRIu64 "\n",
           cfg.seed);

    printf("[CONFIG] order=%s,%s,%s,%s\n",
           condition_name(cfg.order[0]),
           condition_name(cfg.order[1]),
           condition_name(cfg.order[2]),
           condition_name(cfg.order[3]));

    if (load_mapping(
            &calibration,
            &mapping) != 0) {
        goto fail;
    }

    if (class_space_from_mapping(
            &mapping,
            &class_space) != 0) {
        goto fail;
    }

    printf("[MAPPING] possible_class_space=%zu\n",
           class_space);

    if (build_offset_classes(
            &mapping,
            page_size,
            &offset_classes,
            &lines_per_page) != 0) {
        goto fail;
    }

    if (cfg.workset_mib >
        SIZE_MAX / (size_t)MIB_BYTES) {
        goto fail;
    }

    {
        size_t workset_bytes =
            cfg.workset_mib *
            (size_t)MIB_BYTES;

        if (workset_bytes %
            CACHELINE_BYTES != 0U) {
            fprintf(stderr,
                    "[ERROR] workset size must be cache-line aligned\n");
            goto fail;
        }

        workset_lines =
            workset_bytes /
            CACHELINE_BYTES;
    }

    if (workset_lines %
        cfg.group_classes != 0U) {
        fprintf(stderr,
                "[ERROR] workset lines must divide evenly across classes\n");
        goto fail;
    }

    quota_lines =
        workset_lines /
        cfg.group_classes;

    printf("[SELECT] workset_lines=%zu\n",
           workset_lines);

    printf("[SELECT] quota_lines_per_class=%zu\n",
           quota_lines);

    /*
     * Three independent physical pools.
     *
     * No victim/high/low physical pages are intentionally shared.
     */
    if (pool_create(
            &victim_pool,
            "victim",
            cfg.pool_mib,
            page_size,
            cfg.seed ^
                0x1111111111111111ULL) != 0) {
        goto fail;
    }

    if (pool_create(
            &high_pool,
            "high",
            cfg.pool_mib,
            page_size,
            cfg.seed ^
                0x2222222222222222ULL) != 0) {
        goto fail;
    }

    if (pool_create(
            &low_pool,
            "low",
            cfg.pool_mib,
            page_size,
            cfg.seed ^
                0x3333333333333333ULL) != 0) {
        goto fail;
    }

    victim_counts =
        calloc(
            class_space,
            sizeof(victim_counts[0]));

    high_counts =
        calloc(
            class_space,
            sizeof(high_counts[0]));

    low_counts =
        calloc(
            class_space,
            sizeof(low_counts[0]));

    victim_classes =
        calloc(
            cfg.group_classes,
            sizeof(victim_classes[0]));

    low_classes =
        calloc(
            cfg.group_classes,
            sizeof(low_classes[0]));

    if (victim_counts == NULL ||
        high_counts == NULL ||
        low_counts == NULL ||
        victim_classes == NULL ||
        low_classes == NULL) {
        goto fail;
    }

    printf("[CLASSIFY] counting cache-line classes\n");

    if (count_line_classes(
            &victim_pool,
            &mapping,
            offset_classes,
            lines_per_page,
            victim_counts,
            class_space) != 0) {
        goto fail;
    }

    if (count_line_classes(
            &high_pool,
            &mapping,
            offset_classes,
            lines_per_page,
            high_counts,
            class_space) != 0) {
        goto fail;
    }

    if (count_line_classes(
            &low_pool,
            &mapping,
            offset_classes,
            lines_per_page,
            low_counts,
            class_space) != 0) {
        goto fail;
    }

    if (choose_class_sets(
            victim_counts,
            high_counts,
            low_counts,
            class_space,
            cfg.group_classes,
            quota_lines,
            cfg.seed,
            victim_classes,
            low_classes) != 0) {
        goto fail;
    }

    for (i = 0U;
         i < cfg.group_classes;
         ++i) {
        uint64_t high_class =
            victim_classes[i];

        uint64_t low_class =
            low_classes[i];

        printf("[CLASS] slot=%zu "
               "overlap_class=0x%" PRIx64 " "
               "victim_lines=%" PRIu64 " "
               "high_lines=%" PRIu64 " "
               "low_class=0x%" PRIx64 " "
               "low_lines=%" PRIu64 " "
               "quota=%zu\n",
               i,
               high_class,
               victim_counts[high_class],
               high_counts[high_class],
               low_class,
               low_counts[low_class],
               quota_lines);
    }

    if (collect_line_set(
            &victim_set,
            &victim_pool,
            &mapping,
            offset_classes,
            lines_per_page,
            class_space,
            victim_classes,
            cfg.group_classes,
            quota_lines,
            cfg.seed ^
                0x4444444444444444ULL) != 0) {
        goto fail;
    }

    if (collect_line_set(
            &high_set,
            &high_pool,
            &mapping,
            offset_classes,
            lines_per_page,
            class_space,
            victim_classes,
            cfg.group_classes,
            quota_lines,
            cfg.seed ^
                0x5555555555555555ULL) != 0) {
        goto fail;
    }

    if (collect_line_set(
            &low_set,
            &low_pool,
            &mapping,
            offset_classes,
            lines_per_page,
            class_space,
            low_classes,
            cfg.group_classes,
            quota_lines,
            cfg.seed ^
                0x6666666666666666ULL) != 0) {
        goto fail;
    }

    printf("[SELECT] victim_lines=%zu\n",
           victim_set.line_count);

    printf("[SELECT] high_lines=%zu\n",
           high_set.line_count);

    printf("[SELECT] low_lines=%zu\n",
           low_set.line_count);

    victim_hist =
        calloc(
            class_space,
            sizeof(victim_hist[0]));

    high_hist =
        calloc(
            class_space,
            sizeof(high_hist[0]));

    low_hist =
        calloc(
            class_space,
            sizeof(low_hist[0]));

    if (victim_hist == NULL ||
        high_hist == NULL ||
        low_hist == NULL) {
        goto fail;
    }

    /*
     * This is the critical verification that the old code lacked:
     *
     * recompute bank class from every ACTUAL cache-line address.
     */
    if (build_histogram(
            &victim_set,
            &mapping,
            victim_hist,
            class_space) != 0 ||
        build_histogram(
            &high_set,
            &mapping,
            high_hist,
            class_space) != 0 ||
        build_histogram(
            &low_set,
            &mapping,
            low_hist,
            class_space) != 0) {
        goto fail;
    }

    high_jaccard =
        histogram_jaccard(
            victim_hist,
            high_hist,
            class_space,
            &high_intersection,
            &high_union);

    low_jaccard =
        histogram_jaccard(
            victim_hist,
            low_hist,
            class_space,
            &low_intersection,
            &low_union);

    printf("[OVERLAP-LINE] victim_unique_classes=%zu\n",
           histogram_unique_classes(
               victim_hist,
               class_space));

    printf("[OVERLAP-LINE] high_unique_classes=%zu\n",
           histogram_unique_classes(
               high_hist,
               class_space));

    printf("[OVERLAP-LINE] low_unique_classes=%zu\n",
           histogram_unique_classes(
               low_hist,
               class_space));

    printf("[OVERLAP-LINE] victim_vs_high "
           "intersection=%" PRIu64 " "
           "union=%" PRIu64 " "
           "weighted_jaccard=%.6f\n",
           high_intersection,
           high_union,
           high_jaccard);

    printf("[OVERLAP-LINE] victim_vs_low "
           "intersection=%" PRIu64 " "
           "union=%" PRIu64 " "
           "weighted_jaccard=%.6f\n",
           low_intersection,
           low_union,
           low_jaccard);

    /*
     * Hard acceptance criterion.
     *
     * Do not run a latency experiment if actual line-level
     * overlap is not exactly what we intended.
     */
    if (memcmp(
            victim_hist,
            high_hist,
            class_space *
                sizeof(victim_hist[0])) != 0) {
        fprintf(stderr,
                "[ERROR] actual victim/high line histograms differ\n");
        goto fail;
    }

    if (low_intersection != 0U) {
        fprintf(stderr,
                "[ERROR] actual victim/low line overlap is not zero\n");
        goto fail;
    }

    if (make_permutation(
            workset_lines,
            cfg.seed ^
                0x7777777777777777ULL,
            &permutation) != 0) {
        goto fail;
    }

    /*
     * Pointer metadata is embedded in the selected cache lines.
     *
     * Therefore the hot loop does NOT read an external pointer array
     * whose physical bank classes would be uncontrolled.
     */
    if (build_single_chain(
            &victim_set,
            permutation,
            &victim_root) != 0) {
        goto fail;
    }

    if (build_multi_chain(
            &high_set,
            permutation,
            AGGRESSOR_STREAMS,
            high_roots) != 0) {
        goto fail;
    }

    if (build_multi_chain(
            &low_set,
            permutation,
            AGGRESSOR_STREAMS,
            low_roots) != 0) {
        goto fail;
    }

    if (cgroup_context_create(&cgroups) != 0) {
        goto fail;
    }

    printf("\n");
    printf("RESULT_CSV_HEADER,"
           "condition,"
           "victim_elapsed_sec,"
           "victim_reads,"
           "victim_ns_per_read,"
           "high_elapsed_sec,"
           "high_load_ops,"
           "high_Mload_per_sec,"
           "low_elapsed_sec,"
           "low_load_ops,"
           "low_Mload_per_sec,"
           "total_bg_Mload_per_sec\n");

    for (i = 0U;
         i < CONDITION_COUNT;
         ++i) {
        enum condition condition =
            cfg.order[i];

        struct condition_result result;

        double victim_ns;
        double high_mload = 0.0;
        double low_mload = 0.0;
        double total_bg_mload = 0.0;

        printf("\n");
        printf("============================================================\n");
        printf("[RUN] condition=%s seconds=%u\n",
               condition_name(condition),
               cfg.seconds);
        printf("[RUN] victim_cpu=%d\n",
               cfg.victim_cpu);

        if (condition != CONDITION_NONE) {
            printf("[RUN] high_cpu=%d low_cpu=%d\n",
                   cfg.high_aggressor_cpu,
                   cfg.low_aggressor_cpu);
        }

        printf("============================================================\n");

        if (run_condition(
                condition,
                &cfg,
                &cgroups,
                victim_root,
                high_roots,
                low_roots,
                &result) != 0) {
            goto fail;
        }

        results[condition] = result;

        victim_ns =
            result.victim.elapsed_sec *
            1000000000.0 /
            (double)result.victim.operations;

        if (result.has_background) {
            high_mload =
                (double)result.high.operations /
                result.high.elapsed_sec /
                1000000.0;

            low_mload =
                (double)result.low.operations /
                result.low.elapsed_sec /
                1000000.0;

            total_bg_mload =
                high_mload + low_mload;
        }

        printf("[RESULT] condition=%s\n",
               condition_name(condition));
        printf("[RESULT] victim_elapsed_sec=%.6f\n",
               result.victim.elapsed_sec);
        printf("[RESULT] victim_reads=%" PRIu64 "\n",
               result.victim.operations);
        printf("[RESULT] victim_ns_per_read=%.3f\n",
               victim_ns);

        if (result.has_background) {
            printf("[RESULT] high_elapsed_sec=%.6f\n",
                   result.high.elapsed_sec);
            printf("[RESULT] high_load_ops=%" PRIu64 "\n",
                   result.high.operations);
            printf("[RESULT] high_Mload_per_sec=%.3f\n",
                   high_mload);

            printf("[RESULT] low_elapsed_sec=%.6f\n",
                   result.low.elapsed_sec);
            printf("[RESULT] low_load_ops=%" PRIu64 "\n",
                   result.low.operations);
            printf("[RESULT] low_Mload_per_sec=%.3f\n",
                   low_mload);
            printf("[RESULT] total_bg_Mload_per_sec=%.3f\n",
                   total_bg_mload);
        }

        printf("RESULT_CSV,%s,%.6f,%" PRIu64 ",%.3f,"
               "%.6f,%" PRIu64 ",%.3f,"
               "%.6f,%" PRIu64 ",%.3f,%.3f\n",
               condition_name(condition),
               result.victim.elapsed_sec,
               result.victim.operations,
               victim_ns,
               result.has_background ?
                   result.high.elapsed_sec :
                   0.0,
               result.has_background ?
                   result.high.operations :
                   0U,
               high_mload,
               result.has_background ?
                   result.low.elapsed_sec :
                   0.0,
               result.has_background ?
                   result.low.operations :
                   0U,
               low_mload,
               total_bg_mload);
    }

    {
        double latency[CONDITION_COUNT];
        double total_bg[CONDITION_COUNT] = {0.0};
        double none_ns;
        double uncontrolled_ns;
        double selective_ns;
        double all_ns;
        double slowdown_span;
        double selective_recovery = 0.0;
        double all_recovery = 0.0;
        size_t condition;

        for (condition = 0U;
             condition < CONDITION_COUNT;
             ++condition) {
            latency[condition] =
                results[condition].victim.elapsed_sec *
                1000000000.0 /
                (double)results[condition].victim.operations;

            if (results[condition].has_background) {
                total_bg[condition] =
                    (double)results[condition].high.operations /
                        results[condition].high.elapsed_sec /
                        1000000.0 +
                    (double)results[condition].low.operations /
                        results[condition].low.elapsed_sec /
                        1000000.0;
            }
        }

        none_ns = latency[CONDITION_NONE];
        uncontrolled_ns = latency[CONDITION_UNCONTROLLED];
        selective_ns = latency[CONDITION_SELECTIVE];
        all_ns = latency[CONDITION_ALL];

        slowdown_span =
            uncontrolled_ns - none_ns;

        if (slowdown_span > 0.0) {
            selective_recovery =
                (uncontrolled_ns - selective_ns) /
                slowdown_span * 100.0;

            all_recovery =
                (uncontrolled_ns - all_ns) /
                slowdown_span * 100.0;
        }

        printf("[COMPARE] uncontrolled_vs_none_latency_pct=%.3f\n",
               (uncontrolled_ns / none_ns - 1.0) * 100.0);
        printf("[COMPARE] selective_vs_none_latency_pct=%.3f\n",
               (selective_ns / none_ns - 1.0) * 100.0);
        printf("[COMPARE] all_vs_none_latency_pct=%.3f\n",
               (all_ns / none_ns - 1.0) * 100.0);
        printf("[COMPARE] selective_vs_all_latency_pct=%.3f\n",
               (selective_ns / all_ns - 1.0) * 100.0);
        printf("[COMPARE] selective_recovery_pct=%.3f\n",
               selective_recovery);
        printf("[COMPARE] all_recovery_pct=%.3f\n",
               all_recovery);

        if (total_bg[CONDITION_UNCONTROLLED] > 0.0) {
            printf("[COMPARE] selective_bg_retention_pct=%.3f\n",
                   total_bg[CONDITION_SELECTIVE] /
                       total_bg[CONDITION_UNCONTROLLED] *
                       100.0);
            printf("[COMPARE] all_bg_retention_pct=%.3f\n",
                   total_bg[CONDITION_ALL] /
                       total_bg[CONDITION_UNCONTROLLED] *
                       100.0);
        }

        if (total_bg[CONDITION_ALL] > 0.0) {
            printf("[COMPARE] selective_bg_gain_vs_all_pct=%.3f\n",
                   (total_bg[CONDITION_SELECTIVE] /
                        total_bg[CONDITION_ALL] -
                    1.0) * 100.0);
        }
    }

    {
        uint64_t victim_moved;
        uint64_t victim_unreadable;

        uint64_t high_moved;
        uint64_t high_unreadable;

        uint64_t low_moved;
        uint64_t low_unreadable;

        if (verify_pool_pfns(
                &victim_pool,
                &victim_moved,
                &victim_unreadable) != 0 ||
            verify_pool_pfns(
                &high_pool,
                &high_moved,
                &high_unreadable) != 0 ||
            verify_pool_pfns(
                &low_pool,
                &low_moved,
                &low_unreadable) != 0) {
            goto fail;
        }

        printf("[VERIFY] victim_pfn_moved=%" PRIu64
               " unreadable=%" PRIu64 "\n",
               victim_moved,
               victim_unreadable);

        printf("[VERIFY] high_pfn_moved=%" PRIu64
               " unreadable=%" PRIu64 "\n",
               high_moved,
               high_unreadable);

        printf("[VERIFY] low_pfn_moved=%" PRIu64
               " unreadable=%" PRIu64 "\n",
               low_moved,
               low_unreadable);

        if (victim_moved != 0U ||
            high_moved != 0U ||
            low_moved != 0U ||
            victim_unreadable != 0U ||
            high_unreadable != 0U ||
            low_unreadable != 0U) {
            fprintf(stderr,
                    "[ERROR] PFN stability check failed\n");
            goto fail;
        }
    }

    if (cfg.self_test) {
        printf("\n");
        printf("============================================================\n");
        printf("[SELF-TEST PASS]\n");
        printf("  - calibration loaded\n");
        printf("  - bank class computed per 64-byte cache line\n");
        printf("  - victim/high use separate physical pools\n");
        printf("  - victim/low use separate physical pools\n");
        printf("  - victim/high actual line histogram is identical\n");
        printf("  - victim/low actual line overlap is zero\n");
        printf("  - pointer metadata lives inside selected lines\n");
        printf("  - no child COW bank-class change\n");
        printf("  - PFNs remained stable\n");
        printf("  - high and low workers used separate cgroups\n");
        printf("  - uncontrolled/selective/all control conditions completed\n");
        printf("\n");
        printf("NOTE: latency/throughput ordering is not a self-test criterion.\n");
        printf("============================================================\n");
    }

    cgroup_context_destroy(&cgroups);

    free(permutation);

    free(victim_hist);
    free(high_hist);
    free(low_hist);

    free(victim_classes);
    free(low_classes);

    free(victim_counts);
    free(high_counts);
    free(low_counts);

    free(offset_classes);

    line_set_destroy(&victim_set);
    line_set_destroy(&high_set);
    line_set_destroy(&low_set);

    pool_destroy(&victim_pool);
    pool_destroy(&high_pool);
    pool_destroy(&low_pool);

    return EXIT_SUCCESS;

fail:
    fprintf(stderr,
            "\n[FAIL] oracle bank-overlap experiment aborted\n");

    cgroup_context_destroy(&cgroups);

    free(permutation);

    free(victim_hist);
    free(high_hist);
    free(low_hist);

    free(victim_classes);
    free(low_classes);

    free(victim_counts);
    free(high_counts);
    free(low_counts);

    free(offset_classes);

    line_set_destroy(&victim_set);
    line_set_destroy(&high_set);
    line_set_destroy(&low_set);

    pool_destroy(&victim_pool);
    pool_destroy(&high_pool);
    pool_destroy(&low_pool);

    return EXIT_FAILURE;
}

