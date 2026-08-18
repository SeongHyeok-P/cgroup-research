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
    CONDITION_OVERLAP_0,
    CONDITION_OVERLAP_25,
    CONDITION_OVERLAP_50,
    CONDITION_OVERLAP_75,
    CONDITION_OVERLAP_100,
    CONDITION_COUNT
};

struct config {
    size_t pool_mib;
    size_t workset_mib;
    size_t group_classes;

    unsigned int seconds;

    int victim_cpu;
    int aggressor_cpu;

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
    struct worker_result aggressor;

    bool has_aggressor;
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

    case CONDITION_OVERLAP_0:
        return "o0";

    case CONDITION_OVERLAP_25:
        return "o25";

    case CONDITION_OVERLAP_50:
        return "o50";

    case CONDITION_OVERLAP_75:
        return "o75";

    case CONDITION_OVERLAP_100:
        return "o100";

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

    if (strcmp(text, "o0") == 0) {
        *out = CONDITION_OVERLAP_0;
        return 0;
    }

    if (strcmp(text, "o25") == 0) {
        *out = CONDITION_OVERLAP_25;
        return 0;
    }

    if (strcmp(text, "o50") == 0) {
        *out = CONDITION_OVERLAP_50;
        return 0;
    }

    if (strcmp(text, "o75") == 0) {
        *out = CONDITION_OVERLAP_75;
        return 0;
    }

