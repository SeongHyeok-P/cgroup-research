#define _GNU_SOURCE

#include "bank_profile.h"

#include "addr_translate.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct maps_region {
	uint64_t start;
	uint64_t end;
	char perms[8];
	char pathname[256];
};

void bank_profile_config_default(struct bank_profile_config *cfg)
{
	if (cfg == NULL)
		return;

	cfg->max_pages_to_sample = 4096U;
	cfg->sample_stride_pages = 16U;

	cfg->include_anonymous = true;
	cfg->include_heap = true;
	cfg->include_stack = true;
	cfg->include_file_backed = false;
	cfg->include_readonly = false;
}

void bank_profile_reset(struct bank_profile *profile)
{
	if (profile == NULL)
		return;

	memset(profile,0,sizeof(*profile));
}

static void profile_set_error(struct bank_profile *profile, const char *message)
{
	if (profile == NULL || message == NULL)
		return;

	(void)snprintf(profile->error_message,sizeof(profile->error_message),"%s",message);
}

static bool is_space_or_newline(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void trim_trailing_space(char *s)
{
	size_t len;

	if (s == NULL)
		return;

	len = strlen(s);
	while (len > 0U && is_space_or_newline(s[len - 1U])) {
		s[len - 1U] = '\0';
		len--;
	}
}

static const char *skip_leading_space(const char *s)
{
	if (s == NULL)
		return "";

	while (*s == ' ' || *s == '\t') 
		s++;

	return s;
}

static int parse_maps_line(const char *line, struct maps_region *region)
{
	unsigned long long start;
	unsigned long long end;
	unsigned long long offset;
	unsigned long long inode;
	char dev[32];
	int consumed = 0;
	int n;
	const char *path;

	if (line == NULL || region == NULL)
		return -1;

	memset(region, 0, sizeof(*region));

	n = sscanf(line,"%llx-%llx %7s %llx %31s %llu %n",&start,&end,region->perms,&offset,dev,&inode,&consumed);

	(void)offset;
	(void)dev;
	(void)inode;

	if (n < 6 || consumed <= 0 || end <= start) 
		return -1;

	region->start = (uint64_t)start;
	region->end = (uint64_t)end;

	path = skip_leading_space(line + consumed);
	(void)snprintf(region->pathname,sizeof(region->pathname),"%s",path);
	trim_trailing_space(region->pathname);

	return 0;
}

static bool path_is_empty(const char *path)
{
	return path == NULL || path[0] == '\0';
}

static bool path_is_heap(const char *path)
{
	return path != NULL && strcmp(path,"[heap]") == 0;
}

static bool path_is_stack(const char *path)
{
	return path != NULL && strncmp(path,"[stack",6U) == 0;
}

static bool path_is_bracketed_special(const char *path)
{
	return path != NULL && path[0] == '[';
}

static bool path_is_file_backed(const char *path)
{
	return path != NULL && path[0] != '\0' && path[0] != '[';
}

static bool region_is_selected(const struct maps_region *region,const struct bank_profile_config *cfg)
{
	bool is_heap;
	bool is_stack;
	bool is_anon;
	bool is_file;

	if (region == NULL || cfg == NULL) 
		return false;

	if (region->perms[0] != 'r')
		return false;

	if (!cfg->include_readonly && region->perms[1] != 'w')
		return false;

	is_heap = path_is_heap(region->pathname);
	is_stack = path_is_stack(region->pathname);
	is_anon = path_is_empty(region->pathname);
	is_file = path_is_file_backed(region->pathname);

	if (is_heap)
		return cfg->include_heap;

	if (is_stack)
		return cfg->include_stack;

	if (is_anon)
		return cfg->include_anonymous;

	if (is_file)
		return cfg->include_file_backed;

	/*
	 * Other bracketed regions such as [vdso],[vvar],[vsyscall] are skipped
	 * by default. They are not useful for DRAM bank usage profiling.
	 */
	if (path_is_bracketed_special(region->pathname))
		return false;

	return false;
}

/*
 * aggregate src to dst
 */
void bank_profile_merge_counters(struct bank_profile *dst,const struct bank_profile *src)
{
	dst->regions_seen += src->regions_seen;
	dst->regions_selected += src->regions_selected;

	dst->pages_seen += src->pages_seen;
	dst->pages_considered += src->pages_considered;
	dst->pages_sampled += src->pages_sampled;
	dst->pages_translated += src->pages_translated;

	dst->pages_skipped_not_present += src->pages_skipped_not_present;
	dst->pages_skipped_swapped += src->pages_skipped_not_present;
	dst->pages_skipped_pfn_unavailable += src->pages_skipped_not_present;
	dst->pages_skipped_translation_error +=	src->pages_skipped_not_present;
	dst->pages_skipped_mapping_error += src->pages_skipped_not_present;
}
static int profile_add_bank_class_count(struct bank_profile *profile, uint64_t bank_class,uint64_t page_count,uint64_t byte_count)
{
	size_t i;
	
	if (profile == NULL)
		return BANK_PROFILE_ERR_INVALID_ARG;

	if (page_count == 0U && byte_count == 0U)
		return BANK_PROFILE_OK;

	for (i = 0; i < profile->entry_count; i++) {
		if (profile->entries[i].bank_class == bank_class) {
			profile->entries[i].page_count += page_count;
			profile->entries[i].byte_count += byte_count;
			return BANK_PROFILE_OK;
		}
	}

	if (profile->entry_count >= BANK_PROFILE_MAX_ENTRIES) {
		profile_set_error(profile, "too many unique bank classes");
		return BANK_PROFILE_ERR_TOO_MANY_CLASSES;
	}

	profile->entries[profile->entry_count].bank_class = bank_class;
	profile->entries[profile->entry_count].page_count = page_count;
	profile->entries[profile->entry_count].byte_count = byte_count;
	profile->entry_count++;

	return BANK_PROFILE_OK;
}
static int profile_add_bank_class(struct bank_profile *profile, uint64_t bank_class, uint64_t bytes)
{
	return profile_add_bank_class_count(profile,bank_class,1U,bytes);
}
static void profile_finalize(struct bank_profile *profile)
{
	size_t i;
	uint64_t total = 0U;
	uint64_t max_count = 0U;
	uint64_t max_class = 0U;

	if (profile == NULL)
		return;

	for (i = 0; i < profile->entry_count; i++) {
		total += profile->entries[i].page_count;

		if (profile->entries[i].page_count > max_count) {
			max_count = profile->entries[i].page_count;
			max_class = profile->entries[i].bank_class;
		}
	}

	profile->dominant_bank_class = max_class;
	profile->dominant_page_count = max_count;

	if (total > 0U) 
		profile->dominant_fraction = (double)max_count / (double)total;
	else
		profile->dominant_fraction = 0.0;

}

static void count_translation_skip(struct bank_profile *profile,int rc)
{
	if (profile == NULL)
		return;

	switch(rc) {
		case ADDR_TRANSLATE_ERR_NOT_PRESENT:
			profile->pages_skipped_not_present++;
			break;
		case ADDR_TRANSLATE_ERR_SWAPPED:
			profile->pages_skipped_swapped++;
			break;
		case ADDR_TRANSLATE_ERR_PFN_UNAVAILABLE:
			profile->pages_skipped_pfn_unavailable++;
			break;
		default:
			profile->pages_skipped_translation_error++;
			break;
	}
}

static int sample_region(const struct maps_region *region,struct addr_translate_ctx *at,const struct dram_mapping *mapping,const struct bank_profile_config *cfg, struct bank_profile *profile)
{
	uint64_t page_size;
	uint64_t region_pages;
	uint64_t page_idx;
	uint64_t stride;

	if (region == NULL || at == NULL || mapping == NULL || cfg == NULL || profile == NULL) 
		return BANK_PROFILE_ERR_INVALID_ARG;

	page_size = profile->page_size;
	if (page_size == 0U)
		return BANK_PROFILE_ERR_PAGE_SIZE;

	region_pages = (region->end - region->start) / page_size;
	profile->pages_seen += region_pages;

	stride = cfg->sample_stride_pages;
	if (stride == 0U)
		stride = 1U;

	for (page_idx = 0U; page_idx < region_pages; page_idx += stride) {
		struct addr_translation tr;
		uint64_t va;
		uint64_t bank_class;
		int rc;

		if (cfg->max_pages_to_sample > 0U && profile->pages_sampled >= (uint64_t)cfg->max_pages_to_sample)
			break;

		va = region->start + page_idx * page_size;

		profile->pages_considered++;
		profile->pages_sampled++;

		rc = addr_translate_va_to_pa(at,va,&tr);
		if (rc != ADDR_TRANSLATE_OK) {
			count_translation_skip(profile,rc);
			continue;
		}

		profile->pages_translated++;

		rc = dram_mapping_bank_class(mapping,tr.physical_address,&bank_class);

		if (rc != DRAM_MAPPING_OK) {
			profile->pages_skipped_mapping_error++;
			continue;
		}

		rc = profile_add_bank_class(profile,bank_class,page_size);
		if (rc != BANK_PROFILE_OK)
			return rc;
	}

	return BANK_PROFILE_OK;
}

int bank_profile_build(pid_t pid,const struct dram_mapping *mapping,const struct bank_profile_config *cfg,struct bank_profile *out)
{
	struct bank_profile_config local_cfg;
	struct addr_translate_ctx at;
	char maps_path[64];
	char line[1024];
	FILE *maps;
	long page_size_long;
	int rc;
	int n;

	if (pid <= 0 || mapping == NULL || out == NULL)
		return BANK_PROFILE_ERR_INVALID_ARG;

	if (!dram_mapping_is_ready(mapping)) 
		return BANK_PROFILE_ERR_NOT_READY;

	bank_profile_reset(out);
	out->pid = pid;

	if (cfg == NULL) {
		bank_profile_config_default(&local_cfg);
		cfg = &local_cfg;
	}
	else {
		local_cfg = *cfg;
		if (local_cfg.sample_stride_pages == 0U) {
			local_cfg.sample_stride_pages = 1U;
		}
		cfg = &local_cfg;
	}

	page_size_long = sysconf(_SC_PAGESIZE);
	if (page_size_long <= 0) {
		profile_set_error(out,"failed to get page size");
		return BANK_PROFILE_ERR_PAGE_SIZE;
	}

	out->page_size = (uint64_t)page_size_long;

	n = snprintf(maps_path, sizeof(maps_path),"/proc/%ld/maps",(long)pid);
	if (n <= 0 || (size_t)n >= sizeof(maps_path)) {
		profile_set_error(out,"failed to build maps path");
		return BANK_PROFILE_ERR_INVALID_ARG;
	}

	maps = fopen(maps_path,"r");
	if (maps == NULL) {
		profile_set_error(out,"failed to open /proc/[pid]/maps");
		return BANK_PROFILE_ERR_OPEN_MAPS;
	}

	addr_translate_reset(&at);
	rc = addr_translate_open(&at,pid);
	if (rc != ADDR_TRANSLATE_OK) {
		(void)fclose(maps);
		profile_set_error(out,addr_translate_strerror(rc));
		return BANK_PROFILE_ERR_ADDR_OPEN;
	}

	while (fgets(line,sizeof(line),maps) != NULL) {
		struct maps_region region;

		if (parse_maps_line(line,&region) < 0)
			continue;
		out->regions_seen++;

		if (!region_is_selected(&region,cfg)) 
			continue;
		out->regions_selected++;

		rc = sample_region(&region, &at, mapping, cfg,out);
		if (rc != BANK_PROFILE_OK) {
			addr_translate_close(&at);
			(void)fclose(maps);
			return rc;
		}

		if (cfg->max_pages_to_sample > 0U && out->pages_sampled >= (uint64_t)cfg->max_pages_to_sample)
			break;
	}

	if (ferror(maps)) {
		addr_translate_close(&at);
		(void)fclose(maps);
		profile_set_error(out,"error while reading /proc/[pid]/maps");
		return BANK_PROFILE_ERR_IO;
	}

	addr_translate_close(&at);
	(void)fclose(maps);

	profile_finalize(out);

	if (out->pages_translated == 0U || out->entry_count == 0U) {
		profile_set_error(out,"no pages were translated into bank classes");
		return BANK_PROFILE_ERR_NO_SAMPLES;
	}

	return BANK_PROFILE_OK;
}

uint64_t bank_profile_count_for_class(const struct bank_profile *profile, uint64_t bank_class)
{
	size_t i;

	if (profile == NULL)
		return 0U;

	for (i = 0; i < profile->entry_count; i++) {
		if (profile->entries[i].bank_class == bank_class)
			return profile->entries[i].page_count;
	}

	return 0U;
}

int bank_profile_merge(struct bank_profile *dst,const struct bank_profile *src)
{
	size_t i;
	int rc;

	if (dst == NULL || src == NULL)
		return BANK_PROFILE_ERR_INVALID_ARG;

	/*
	 * Empty src has nothing to merge
	 */
	if (src->entry_count == 0U)
		return BANK_PROFILE_OK;

	/*
	 * If src has entries, it must have a valid page size
	 */
	if (src->page_size == 0U) {
		profile_set_error(dst, "source profile has invalid page size");
		return BANK_PROFILE_ERR_PAGE_SIZE;
	}

	/*
	 * dst may be an empty aggregate profile initialized by bank_profile_reset()
	 * In that case inherit page size from src
	 */
	if (dst->page_size == 0U) {
		dst->page_size = src->page_size;
	}
	else if (dst->page_size != src->page_size) {
		profile_set_error(dst, "cannot merge profiles with different page sizes");
		return BANK_PROFILE_ERR_PAGE_SIZE;
	}

	/*
	 * Aggregate profile dose not represent one concrete pid
	 * pid == 0 means aggregate profile
	 */
	dst->pid = 0;

	/*
	 * Merge histogram entries.
	 * bank_class = 0 is valid, so do not skip it
	 */

	for (i = 0U; i < src->entry_count; i++) {
		rc = profile_add_bank_class_count(dst,src->entries[i].bank_class, src->entries[i].page_count,src->entries[i].byte_count);
		if (rc != BANK_PROFILE_OK)
			return rc;
	}

	/*
	 * Merge profile-level counters once per src profile.
         * Do not do this inside the entry loop.
	 */
	bank_profile_merge_counters(dst,src);

	/*
	 * Recompute dominant_bank_class, dominant_page_count,
	 * and dominant_fraction after merging.
	 */
	profile_finalize(dst);

	return BANK_PROFILE_OK;
}

int bank_profile_overlap_score(const struct bank_profile *a, const struct bank_profile *b, double *score_out,uint64_t *common_pages_out)
{
	uint64_t intersection = 0U;
	uint64_t union_count = 0U;
	size_t i;
	size_t j;

	if (a == NULL || b == NULL || score_out == NULL || a->entry_count == 0U || b->entry_count == 0U)
		return BANK_PROFILE_ERR_INVALID_ARG;

	for (i = 0; i < a->entry_count; i++) {
		uint64_t count_a = a->entries[i].page_count;
		uint64_t count_b = bank_profile_count_for_class(b,a->entries[i].bank_class);

		intersection += count_a < count_b ? count_a : count_b;
		union_count += count_a > count_b ? count_a : count_b;

	}

	for (j = 0; j < b->entry_count; j++) {
		bool exists_in_a = false;

		for (i = 0; i < a->entry_count; i++) {
			if (a->entries[i].bank_class == b->entries[j].bank_class) {
				exists_in_a = true;
				break;
			}
		}

		if (!exists_in_a) {
			union_count += b->entries[j].page_count;
		}
	}

	if (union_count == 0U)
		return BANK_PROFILE_ERR_NO_SAMPLES;

	*score_out = (double)intersection / (double)union_count;

	if (common_pages_out != NULL)
		*common_pages_out = intersection;

	return BANK_PROFILE_OK;
}

void bank_profile_print(FILE *stream, const struct bank_profile *profile, size_t max_entries)
{
    size_t i;
    size_t limit;

    if (stream == NULL || profile == NULL) {
        return;
    }

    fprintf(stream, "pid                 : %ld\n", (long)profile->pid);
    fprintf(stream, "page_size           : %" PRIu64 "\n", profile->page_size);
    fprintf(stream, "regions seen        : %" PRIu64 "\n", profile->regions_seen);
    fprintf(stream, "regions selected    : %" PRIu64 "\n", profile->regions_selected);
    fprintf(stream, "pages seen          : %" PRIu64 "\n", profile->pages_seen);
    fprintf(stream, "pages considered    : %" PRIu64 "\n", profile->pages_considered);
    fprintf(stream, "pages sampled       : %" PRIu64 "\n", profile->pages_sampled);
    fprintf(stream, "pages translated    : %" PRIu64 "\n", profile->pages_translated);
    fprintf(stream, "not present         : %" PRIu64 "\n",
            profile->pages_skipped_not_present);
    fprintf(stream, "swapped             : %" PRIu64 "\n",
            profile->pages_skipped_swapped);
    fprintf(stream, "pfn unavailable     : %" PRIu64 "\n",
            profile->pages_skipped_pfn_unavailable);
    fprintf(stream, "translation errors  : %" PRIu64 "\n",
            profile->pages_skipped_translation_error);
    fprintf(stream, "mapping errors      : %" PRIu64 "\n",
            profile->pages_skipped_mapping_error);
    fprintf(stream, "unique bank classes : %zu\n", profile->entry_count);
    fprintf(stream, "dominant class      : 0x%" PRIx64 "\n",
            profile->dominant_bank_class);
    fprintf(stream, "dominant pages      : %" PRIu64 "\n",
            profile->dominant_page_count);
    fprintf(stream, "dominant fraction   : %.4f\n",
            profile->dominant_fraction);

    if (max_entries == 0U || max_entries > profile->entry_count) {
        limit = profile->entry_count;
    } else {
        limit = max_entries;
    }

    fprintf(stream, "\n%12s  %12s  %12s\n",
            "bank_class",
            "pages",
            "bytes");
    fprintf(stream, "%12s  %12s  %12s\n",
            "------------",
            "------------",
            "------------");

    for (i = 0; i < limit; i++) {
        fprintf(stream,
                "0x%010" PRIx64 "  %12" PRIu64 "  %12" PRIu64 "\n",
                profile->entries[i].bank_class,
                profile->entries[i].page_count,
                profile->entries[i].byte_count);
    }

    if (limit < profile->entry_count) {
        fprintf(stream, "... %zu more entries omitted\n",
                profile->entry_count - limit);
    }
}

const char *bank_profile_strerror(int rc)
{
    switch (rc) {
    case BANK_PROFILE_OK:
        return "success";
    case BANK_PROFILE_ERR_INVALID_ARG:
        return "invalid argument";
    case BANK_PROFILE_ERR_NOT_READY:
        return "DRAM mapping is not ready";
    case BANK_PROFILE_ERR_PAGE_SIZE:
        return "failed to determine page size";
    case BANK_PROFILE_ERR_OPEN_MAPS:
        return "failed to open /proc/[pid]/maps";
    case BANK_PROFILE_ERR_ADDR_OPEN:
        return "failed to open /proc/[pid]/pagemap";
    case BANK_PROFILE_ERR_TOO_MANY_CLASSES:
        return "too many unique bank classes";
    case BANK_PROFILE_ERR_NO_SAMPLES:
        return "no usable page samples";
    case BANK_PROFILE_ERR_IO:
        return "I/O error";
    default:
        return "unknown bank-profile error";
    }
}



