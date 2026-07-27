#define _POSIX_C_SOURCE 200809L

#include "calibration.h"
#include "dram_mapping.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static void print_mapping(const struct dram_mapping *mapping)
{
    size_t i;

    printf("\n=== DRAM Mapping ===\n");
    printf("ready       : %s\n",
           dram_mapping_is_ready(mapping) ? "true" : "false");
    printf("mask count  : %zu\n", mapping->mask_count);
    printf("mask set id : %s\n", mapping->mask_set_id);

    for (i = 0U; i < mapping->mask_count; ++i) {
        printf("mask[%zu]    : 0x%016" PRIx64 "\n",
               i,
               mapping->masks[i]);
    }
}

static int test_one_pa(const struct dram_mapping *mapping,
                       uint64_t pa)
{
    uint64_t bank_class = 0U;
    int rc = dram_mapping_bank_class(mapping, pa, &bank_class);

    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr,
                "dram_mapping_bank_class failed for PA "
                "0x%016" PRIx64 ": %s\n",
                pa,
                dram_mapping_strerror(rc));
        return -1;
    }

    printf("PA 0x%016" PRIx64
           " -> bank_class 0x%016" PRIx64 "\n",
           pa,
           bank_class);

    return 0;
}

static int test_pair(const struct dram_mapping *mapping,
                     uint64_t pa1,
                     uint64_t pa2)
{
    uint64_t cls1 = 0U;
    uint64_t cls2 = 0U;
    bool same = false;
    int rc;

    rc = dram_mapping_bank_class(mapping, pa1, &cls1);
    if (rc != DRAM_MAPPING_OK) {
        return -1;
    }

    rc = dram_mapping_bank_class(mapping, pa2, &cls2);
    if (rc != DRAM_MAPPING_OK) {
        return -1;
    }

    rc = dram_mapping_same_bank_class(mapping, pa1, pa2, &same);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr,
                "same-class comparison failed: %s\n",
                dram_mapping_strerror(rc));
        return -1;
    }

    printf("\nPAIR TEST\n");
    printf("  PA1   : 0x%016" PRIx64 "\n", pa1);
    printf("  class : 0x%016" PRIx64 "\n", cls1);
    printf("  PA2   : 0x%016" PRIx64 "\n", pa2);
    printf("  class : 0x%016" PRIx64 "\n", cls2);
    printf("  same? : %s\n", same ? "true" : "false");

    if (same != (cls1 == cls2)) {
        fprintf(stderr,
                "ERROR: direct same-class result disagrees "
                "with class equality\n");
        return -1;
    }

    return 0;
}

int main(void)
{
    struct calibration_config cfg = {
        .worker_path = "../scripts/kk_calibrate.py",
        .kk_main_path = "../../Knock-Knock/main",
        .result_path = "/run/protected-daemon/dram-map.json",
        .work_dir = "/run/protected-daemon/kk-calibration",
        .log_path = "/run/protected-daemon/calibration.log",

        .runs = 3U,
        .memory_percent = 25.0,
        .measurements = 500U,
        .timing_rounds = 50U,
        .timeout_sec = 1800U,

        .keep_csv = true
    };

    struct calibration_result calib;
    struct dram_mapping mapping;
    const uint64_t test_pas[] = {
        0x0000000012345000ULL,
        0x0000000012346000ULL,
        0x0000000023456000ULL,
        0x0000000034567000ULL,
        0x0000000045678000ULL
    };

    size_t i;
    int rc;

    printf("=== Step 1: Run calibration ===\n");

    rc = calibration_run(&cfg, &calib);
    if (rc != CALIBRATION_OK) {
        fprintf(stderr,
                "calibration failed: %s\n",
                calibration_strerror(rc));

        if (calib.error_message[0] != '\0') {
            fprintf(stderr,
                    "detail: %s\n",
                    calib.error_message);
        }

        return EXIT_FAILURE;
    }

    printf("Calibration success\n");
    printf("Mask count : %zu\n", calib.bank_mask_count);
    printf("Precision  : %.4f\n", calib.holdout.precision);
    printf("Recall     : %.4f\n", calib.holdout.recall);

    printf("\n=== Step 2: Initialize DRAM mapping ===\n");

    dram_mapping_reset(&mapping);

    rc = dram_mapping_init_from_calibration(&mapping, &calib);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr,
                "dram_mapping_init_from_calibration failed: %s\n",
                dram_mapping_strerror(rc));
        return EXIT_FAILURE;
    }

    print_mapping(&mapping);

    printf("\n=== Step 3: PA -> bank class ===\n");

    for (i = 0U;
         i < sizeof(test_pas) / sizeof(test_pas[0]);
         ++i) {
        if (test_one_pa(&mapping, test_pas[i]) != 0) {
            return EXIT_FAILURE;
        }
    }

    printf("\n=== Step 4: Pair comparison ===\n");

    if (test_pair(&mapping, test_pas[0], test_pas[0]) != 0) {
        return EXIT_FAILURE;
    }

    if (test_pair(&mapping, test_pas[0], test_pas[1]) != 0) {
        return EXIT_FAILURE;
    }

    if (test_pair(&mapping, test_pas[0], test_pas[4]) != 0) {
        return EXIT_FAILURE;
    }

    printf("\n=== Step 5: Fast-path consistency ===\n");

    for (i = 0U;
         i < sizeof(test_pas) / sizeof(test_pas[0]);
         ++i) {
        uint64_t checked = 0U;
        uint64_t fast;

        rc = dram_mapping_bank_class(&mapping,
                                     test_pas[i],
                                     &checked);
        if (rc != DRAM_MAPPING_OK) {
            fprintf(stderr,
                    "checked path failed: %s\n",
                    dram_mapping_strerror(rc));
            return EXIT_FAILURE;
        }

        fast = dram_mapping_bank_class_fast(&mapping, test_pas[i]);

        printf("PA 0x%016" PRIx64
               " checked=0x%016" PRIx64
               " fast=0x%016" PRIx64 "\n",
               test_pas[i],
               checked,
               fast);

        if (checked != fast) {
            fprintf(stderr,
                    "ERROR: checked and fast path disagree\n");
            return EXIT_FAILURE;
        }
    }

    printf("\nAll DRAM mapping tests passed.\n");
    return EXIT_SUCCESS;
}