    if (strcmp(text, "o100") == 0) {
        *out = CONDITION_OVERLAP_100;
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

static int condition_shared_classes(
    enum condition condition,
    size_t group_classes,
    size_t *shared_out)
{
    size_t shared;

    if (shared_out == NULL ||
        group_classes == 0U ||
        group_classes % 4U != 0U) {
        return -1;
    }

    switch (condition) {
    case CONDITION_OVERLAP_0:
        shared = 0U;
        break;

    case CONDITION_OVERLAP_25:
        shared = group_classes / 4U;
        break;

    case CONDITION_OVERLAP_50:
        shared = group_classes / 2U;
        break;

    case CONDITION_OVERLAP_75:
        shared = (group_classes * 3U) / 4U;
        break;

    case CONDITION_OVERLAP_100:
        shared = group_classes;
        break;

    default:
        return -1;
    }

    *shared_out = shared;
    return 0;
}

static int build_overlap_classes(
    const uint64_t *victim_classes,
    const uint64_t *disjoint_classes,
    size_t group_classes,
    size_t shared_classes,
    uint64_t *out)
{
    size_t i;
    size_t disjoint_needed;

    if (victim_classes == NULL ||
        disjoint_classes == NULL ||
        out == NULL ||
        group_classes == 0U ||
        shared_classes > group_classes) {
        return -1;
    }

    disjoint_needed =
        group_classes - shared_classes;

    for (i = 0U; i < shared_classes; ++i) {
        out[i] = victim_classes[i];
    }

    for (i = 0U; i < disjoint_needed; ++i) {
        out[shared_classes + i] =
            disjoint_classes[i];
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
    cfg->aggressor_cpu = 12;

    cfg->seed = 20260814ULL;

    cfg->order[0] = CONDITION_NONE;
    cfg->order[1] = CONDITION_OVERLAP_0;
    cfg->order[2] = CONDITION_OVERLAP_25;
    cfg->order[3] = CONDITION_OVERLAP_50;
    cfg->order[4] = CONDITION_OVERLAP_75;
    cfg->order[5] = CONDITION_OVERLAP_100;
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
            "[--aggressor-cpu N] "
            "[--seed N] "
            "[--order none,o0,o25,o50,o75,o100]\n"
            "\n"
            "Notes:\n"
            "  --classes must be divisible by 4.\n"
            "  o25 means 25%% of the victim bank classes are shared,\n"
            "  not weighted-Jaccard=0.25.\n",
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

        if (strcmp(argv[i], "--aggressor-cpu") == 0) {
            if (++i >= argc ||
                parse_int(argv[i], &cfg->aggressor_cpu) != 0) {
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

static int choose_base_class_sets(
    const uint64_t *victim_counts,
    const uint64_t *aggressor_counts,
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
    size_t i;

    if (victim_counts == NULL ||
        aggressor_counts == NULL ||
        victim_classes == NULL ||
        disjoint_classes == NULL ||
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
     * Both the victim class set and the disjoint class set come
     * from one common eligibility population.
     *
     * Every candidate class must have at least quota_lines in BOTH
     * the victim pool and the aggressor pool.  This avoids selecting
     * overlap classes from unusually dense regions while selecting
     * disjoint classes from sparse regions.
     */
    for (cls = 0U;
         cls < class_space;
         ++cls) {
        if (victim_counts[cls] >=
                (uint64_t)quota_lines &&
            aggressor_counts[cls] >=
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

        disjoint_classes[i] =
            candidates[group_classes + i];
    }

    free(candidates);

    return 0;
}

static size_t histogram_shared_classes(
    const uint64_t *a,
    const uint64_t *b,
    size_t class_space)
{
    size_t shared = 0U;
    size_t i;

    if (a == NULL || b == NULL) {
        return 0U;
    }

    for (i = 0U; i < class_space; ++i) {
        if (a[i] != 0U && b[i] != 0U) {
            ++shared;
        }
    }

    return shared;
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
    uintptr_t victim_root,
    const uintptr_t aggressor_roots[AGGRESSOR_STREAMS],
    struct condition_result *out)
{
    pid_t victim_pid = -1;
    pid_t aggressor_pid = -1;

    int victim_fd = -1;
    int aggressor_fd = -1;

    if (cfg == NULL ||
        out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (condition != CONDITION_NONE) {
        if (aggressor_roots == NULL) {
            return -1;
        }

        out->has_aggressor = true;
    }

    victim_pid = spawn_victim(
        cfg->victim_cpu,
        cfg->seconds,
        victim_root,
        &victim_fd);

    if (victim_pid < 0) {
        goto fail;
    }

    if (wait_until_stopped(
            victim_pid) != 0) {
        goto fail;
    }

    if (out->has_aggressor) {
        aggressor_pid =
            spawn_aggressor(
                cfg->aggressor_cpu,
                cfg->seconds,
                aggressor_roots,
                &aggressor_fd);

        if (aggressor_pid < 0) {
            goto fail;
        }

        if (wait_until_stopped(
                aggressor_pid) != 0) {
            goto fail;
        }

        /*
         * Start aggressor first.
         *
         * Victim follows immediately.
         * Both then perform the same one-second warmup.
         */
        if (kill(
                aggressor_pid,
                SIGCONT) != 0) {
            goto fail;
        }
    }

    if (kill(
            victim_pid,
            SIGCONT) != 0) {
        goto fail;
    }

    if (wait_success(
            victim_pid) != 0) {
        goto fail;
    }

    victim_pid = -1;

    if (out->has_aggressor) {
        if (wait_success(
                aggressor_pid) != 0) {
            goto fail;
        }

        aggressor_pid = -1;
    }

    if (read_full(
            victim_fd,
            &out->victim,
            sizeof(out->victim)) != 0) {
        goto fail;
    }

    (void)close(victim_fd);
    victim_fd = -1;

    if (out->has_aggressor) {
        if (read_full(
                aggressor_fd,
                &out->aggressor,
                sizeof(out->aggressor)) != 0) {
            goto fail;
        }

        (void)close(aggressor_fd);
        aggressor_fd = -1;
    }

    return 0;

fail:
    if (victim_pid > 0) {
        (void)kill(
            victim_pid,
            SIGCONT);
        (void)kill(
            victim_pid,
            SIGKILL);
        (void)waitpid(
            victim_pid,
            NULL,
            0);
    }

    if (aggressor_pid > 0) {
        (void)kill(
            aggressor_pid,
            SIGCONT);
        (void)kill(
            aggressor_pid,
            SIGKILL);
        (void)waitpid(
            aggressor_pid,
            NULL,
            0);
    }

    if (victim_fd >= 0) {
        (void)close(victim_fd);
    }

    if (aggressor_fd >= 0) {
        (void)close(aggressor_fd);
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

    struct memory_pool victim_pool;
    struct memory_pool aggressor_pool;

    struct line_set victim_set;
    struct line_set aggressor_set;

    uint64_t *offset_classes = NULL;

    uint64_t *victim_counts = NULL;
    uint64_t *aggressor_counts = NULL;

    uint64_t *victim_classes = NULL;
    uint64_t *disjoint_classes = NULL;
    uint64_t *aggressor_classes = NULL;

    uint64_t *victim_hist = NULL;
    uint64_t *aggressor_hist = NULL;

    size_t *permutation = NULL;

    uintptr_t victim_root = 0U;
    uintptr_t aggressor_roots[AGGRESSOR_STREAMS];

    struct condition_result results[CONDITION_COUNT];
    double measured_jaccard[CONDITION_COUNT];
    size_t measured_shared_classes[CONDITION_COUNT];

    long page_size_long;
    size_t page_size;

    size_t class_space;
    size_t lines_per_page;

    size_t workset_lines;
    size_t quota_lines;

    size_t i;

    memset(&victim_pool, 0, sizeof(victim_pool));
    memset(&aggressor_pool, 0, sizeof(aggressor_pool));
    memset(&victim_set, 0, sizeof(victim_set));
    memset(&aggressor_set, 0, sizeof(aggressor_set));
    memset(aggressor_roots, 0, sizeof(aggressor_roots));
    memset(results, 0, sizeof(results));
    memset(measured_jaccard, 0, sizeof(measured_jaccard));
    memset(measured_shared_classes, 0, sizeof(measured_shared_classes));

    if (parse_args(
            argc,
            argv,
            &cfg) != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (cfg.group_classes % 4U != 0U) {
        fprintf(stderr,
                "[ERROR] --classes must be divisible by 4 for 0/25/50/75/100 sweep\n");
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
    printf("Oracle DRAM bank-overlap sweep experiment v1\n");
    printf("============================================================\n");

    printf("[CONFIG] self_test=%s\n",
           cfg.self_test ? "true" : "false");
    printf("[CONFIG] page_size=%zu\n",
           page_size);
    printf("[CONFIG] pool_mib_each=%zu\n",
           cfg.pool_mib);
    printf("[CONFIG] pool_total_mib=%zu\n",
           cfg.pool_mib * 2U);
    printf("[CONFIG] workset_mib_each=%zu\n",
           cfg.workset_mib);
    printf("[CONFIG] group_classes=%zu\n",
           cfg.group_classes);
    printf("[CONFIG] seconds_per_condition=%u\n",
           cfg.seconds);
    printf("[CONFIG] victim_cpu=%d\n",
           cfg.victim_cpu);
    printf("[CONFIG] aggressor_cpu=%d\n",
           cfg.aggressor_cpu);
    printf("[CONFIG] seed=%" PRIu64 "\n",
           cfg.seed);

    printf("[CONFIG] order=");
    for (i = 0U; i < CONDITION_COUNT; ++i) {
        if (i != 0U) {
            printf(",");
        }
        printf("%s", condition_name(cfg.order[i]));
    }
    printf("\n");

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
     * Two independent physical pools:
     *
     *   victim_pool    : protected workload only
     *   aggressor_pool : reused across overlap levels
     *
     * Reusing one aggressor pool keeps the physical candidate
     * population fixed while only the bank-class composition changes.
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
            &aggressor_pool,
            "aggressor",
            cfg.pool_mib,
            page_size,
            cfg.seed ^
                0x2222222222222222ULL) != 0) {
        goto fail;
    }

    victim_counts =
        calloc(
            class_space,
            sizeof(victim_counts[0]));

    aggressor_counts =
        calloc(
            class_space,
            sizeof(aggressor_counts[0]));

    victim_classes =
        calloc(
            cfg.group_classes,
            sizeof(victim_classes[0]));

    disjoint_classes =
        calloc(
            cfg.group_classes,
            sizeof(disjoint_classes[0]));

    aggressor_classes =
        calloc(
            cfg.group_classes,
            sizeof(aggressor_classes[0]));

    victim_hist =
        calloc(
            class_space,
            sizeof(victim_hist[0]));

    aggressor_hist =
        calloc(
            class_space,
            sizeof(aggressor_hist[0]));

    if (victim_counts == NULL ||
        aggressor_counts == NULL ||
        victim_classes == NULL ||
        disjoint_classes == NULL ||
        aggressor_classes == NULL ||
        victim_hist == NULL ||
        aggressor_hist == NULL) {
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
            &aggressor_pool,
            &mapping,
            offset_classes,
            lines_per_page,
            aggressor_counts,
            class_space) != 0) {
        goto fail;
    }

    if (choose_base_class_sets(
            victim_counts,
            aggressor_counts,
            class_space,
            cfg.group_classes,
            quota_lines,
            cfg.seed,
            victim_classes,
            disjoint_classes) != 0) {
        goto fail;
    }

    for (i = 0U;
         i < cfg.group_classes;
         ++i) {
        printf("[CLASS-BASE] slot=%zu "
               "victim_class=0x%" PRIx64 " "
               "disjoint_class=0x%" PRIx64 " "
               "victim_pool_lines=%" PRIu64 " "
               "aggressor_victim_class_lines=%" PRIu64 " "
               "aggressor_disjoint_class_lines=%" PRIu64 " "
               "quota=%zu\n",
               i,
               victim_classes[i],
               disjoint_classes[i],
               victim_counts[victim_classes[i]],
               aggressor_counts[victim_classes[i]],
               aggressor_counts[disjoint_classes[i]],
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

    printf("[SELECT] victim_lines=%zu\n",
           victim_set.line_count);

    if (build_histogram(
            &victim_set,
            &mapping,
            victim_hist,
            class_space) != 0) {
        goto fail;
    }

    if (histogram_unique_classes(
            victim_hist,
            class_space) !=
        cfg.group_classes) {
        fprintf(stderr,
                "[ERROR] victim unique-class count mismatch\n");
        goto fail;
    }

    if (make_permutation(
            workset_lines,
            cfg.seed ^
                0x7777777777777777ULL,
            &permutation) != 0) {
        goto fail;
    }

    if (build_single_chain(
            &victim_set,
            permutation,
            &victim_root) != 0) {
        goto fail;
    }

    printf("\n");
    printf("RESULT_CSV_HEADER,"
           "condition,"
           "shared_classes,"
           "shared_class_fraction,"
           "weighted_jaccard,"
           "victim_elapsed_sec,"
           "victim_reads,"
           "victim_ns_per_read,"
           "aggressor_elapsed_sec,"
           "aggressor_passes,"
           "aggressor_load_ops,"
           "aggressor_Mload_per_sec\n");

    for (i = 0U;
         i < CONDITION_COUNT;
         ++i) {
        enum condition condition =
            cfg.order[i];

        struct condition_result result;
        size_t shared_classes = 0U;
        double shared_fraction = 0.0;
        double jaccard = 0.0;
        uint64_t intersection = 0U;
        uint64_t union_count = 0U;
        uint64_t expected_intersection = 0U;
        uint64_t expected_union = 0U;
        size_t actual_shared_classes = 0U;

        double victim_ns;
        uint64_t aggressor_passes = 0U;
        double aggressor_mload = 0.0;

        memset(&result, 0, sizeof(result));

        if (condition != CONDITION_NONE) {
            if (condition_shared_classes(
                    condition,
                    cfg.group_classes,
                    &shared_classes) != 0) {
                goto fail;
            }

            shared_fraction =
                (double)shared_classes /
                (double)cfg.group_classes;

            if (build_overlap_classes(
                    victim_classes,
                    disjoint_classes,
                    cfg.group_classes,
                    shared_classes,
                    aggressor_classes) != 0) {
                goto fail;
            }

            line_set_destroy(
                &aggressor_set);

            if (collect_line_set(
                    &aggressor_set,
                    &aggressor_pool,
                    &mapping,
                    offset_classes,
                    lines_per_page,
                    class_space,
                    aggressor_classes,
                    cfg.group_classes,
                    quota_lines,
                    /*
                     * Deliberately use the SAME address-selection order
                     * for every overlap level.  For a bank class shared
                     * across two conditions, this keeps the selected
                     * aggressor cache lines stable across those conditions.
                     */
                    cfg.seed ^
                        0x5555555555555555ULL) != 0) {
                goto fail;
            }

            if (build_histogram(
                    &aggressor_set,
                    &mapping,
                    aggressor_hist,
                    class_space) != 0) {
                goto fail;
            }

            if (histogram_unique_classes(
                    aggressor_hist,
                    class_space) !=
                cfg.group_classes) {
                fprintf(stderr,
                        "[ERROR] aggressor unique-class count mismatch\n");
                goto fail;
            }

            jaccard =
                histogram_jaccard(
                    victim_hist,
                    aggressor_hist,
                    class_space,
                    &intersection,
                    &union_count);

            actual_shared_classes =
                histogram_shared_classes(
                    victim_hist,
                    aggressor_hist,
                    class_space);

            expected_intersection =
                (uint64_t)shared_classes *
                (uint64_t)quota_lines;

            expected_union =
                (uint64_t)(
                    cfg.group_classes * 2U -
                    shared_classes) *
                (uint64_t)quota_lines;

            printf("[OVERLAP] condition=%s "
                   "shared_classes=%zu/%zu "
                   "shared_fraction=%.6f "
                   "intersection=%" PRIu64 " "
                   "union=%" PRIu64 " "
                   "weighted_jaccard=%.6f\n",
                   condition_name(condition),
                   actual_shared_classes,
                   cfg.group_classes,
                   shared_fraction,
                   intersection,
                   union_count,
                   jaccard);

            /*
             * Structural acceptance criterion:
             * verify the ACTUAL cache-line histogram, not only
             * the requested class list.
             */
            if (actual_shared_classes !=
                    shared_classes ||
                intersection !=
                    expected_intersection ||
                union_count !=
                    expected_union) {
                fprintf(stderr,
                        "[ERROR] actual overlap does not match requested sweep level\n");
                goto fail;
            }

            if (build_multi_chain(
                    &aggressor_set,
                    permutation,
                    AGGRESSOR_STREAMS,
                    aggressor_roots) != 0) {
                goto fail;
            }

            measured_jaccard[condition] =
                jaccard;
            measured_shared_classes[condition] =
                actual_shared_classes;
        }

        printf("\n");
        printf("============================================================\n");
        printf("[RUN] condition=%s seconds=%u\n",
               condition_name(condition),
               cfg.seconds);
        printf("[RUN] victim_cpu=%d\n",
               cfg.victim_cpu);

        if (condition != CONDITION_NONE) {
            printf("[RUN] aggressor_cpu=%d\n",
                   cfg.aggressor_cpu);
        }

        printf("============================================================\n");

        if (run_condition(
                condition,
                &cfg,
                victim_root,
                condition == CONDITION_NONE ?
                    NULL :
                    aggressor_roots,
                &result) != 0) {
            goto fail;
        }

        results[condition] =
            result;

        victim_ns =
            result.victim.elapsed_sec *
            1000000000.0 /
            (double)result.victim.operations;

        if (result.has_aggressor) {
            aggressor_passes =
                result.aggressor.operations /
                (uint64_t)workset_lines;

            aggressor_mload =
                (double)
                    result.aggressor.operations /
                result.aggressor.elapsed_sec /
                1000000.0;
        }

        printf("[RESULT] condition=%s\n",
               condition_name(condition));
        printf("[RESULT] shared_classes=%zu/%zu\n",
               measured_shared_classes[condition],
               cfg.group_classes);
        printf("[RESULT] weighted_jaccard=%.6f\n",
               measured_jaccard[condition]);
        printf("[RESULT] victim_elapsed_sec=%.6f\n",
               result.victim.elapsed_sec);
        printf("[RESULT] victim_reads=%" PRIu64 "\n",
               result.victim.operations);
        printf("[RESULT] victim_ns_per_read=%.3f\n",
               victim_ns);

        if (result.has_aggressor) {
            printf("[RESULT] aggressor_elapsed_sec=%.6f\n",
                   result.aggressor.elapsed_sec);
            printf("[RESULT] aggressor_passes=%" PRIu64 "\n",
                   aggressor_passes);
            printf("[RESULT] aggressor_load_ops=%" PRIu64 "\n",
                   result.aggressor.operations);
            printf("[RESULT] aggressor_Mload_per_sec=%.3f\n",
                   aggressor_mload);
        }

        printf("RESULT_CSV,%s,%zu,%.6f,%.6f,"
               "%.6f,%" PRIu64 ",%.3f,"
               "%.6f,%" PRIu64 ",%" PRIu64 ",%.3f\n",
               condition_name(condition),
               measured_shared_classes[condition],
               condition == CONDITION_NONE ?
                   0.0 :
                   (double)measured_shared_classes[condition] /
                       (double)cfg.group_classes,
               measured_jaccard[condition],
               result.victim.elapsed_sec,
               result.victim.operations,
               victim_ns,
               result.has_aggressor ?
                   result.aggressor.elapsed_sec :
                   0.0,
               aggressor_passes,
               result.has_aggressor ?
                   result.aggressor.operations :
                   0U,
               aggressor_mload);
    }

    {
        double none_ns =
            results[CONDITION_NONE].
                victim.elapsed_sec *
            1000000000.0 /
            (double)
                results[CONDITION_NONE].
                    victim.operations;

        enum condition overlap_conditions[] = {
            CONDITION_OVERLAP_0,
            CONDITION_OVERLAP_25,
            CONDITION_OVERLAP_50,
            CONDITION_OVERLAP_75,
            CONDITION_OVERLAP_100
        };

        size_t c;

        printf("\n");
        printf("[SWEEP-SUMMARY] baseline_ns=%.3f\n",
               none_ns);

        for (c = 0U;
             c < sizeof(overlap_conditions) /
                     sizeof(overlap_conditions[0]);
             ++c) {
            enum condition condition =
                overlap_conditions[c];

            double ns =
                results[condition].
                    victim.elapsed_sec *
                1000000000.0 /
                (double)
                    results[condition].
                        victim.operations;

            double slowdown =
                (ns / none_ns - 1.0) *
                100.0;

            double rate =
                (double)
                    results[condition].
                        aggressor.operations /
                results[condition].
                    aggressor.elapsed_sec /
                1000000.0;

            printf("[SWEEP-SUMMARY] condition=%s "
                   "shared_classes=%zu/%zu "
                   "weighted_jaccard=%.6f "
                   "victim_ns=%.3f "
                   "slowdown_vs_none_pct=%.3f "
                   "aggressor_Mload_per_sec=%.3f\n",
                   condition_name(condition),
                   measured_shared_classes[condition],
                   cfg.group_classes,
                   measured_jaccard[condition],
                   ns,
                   slowdown,
                   rate);
        }
    }

    {
        uint64_t victim_moved;
        uint64_t victim_unreadable;
        uint64_t aggressor_moved;
        uint64_t aggressor_unreadable;

        if (verify_pool_pfns(
                &victim_pool,
                &victim_moved,
                &victim_unreadable) != 0 ||
            verify_pool_pfns(
                &aggressor_pool,
                &aggressor_moved,
                &aggressor_unreadable) != 0) {
            goto fail;
        }

        printf("[VERIFY] victim_pfn_moved=%" PRIu64
               " unreadable=%" PRIu64 "\n",
               victim_moved,
               victim_unreadable);

        printf("[VERIFY] aggressor_pfn_moved=%" PRIu64
               " unreadable=%" PRIu64 "\n",
               aggressor_moved,
               aggressor_unreadable);

        if (victim_moved != 0U ||
            aggressor_moved != 0U ||
            victim_unreadable != 0U ||
            aggressor_unreadable != 0U) {
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
        printf("  - victim/aggressor use separate physical pools\n");
        printf("  - common eligible class population was used\n");
        printf("  - actual 0/25/50/75/100 overlap levels were verified\n");
        printf("  - pointer metadata lives inside selected lines\n");
        printf("  - PFNs remained stable\n");
        printf("\n");
        printf("NOTE: latency monotonicity is NOT a self-test acceptance criterion.\n");
        printf("============================================================\n");
    }

    free(permutation);

    free(victim_hist);
    free(aggressor_hist);

    free(victim_classes);
    free(disjoint_classes);
    free(aggressor_classes);

    free(victim_counts);
    free(aggressor_counts);

    free(offset_classes);

    line_set_destroy(&victim_set);
    line_set_destroy(&aggressor_set);

    pool_destroy(&victim_pool);
    pool_destroy(&aggressor_pool);

    return EXIT_SUCCESS;

fail:
    fprintf(stderr,
            "\n[FAIL] oracle bank-overlap sweep experiment aborted\n");

    free(permutation);

    free(victim_hist);
    free(aggressor_hist);

    free(victim_classes);
    free(disjoint_classes);
    free(aggressor_classes);

    free(victim_counts);
    free(aggressor_counts);

    free(offset_classes);

    line_set_destroy(&victim_set);
    line_set_destroy(&aggressor_set);

    pool_destroy(&victim_pool);
    pool_destroy(&aggressor_pool);

    return EXIT_FAILURE;
}

