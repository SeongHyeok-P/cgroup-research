#define _GNU_SOURCE

#include "addr_translate.h"
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
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_RESULT_PATH "/run/protected-daemon/dram-map.json"
#define DEFAULT_WORK_DIR    "/run/protected-daemon/kk-calibration"
#define DEFAULT_LOG_PATH    "/run/protected-daemon/calibration.log"
#define DEFAULT_PAGE_COUNT  16U

static bool path_exists(const char *path)
{
	return path != NULL && access(path, F_OK) == 0;
}

static const char *choose_existing_path(const char *path_a, const char *path_b)
{
	if (path_exists(path_a)) {
		return path_a;
	}

	if (path_exists(path_b)) {
		return path_b;
	}

	return NULL;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--force-calibration] [--load-only] [--pages N]\n"
            "\n"
            "Default behavior:\n"
            "  1. Try to load " DEFAULT_RESULT_PATH "\n"
            "  2. If loading fails, run kk_calibrate.py\n"
            "  3. mmap N pages, translate VA->PA, then PA->bank_class\n"
            "\n"
            "Options:\n"
            "  --force-calibration   Ignore existing JSON and run calibration again\n"
            "  --load-only           Do not run calibration; fail if JSON is missing/invalid\n"
            "  --pages N             Number of anonymous pages to test, default 16\n",
            prog);
}

static int parse_args(int argc, char *argv[], bool *force_calibration, bool *load_only, size_t *page_count)
{
	int i;
	if (force_calibration == NULL || load_only == NULL || page_count == NULL) {
		return -1;
	}

	*force_calibration = false;
	*load_only = false;
	*page_count = DEFAULT_PAGE_COUNT;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i],"--force-calibration") == 0) {
			*force_calibration = true;
			continue;
		}

		if (strcmp(argv[i], "--load-only") == 0) {
			*load_only = true;
			continue;
		}

		if (strcmp(argv[i], "--pages") == 0) {
			char *end = NULL;
			unsigned long value;

			if (i + 1 >= argc) {
				return -1;
			}

			errno = 0;
			value = strtoul(argv[i + 1],&end,10);
			if (errno != 0 || end == argv[i + 1] || *end != '\0' || value == 0UL) {
				return -1;
			}

			*page_count = (size_t)value;
			i++;
			continue;
		}

		if (strcmp(argv[i],"--help") == 0 || strcmp(argv[i],"h") == 0) {
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		}

		return -1;
	}

	if (*force_calibration && *load_only) {
		return -1;
	}

	return 0;
}

static int ensure_runtime_dir(void)
{
	if (mkdir("/run/protected-daemon",0755) < 0) {
		if (errno != EEXIST) {
			perror("mkdir /run/protected-daemon");
			return -1;
		}
	}
	
	return 0;
}

