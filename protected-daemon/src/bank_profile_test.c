#define _GNU_SOURCE

#include "bank_profile.h"
#include "calibration.h"
#include "dram_mapping.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEFAULT_RESULT_PATH "/run/protected-daemon/dram-map.json"
#define DEFAULT_PAGES 4096U
#define DEFAULT_STRIDE 16U
#define DEFAULT_MAX_SAMPLES 0U
#define DEFAULT_PRINT_ENTRIES 64U

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--pages N] [--stride N] [--max-samples N] [--print N]\n"
            "\n"
            "This test:\n"
            "  1. Loads " DEFAULT_RESULT_PATH "\n"
            "  2. Initializes dram_mapping\n"
            "  3. Allocates and touches anonymous memory\n"
            "  4. Builds a process-level bank_class profile for itself\n"
            "  5. Prints the resulting bank_class histogram\n"
            "\n"
            "Options:\n"
            "  --pages N        Number of anonymous pages to mmap/touch, default 4096\n"
            "  --stride N       Sample every N pages in selected VMAs, default 16\n"
            "  --max-samples N  Maximum pages to sample from process, default 0 = unlimited\n"
            "  --print N        Number of bank_class histogram entries to print, default 64\n",
            prog);
}

static int parse_size_arg(const char *s, size_t *out)
{
    char *end = NULL;
    unsigned long value;

    if (s == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }

    *out = (size_t)value;
    return 0;
}

static int parse_args(
    int argc,
    char **argv,
    size_t *pages,
    size_t *stride,
    size_t *max_samples,
    size_t *print_entries)
{
    int i;

    if (pages == NULL || stride == NULL ||
        max_samples == NULL || print_entries == NULL) {
        return -1;
    }

    *pages = DEFAULT_PAGES;
    *stride = DEFAULT_STRIDE;
    *max_samples = DEFAULT_MAX_SAMPLES;
    *print_entries = DEFAULT_PRINT_ENTRIES;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pages") == 0) {
            if (i + 1 >= argc || parse_size_arg(argv[i + 1], pages) < 0 ||
                *pages == 0U) {
                return -1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--stride") == 0) {
            if (i + 1 >= argc || parse_size_arg(argv[i + 1], stride) < 0 ||
                *stride == 0U) {
                return -1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--max-samples") == 0) {
            if (i + 1 >= argc || parse_size_arg(argv[i + 1], max_samples) < 0) {
                return -1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--print") == 0) {
            if (i + 1 >= argc || parse_size_arg(argv[i + 1], print_entries) < 0) {
                return -1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }

        return -1;
    }

    return 0;
}

static int load_dram_mapping(
    struct calibration_result *calibration,
    struct dram_mapping *mapping)
{
    int rc;
    size_t i;

    if (calibration == NULL || mapping == NULL) {
        return EXIT_FAILURE;
    }

    calibration_result_reset(calibration);
    dram_mapping_reset(mapping);

    rc = calibration_load_result(DEFAULT_RESULT_PATH, calibration);
    if (rc != CALIBRATION_OK) {
        fprintf(stderr,
                "[ERROR] failed to load calibration JSON %s: %s\n",
                DEFAULT_RESULT_PATH,
                calibration_strerror(rc));
        fprintf(stderr,
                "[HINT] Run protected_daemon or addr_translate_dram_test first "
                "to create a valid dram-map.json.\n");
        return EXIT_FAILURE;
    }

    rc = dram_mapping_init_from_calibration(mapping, calibration);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr,
                "[ERROR] dram_mapping_init_from_calibration failed: %s\n",
                dram_mapping_strerror(rc));
        return EXIT_FAILURE;
    }

    printf("[INFO] DRAM mapping ready\n");
    printf("[INFO] mask_set_id=%s mask_count=%zu\n",
           mapping->mask_set_id,
           mapping->mask_count);
    printf("[INFO] holdout precision=%.4f recall=%.4f\n",
           calibration->holdout.precision,
           calibration->holdout.recall);

    for (i = 0; i < mapping->mask_count; i++) {
        printf("[INFO] bank_mask[%zu]=0x%016" PRIx64 "\n",
               i,
               mapping->masks[i]);
    }

    return EXIT_SUCCESS;
}

