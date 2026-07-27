#ifndef PROTECTED_DAEMON_DRAM_MAPPING_H
#define PROTECTED_DAEMON_DRAM_MAPPING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "calibration.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRAM_MAPPING_MAX_MASKS CALIBRATION_MAX_BANK_MASKS
#define DRAM_MAPPING_MASK_SET_ID_MAX CALIBRATION_MASK_SET_ID_MAX

enum dram_mapping_rc {
	DRAM_MAPPING_OK = 0,
	DRAM_MAPPING_ERR_INVALID_ARG = -1,
	DRAM_MAPPING_ERR_NOT_READY = -2,
	DRAM_MAPPING_ERR_CALIBRATION_REJECTED = -3,
	DRAM_MAPPING_ERR_MASK_COUNT = -4,
	DRAM_MAPPING_ERR_ZERO_MASK = -5,
	DRAM_MAPPING_ERR_DUPLICATE_MASK = -6,
	DRAM_MAPPING_ERR_MASK_SET_ID = -7
};

/*
 * Recovered PA -> bank-class mapping.
 *
 * "bank_class" is intentionally used instead of "bank_id":
 * the recovered parity masks are validated conflict-separation functions,
 * but they are not claimed to be JEDEC channel/rank/bank-group/bank fields.
 */

struct dram_mapping {
	uint64_t masks[DRAM_MAPPING_MAX_MASKS];
	size_t mask_count;

	char mask_set_id[DRAM_MAPPING_MASK_SET_ID_MAX];

	bool ready;
};

void dram_mapping_reset(struct dram_mapping *mapping);

bool dram_mapping_is_ready(const struct dram_mapping *mapping);

/*
 * Initialize from a calibration result that has already passed:
 * status_success == true
 * acceptance_passed == true
 * bank_mask_count > 0
 *
 * The function also rejects zero and duplicate masks.
 */

int dram_mapping_init_from_calibration(struct dram_mapping *mapping, const struct calibration_result *result);

/*
 * Compute the recoverd bank class/signature for one physical address.
 *
 * Class bit i:
 * parity(physical_address & masks[i])
 *
 * Returns DRAM_MAPPING_OK on success.
 */

int dram_mapping_bank_class(const struct dram_mapping *mapping,uint64_t physical_address,uint64_t *bank_class_out);

/*
 * Fast path for callers that have already established mapping->ready == t
 * No argument/readiness validation is performed.
 */

uint64_t dram_mapping_bank_class_fast(const struct dram_mapping *mapping,uint64_t physical_address);

/*
 * Compare two physical addresses directly.
 *
 * This avoids constructing two classes:
 * two addresses are in the same recovered class iff, for every mask,
 *
 * parity((pa1 ^ pa2) & mask) == 0
 */

int dram_mapping_same_bank_class(const struct dram_mapping *mapping, uint64_t pa1, uint64_t pa2, bool *same_out);

const char *dram_mapping_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* PROTECTED_DAEMON_DRAM_MAPPING_H */
