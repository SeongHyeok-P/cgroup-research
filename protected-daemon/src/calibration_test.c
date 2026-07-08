#include "calibration.h"

#include <inttypes.h>
#include <stdio.h>

int main(void)
{
    struct calibration_config cfg = {
        .worker_path =
            "../scripts/kk_calibrate.py",

        .kk_main_path =
            "../../Knock-Knock/main",

        .result_path =
            "/run/protected-daemon/dram-map.json",

        .work_dir =
            "/run/protected-daemon/kk-calibration",

        .log_path =
            "/run/protected-daemon/calibration.log",

        .runs = 3,
        .memory_percent = 25.0,
        .measurements = 500,
        .timing_rounds = 50,
        .timeout_sec = 1800,
        .keep_csv = true
    };

    struct calibration_result result;

    int rc = calibration_run(&cfg, &result);

    if (rc != CALIBRATION_OK) {
        fprintf(stderr,
                "calibration failed: %s\n",
                calibration_strerror(rc));

        fprintf(stderr,
                "detail: %s\n",
                result.error_message);

        return 1;
    }

    printf("Calibration success\n");
    printf("Mask count: %zu\n",
           result.bank_mask_count);

    printf("Precision: %.4f\n",
           result.holdout.precision);

    printf("Recall: %.4f\n",
           result.holdout.recall);

    for (size_t i = 0;
         i < result.bank_mask_count;
         i++) {

        printf("mask[%zu] = 0x%016" PRIx64 "\n",
               i,
               result.bank_masks[i]);
    }

    return 0;
}