static int load_or_run_calibration(bool force_calibration,bool load_only, struct calibration_result *calibration)
{
	struct calibration_config cfg;
	const char *worker_path;
	const char *kk_main_path;
	int rc;

	if (calibration == NULL) {
		return CALIBRATION_ERR_INVALID_ARG;
	}

	calibration_result_reset(calibration);

	if (!force_calibration) {
		rc = calibration_load_result(DEFAULT_RESULT_PATH, calibration);
		if (rc == CALIBRATION_OK) {
			printf("[INFO] loaded existing calibration JSON: %s\n",DEFAULT_RESULT_PATH);
			return CALIBRATION_OK;
		}
		printf("[INFO] existing calibration JSON unavailable: %s\n",
     		        calibration_strerror(rc));

	        if (load_only) {
        	    return rc;
        	}	
    	}

	worker_path = choose_existing_path("../scripts/kk_calibrate.py",
                                       "scripts/kk_calibrate.py");
        kk_main_path = choose_existing_path("../../Knock-Knock/main",
                                        "../Knock-Knock/main");

    	if (worker_path == NULL) {
        	fprintf(stderr,
                	"[ERROR] cannot find kk_calibrate.py.\n"
	                "        Tried: ../scripts/kk_calibrate.py\n"
	                "               scripts/kk_calibrate.py\n");
        	return CALIBRATION_ERR_INVALID_ARG;
    	}

	if (kk_main_path == NULL) {
        	fprintf(stderr,
                	"[ERROR] cannot find Knock-Knock main binary.\n"
                	"        Tried: ../../Knock-Knock/main\n"
               		"               ../Knock-Knock/main\n");
	        return CALIBRATION_ERR_INVALID_ARG;
    	}

    	if (ensure_runtime_dir() < 0) {
       		 return CALIBRATION_ERR_IO;
    	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.worker_path = worker_path;
    	cfg.kk_main_path = kk_main_path;
    	cfg.result_path = DEFAULT_RESULT_PATH;
    	cfg.work_dir = DEFAULT_WORK_DIR;
    	cfg.log_path = DEFAULT_LOG_PATH;
    	cfg.runs = 3U;
    	cfg.memory_percent = 25.0;
    	cfg.measurements = 500U;
    	cfg.timing_rounds = 50U;
    	cfg.timeout_sec = 1800U;
    	cfg.keep_csv = true;

    	printf("[INFO] running calibration worker\n");
    	printf("[INFO] worker_path=%s\n", cfg.worker_path);
    	printf("[INFO] kk_main_path=%s\n", cfg.kk_main_path);

    	rc = calibration_run(&cfg, calibration);
    	if (rc != CALIBRATION_OK) {
        	fprintf(stderr,
                	"[ERROR] calibration_run failed: %s\n",
                	calibration_strerror(rc));
        	fprintf(stderr,
                	"[ERROR] detail: %s\n",
                	calibration->error_message);
        	return rc;
    	}

    	return CALIBRATION_OK;
}

static int test_va_pa_bank_class(const struct dram_mapping *mapping, size_t page_count)
{
    struct addr_translate_ctx at;
    long page_size_long;
    size_t page_size;
    size_t mapping_size;
    unsigned char *buf;
    size_t i;
    int rc;

    if (mapping == NULL || !dram_mapping_is_ready(mapping) || page_count == 0U) {
        return EXIT_FAILURE;
    }

    page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        fprintf(stderr, "[ERROR] sysconf(_SC_PAGESIZE) failed\n");
        return EXIT_FAILURE;
    }

    page_size = (size_t)page_size_long;

    if (page_count > SIZE_MAX / page_size) {
        fprintf(stderr, "[ERROR] mapping size overflow\n");
        return EXIT_FAILURE;
    }

    mapping_size = page_count * page_size;

    buf = mmap(NULL,
               mapping_size,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    /*
     * Force physical page allocation. mmap alone may only reserve virtual
     * address space; touching each page makes it present in pagemap.
     */
    for (i = 0; i < page_count; i++) {
        buf[i * page_size] = (unsigned char)(0x40U + (i & 0x3fU));
    }

    addr_translate_reset(&at);

    rc = addr_translate_open(&at, getpid());
    if (rc != ADDR_TRANSLATE_OK) {
        fprintf(stderr,
                "[ERROR] addr_translate_open failed: %s\n",
                addr_translate_strerror(rc));
        (void)munmap(buf, mapping_size);
        return EXIT_FAILURE;
    }

    printf("\n[INFO] VA -> PA -> bank_class test\n");
    printf("[INFO] pid=%ld page_size=%zu pages=%zu\n",
           (long)getpid(),
           page_size,
           page_count);
    printf("\n");
    printf("%5s  %18s  %18s  %18s  %12s\n",
           "idx",
           "VA",
           "PFN",
           "PA",
           "bank_class");
    printf("%5s  %18s  %18s  %18s  %12s\n",
           "-----",
           "------------------",
           "------------------",
           "------------------",
           "------------");

    for (i = 0; i < page_count; i++) {
        struct addr_translation tr;
        uint64_t va;
        uint64_t bank_class_checked;
        uint64_t bank_class_fast;

        va = (uint64_t)(uintptr_t)(buf + (i * page_size));

        rc = addr_translate_va_to_pa(&at, va, &tr);
        if (rc != ADDR_TRANSLATE_OK) {
            fprintf(stderr,
                    "[ERROR] VA translation failed at page %zu: %s\n",
                    i,
                    addr_translate_strerror(rc));
            addr_translate_close(&at);
            (void)munmap(buf, mapping_size);
            return EXIT_FAILURE;
        }

        rc = dram_mapping_bank_class(mapping,
                                     tr.physical_address,
                                     &bank_class_checked);
        if (rc != DRAM_MAPPING_OK) {
            fprintf(stderr,
                    "[ERROR] dram_mapping_bank_class failed at page %zu: %s\n",
                    i,
                    dram_mapping_strerror(rc));
            addr_translate_close(&at);
            (void)munmap(buf, mapping_size);
            return EXIT_FAILURE;
        }

        bank_class_fast =
            dram_mapping_bank_class_fast(mapping, tr.physical_address);

        if (bank_class_checked != bank_class_fast) {
            fprintf(stderr,
                    "[ERROR] checked path and fast path mismatch at page %zu\n",
                    i);
            addr_translate_close(&at);
            (void)munmap(buf, mapping_size);
            return EXIT_FAILURE;
        }

        printf("%5zu  0x%016" PRIx64 "  0x%016" PRIx64
               "  0x%016" PRIx64 "  0x%010" PRIx64 "\n",
               i,
               tr.virtual_address,
               tr.pfn,
               tr.physical_address,
               bank_class_checked);
    }

    printf("\n[INFO] checked path == fast path for all pages\n");

    if (page_count >= 2U) {
        struct addr_translation first;
        struct addr_translation second;
        bool same_class = false;
        uint64_t first_va = (uint64_t)(uintptr_t)buf;
        uint64_t second_va = (uint64_t)(uintptr_t)(buf + page_size);

        rc = addr_translate_va_to_pa(&at, first_va, &first);
        if (rc == ADDR_TRANSLATE_OK) {
            rc = addr_translate_va_to_pa(&at, second_va, &second);
        }

        if (rc == ADDR_TRANSLATE_OK) {
            rc = dram_mapping_same_bank_class(mapping,
                                              first.physical_address,
                                              second.physical_address,
                                              &same_class);
            if (rc != DRAM_MAPPING_OK) {
                fprintf(stderr,
                        "[ERROR] dram_mapping_same_bank_class failed: %s\n",
                        dram_mapping_strerror(rc));
                addr_translate_close(&at);
                (void)munmap(buf, mapping_size);
                return EXIT_FAILURE;
            }

            printf("[INFO] page[0] and page[1] same bank_class: %s\n",
                   same_class ? "true" : "false");
        }
    }

    addr_translate_close(&at);
    (void)munmap(buf, mapping_size);

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    struct calibration_result calibration;
    struct dram_mapping mapping;
    bool force_calibration;
    bool load_only;
    size_t page_count;
    size_t i;
    int rc;

    if (parse_args(argc,
                   argv,
                   &force_calibration,
                   &load_only,
                   &page_count) < 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    rc = load_or_run_calibration(force_calibration,
                                 load_only,
                                 &calibration);
    if (rc != CALIBRATION_OK) {
        fprintf(stderr,
                "[ERROR] failed to prepare calibration: %s\n",
                calibration_strerror(rc));
        return EXIT_FAILURE;
    }

    rc = dram_mapping_init_from_calibration(&mapping, &calibration);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr,
                "[ERROR] dram_mapping_init_from_calibration failed: %s\n",
                dram_mapping_strerror(rc));
        return EXIT_FAILURE;
    }

    printf("[INFO] DRAM mapping ready\n");
    printf("[INFO] mask_set_id=%s mask_count=%zu\n",
           mapping.mask_set_id,
           mapping.mask_count);
    printf("[INFO] holdout precision=%.4f recall=%.4f\n",
           calibration.holdout.precision,
           calibration.holdout.recall);

    for (i = 0; i < mapping.mask_count; i++) {
        printf("[INFO] bank_mask[%zu]=0x%016" PRIx64 "\n",
               i,
               mapping.masks[i]);
    }

    rc = test_va_pa_bank_class(&mapping, page_count);
    if (rc != EXIT_SUCCESS) {
        return rc;
    }

    printf("\n[INFO] addr_translate + dram_mapping integration test passed\n");
    return EXIT_SUCCESS;
}

