#ifndef PROTECTED_DAEMON_CALIBRATION_H
#define PROTECTED_DAEMON_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CALIBRATION_MAX_BANK_MASKS 32U
#define CALIBRATION_MASK_SET_ID_MAX 65U
#define CALIBRATION_ERROR_MESSAGE_MAX 256U

enum calibration_rc {
	CALIBRATION_OK = 0,
	CALIBRATION_ERR_INVALID_ARG = -1,
	CALIBRATION_ERR_FORK = -2,
	CALIBRATION_ERR_WAIT = -3,
	CALIBRATION_ERR_TIMEOUT = -4,
	CALIBRATION_ERR_WORKER_EXIT = -5,
	CALIBRATION_ERR_IO = -6,
	CALIBRATION_ERR_JSON = -7,
	CALIBRATION_ERR_REJECTED = -8,
	CALIBRATION_ERR_TOO_MANY_MASKS = -9,
};

struct calibration_config {
	const char *worker_path;    /* scripts/kk_calibration.py */
	const char *kk_main_path;   /* patched Knock-Knock/main, optional */
	const char *result_path;    /* /run/protected-daemon/dram-map.json */
	const char *work_dir;       /* /run/protected-daemon/kk-calibration */
	const char *log_path;	    /* worker stdout/stderr log, optional */

 	unsigned int runs;
	double memory_percent;
	unsigned int measurements;
	unsigned int timing_rounds;
	unsigned int timeout_sec;  /* 0 => no timeout */
	
	bool keep_csv;
};

struct calibration_metrics {
	uint64_t tp;
	uint64_t tn;
	uint64_t fp;
	uint64_t fn;

	double accuracy;
	double precision;
	double recall;
	double f1;
};

struct calibration_result {
	bool status_success;
	bool acceptance_passed;

	uint64_t bank_masks[CALIBRATION_MAX_BANK_MASKS];
	size_t bank_mask_count;

	char mask_set_id[CALIBRATION_MASK_SET_ID_MAX];
	struct calibration_metrics holdout;

	int worker_exit_code;
	char error_message[CALIBRATION_ERROR_MESSAGE_MAX];
};

int calibration_run(const struct calibration_config *cfg, struct calibration_result *out);

int calibration_load_result(const char *result_path, struct calibration_result *out);

void calibration_result_reset(struct calibration_result *out);

const char *calibration_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* PROTECTED_DAEMON_CALIBRATION_H */