static void *allocate_and_touch_pages(size_t pages, size_t *mapping_size_out)
{
    long page_size_long;
    size_t page_size;
    size_t mapping_size;
    unsigned char *buf;
    size_t i;
    volatile uint64_t checksum = 0U;

    if (pages == 0U || mapping_size_out == NULL) {
        return MAP_FAILED;
    }

    page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        fprintf(stderr, "[ERROR] sysconf(_SC_PAGESIZE) failed\n");
        return MAP_FAILED;
    }

    page_size = (size_t)page_size_long;

    if (pages > SIZE_MAX / page_size) {
        fprintf(stderr, "[ERROR] mmap size overflow\n");
        return MAP_FAILED;
    }

    mapping_size = pages * page_size;

    buf = mmap(NULL,
               mapping_size,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return MAP_FAILED;
    }

    /*
     * Force physical allocation.
     * mmap alone reserves virtual address space, but each page may still be
     * not-present until it is touched.
     */
    for (i = 0; i < pages; i++) {
        size_t offset = i * page_size;

        buf[offset] = (unsigned char)(0x30U + (i & 0x3fU));
        checksum += buf[offset];
    }

    printf("[INFO] touched anonymous mapping: ptr=%p pages=%zu bytes=%zu checksum=%" PRIu64 "\n",
           (void *)buf,
           pages,
           mapping_size,
           (uint64_t)checksum);

    *mapping_size_out = mapping_size;
    return buf;
}

int main(int argc, char **argv)
{
    struct calibration_result calibration;
    struct dram_mapping mapping;
    struct bank_profile_config cfg;
    struct bank_profile profile;
    size_t pages;
    size_t stride;
    size_t max_samples;
    size_t print_entries;
    size_t mapping_size = 0U;
    void *buffer;
    int rc;

    if (parse_args(argc,
                   argv,
                   &pages,
                   &stride,
                   &max_samples,
                   &print_entries) < 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    printf("[INFO] bank_profile_test starting\n");
    printf("[INFO] target pid=%ld\n", (long)getpid());

    if (load_dram_mapping(&calibration, &mapping) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    buffer = allocate_and_touch_pages(pages, &mapping_size);
    if (buffer == MAP_FAILED) {
        return EXIT_FAILURE;
    }

    bank_profile_config_default(&cfg);
    cfg.sample_stride_pages = stride;
    cfg.max_pages_to_sample = max_samples;
    cfg.include_anonymous = true;
    cfg.include_heap = true;
    cfg.include_stack = true;
    cfg.include_file_backed = false;
    cfg.include_readonly = false;

    printf("[INFO] profile config: stride=%zu max_samples=%zu "
           "anon=%s heap=%s stack=%s file_backed=%s readonly=%s\n",
           cfg.sample_stride_pages,
           cfg.max_pages_to_sample,
           cfg.include_anonymous ? "true" : "false",
           cfg.include_heap ? "true" : "false",
           cfg.include_stack ? "true" : "false",
           cfg.include_file_backed ? "true" : "false",
           cfg.include_readonly ? "true" : "false");

    rc = bank_profile_build(getpid(), &mapping, &cfg, &profile);
    if (rc != BANK_PROFILE_OK) {
        fprintf(stderr,
                "[ERROR] bank_profile_build failed: %s\n",
                bank_profile_strerror(rc));
        fprintf(stderr,
                "[ERROR] detail: %s\n",
                profile.error_message);
        (void)munmap(buffer, mapping_size);
        return EXIT_FAILURE;
    }

    printf("\n[INFO] process bank profile\n");
    bank_profile_print(stdout, &profile, print_entries);

    printf("\n[INFO] sanity checks\n");
    printf("[INFO] pages_translated > 0: %s\n",
           profile.pages_translated > 0U ? "true" : "false");
    printf("[INFO] entry_count > 0: %s\n",
           profile.entry_count > 0U ? "true" : "false");
    printf("[INFO] dominant_bank_class=0x%" PRIx64
           " dominant_fraction=%.4f\n",
           profile.dominant_bank_class,
           profile.dominant_fraction);

    if (profile.pages_translated == 0U || profile.entry_count == 0U) {
        fprintf(stderr, "[ERROR] profile is empty\n");
        (void)munmap(buffer, mapping_size);
        return EXIT_FAILURE;
    }

    (void)munmap(buffer, mapping_size);

    printf("\n[INFO] bank_profile_test passed\n");
    return EXIT_SUCCESS;
}
