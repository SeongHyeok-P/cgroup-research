#include "dram_mapping.h"

#include <string.h>

static size_t bounded_strlen(const char *s, size_t max_len)
{
	size_t n = 0U;

	if (s == NULL) {
		return 0U;
	}

	while (n < max_len && s[n] != '\0') {
		++n;
	}
	
	return n;
}

static unsigned int parity64(uint64_t value)
{
#if defined(__GNUC__) || defined(__clang__)
	return (unsigned int)__builtin_parityll(
	     (unsigned long long)value);
#else
    /*
     * Portable fallback: fold to 4bits and use a parity lookup constant.
     */
    value ^= value >> 32U;
    value ^= value >> 16U;
    value ^= value >> 8U;
    value ^= value >> 4U;

    return (unsigned int)((0x6996U >> (value & 0xFU)) & 1U);
#endif
}

void dram_mapping_reset(struct dram_mapping *mapping)
{
	if (mapping == NULL) {
		return;
	}

	memset(mapping, 0, sizeof(*mapping));
}

bool dram_mapping_is_ready(const struct dram_mapping *mapping)
{
	return mapping != NULL && mapping->ready && mapping->mask_count > 0U;
}

static int validate_calibration_result(const struct calibration_result *result)
{
	size_t id_len;
	size_t i;
	size_t j;

	if (result == NULL) {
		return DRAM_MAPPING_ERR_INVALID_ARG;
	}

	if (!result->status_success || !result->acceptance_passed) {
		return DRAM_MAPPING_ERR_CALIBRATION_REJECTED;
	}

	if(result->bank_mask_count == 0U || result->bank_mask_count > DRAM_MAPPING_MAX_MASKS) {
		return DRAM_MAPPING_ERR_MASK_COUNT;
	}

	id_len = bounded_strlen(result->mask_set_id, DRAM_MAPPING_MASK_SET_ID_MAX);

	if (id_len == 0U || id_len >= DRAM_MAPPING_MASK_SET_ID_MAX) {
		return DRAM_MAPPING_ERR_MASK_SET_ID;
	}

	for (i = 0U; i < result->bank_mask_count; ++i) {
		if (result->bank_masks[i] == 0U) {
			return DRAM_MAPPING_ERR_ZERO_MASK;
		}

		for (j = 0U; j < i; ++j) {
			if (result->bank_masks[i] == result->bank_masks[j]) {
				return DRAM_MAPPING_ERR_DUPLICATE_MASK;
			}
		}
	}

	return DRAM_MAPPING_OK;
}

int dram_mapping_init_from_calibration(struct dram_mapping *mapping, const struct calibration_result *result)
{
	int rc;
	size_t id_len;

	if (mapping == NULL || result == NULL) {
		return DRAM_MAPPING_ERR_INVALID_ARG;
	}

	/*
	 * Fail closed: a partially initialized mapping is never left ready.
	 */
	dram_mapping_reset(mapping);

	rc = validate_calibration_result(result);
	if (rc != DRAM_MAPPING_OK) {
		return rc;
	}

	memcpy(mapping->masks, result->bank_masks, result->bank_mask_count * sizeof(mapping->masks[0]));

	mapping->mask_count = result->bank_mask_count;

	id_len = bounded_strlen(result->mask_set_id, DRAM_MAPPING_MASK_SET_ID_MAX);

	memcpy(mapping->mask_set_id, result->mask_set_id, id_len);
	mapping->mask_set_id[id_len] = '\0';
	/*
	 * Publish readiness only after every field has been initialized.
	 */
	mapping->ready = true;
	return DRAM_MAPPING_OK;
}

uint64_t dram_mapping_bank_class_fast(const struct dram_mapping *mapping, uint64_t physical_address)
{
	uint64_t bank_class = 0U;
	size_t i;

	for (i = 0U; i < mapping->mask_count; ++i) {
		uint64_t bit = (uint64_t)parity64(physical_address & mapping->masks[i]);
		bank_class |= bit << i;
	}

	return bank_class;
}

int dram_mapping_bank_class(const struct dram_mapping *mapping,uint64_t physical_address,uint64_t *bank_class_out)
{
	if (mapping == NULL || bank_class_out == NULL) {
		return DRAM_MAPPING_ERR_INVALID_ARG;
	}

	if (!dram_mapping_is_ready(mapping)) {
		return DRAM_MAPPING_ERR_NOT_READY;
	}

	*bank_class_out = dram_mapping_bank_class_fast(mapping,physical_address);
	return DRAM_MAPPING_OK;
}

int dram_mapping_same_bank_class(const struct dram_mapping *mapping, uint64_t pa1,
		                                                     uint64_t pa2,
								     bool *same_out)
{
	uint64_t diff;
	size_t i;

	if (mapping == NULL || same_out == NULL) {
		return DRAM_MAPPING_ERR_INVALID_ARG;
	}

	if (!dram_mapping_is_ready(mapping)) {
		return DRAM_MAPPING_ERR_NOT_READY;
	}

	diff = pa1 ^ pa2;

	for (i = 0U; i < mapping->mask_count; ++i) {
		if (parity64(diff & mapping->masks[i]) != 0U) {
			*same_out = false;
			return DRAM_MAPPING_OK;
		}
	}

	*same_out = true;
	return DRAM_MAPPING_OK;
}

const char *dram_mapping_strerror(int rc)
{
	switch (rc) {
	case DRAM_MAPPING_OK:
		return "success";
	case DRAM_MAPPING_ERR_INVALID_ARG:
	        return "invalid argument";
        case DRAM_MAPPING_ERR_NOT_READY:
    	        return "DRAM mapping is not ready";
        case DRAM_MAPPING_ERR_CALIBRATION_REJECTED:
        	return "calibration result was not accepted";
    	case DRAM_MAPPING_ERR_MASK_COUNT:
	        return "invalid bank-mask count";
    	case DRAM_MAPPING_ERR_ZERO_MASK:
	        return "zero bank mask is not allowed";
    	case DRAM_MAPPING_ERR_DUPLICATE_MASK:
	        return "duplicate bank mask";
    	case DRAM_MAPPING_ERR_MASK_SET_ID:
	        return "invalid mask-set id";
   	default:
	        return "unknown DRAM-mapping error";
    }
}


