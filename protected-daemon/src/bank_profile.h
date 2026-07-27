#ifndef PROTECTED_DAEMON_BANK_PROFILE_H
#define PROTECTED_DAEMON_BANK_PROFILE_H

#include "dram_mapping.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BANK_PROFILE_MAX_ENTRIES 4096U
#define BANK_PROFILE_ERROR_MESSAGE_MAX 256U

enum bank_profile_rc {
	BANK_PROFILE_OK = 0,
	BANK_PROFILE_ERR_INVALID_ARG = -1,
	BANK_PROFILE_ERR_NOT_READY = -2,
	BANK_PROFILE_ERR_PAGE_SIZE = -3,
	BANK_PROFILE_ERR_OPEN_MAPS = -4,
	BANK_PROFILE_ERR_ADDR_OPEN = -5,
	BANK_PROFILE_ERR_TOO_MANY_CLASSES = -6,
	BANK_PROFILE_ERR_NO_SAMPLES = -7,
	BANK_PROFILE_ERR_IO = -8,
};

struct bank_profile_config {
	/*
	 * Maximum number of virtual pages sampled from one process.
	 * 0 means no explicit limit.
	 */
	size_t max_pages_to_sample;

	/*
	 * Sample every N pages within each selected VMA.
	 * 1 means dense page-by-page sampling.
	 */
	size_t sample_stride_pages;

	/*
	 * Region filters
	 *
	 * Default policy is intentionally conservative:
	 * -inclue anonymous, heap, and stack regions
	 *  -skip file-backed mappings
	 *  -skip read-only mappings
	 */
	bool include_anonymous;
	bool include_heap;
	bool include_stack;
	bool include_file_backed;
	bool include_readonly;
};

struct bank_profile_entry {
	uint64_t bank_class;
	uint64_t page_count;
	uint64_t byte_count;
};

struct bank_profile {
	pid_t pid;
	uint64_t page_size;

	uint64_t regions_seen;
	uint64_t regions_selected;

	uint64_t pages_seen;
	uint64_t pages_considered;
	uint64_t pages_sampled;
	uint64_t pages_translated;

	uint64_t pages_skipped_not_present;
	uint64_t pages_skipped_swapped;
	uint64_t pages_skipped_pfn_unavailable;
	uint64_t pages_skipped_translation_error;
	uint64_t pages_skipped_mapping_error;

	size_t entry_count;
	struct bank_profile_entry entries[BANK_PROFILE_MAX_ENTRIES];

	uint64_t dominant_bank_class;
	uint64_t dominant_page_count;
	double dominant_fraction;

	char error_message[BANK_PROFILE_ERROR_MESSAGE_MAX];
};

void bank_profile_config_default(struct bank_profile_config *cfg);

void bank_profile_reset(struct bank_profile *profile);

int bank_profile_build(pid_t pid,const struct dram_mapping *mapping,const struct bank_profile_config *cfg, struct bank_profile *out);

int bank_profile_overlap_score(const struct bank_profile *a, const struct bank_profile *b, double *score_out, uint64_t *common_pages_out);

uint64_t bank_profile_count_for_class(const struct bank_profile *profile, uint64_t bank_class);

void bank_profile_print(FILE *stream, const struct bank_profile *profile, size_t max_entries);

int bank_profile_merge(struct bank_profile *dst,const struct bank_profile *src);

const char *bank_profile_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* PROTECTED_DAEMON_BANK_PROFILE_H */


