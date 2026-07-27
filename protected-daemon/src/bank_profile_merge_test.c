#include "bank_profile.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static void fail(const char *message)
{
    fprintf(stderr, "[FAIL] %s\n", message);
    exit(1);
}

static void expect_u64(const char *name, uint64_t got, uint64_t expected)
{
    if (got != expected) {
        fprintf(stderr,
                "[FAIL] %s: got=%" PRIu64 " expected=%" PRIu64 "\n",
                name,
                got,
                expected);
        exit(1);
    }
}

static void expect_size(const char *name, size_t got, size_t expected)
{
    if (got != expected) {
        fprintf(stderr,
                "[FAIL] %s: got=%zu expected=%zu\n",
                name,
                got,
                expected);
        exit(1);
    }
}

static void expect_double_close(
    const char *name,
    double got,
    double expected)
{
    double diff;

    if (got > expected) {
        diff = got - expected;
    } else {
        diff = expected - got;
    }

    if (diff > 0.000001) {
        fprintf(stderr,
                "[FAIL] %s: got=%.9f expected=%.9f\n",
                name,
                got,
                expected);
        exit(1);
    }
}

static struct bank_profile_entry *find_entry(
    struct bank_profile *profile,
    uint64_t bank_class)
{
    size_t i;

    if (profile == NULL) {
        return NULL;
    }

    for (i = 0U; i < profile->entry_count; i++) {
        if (profile->entries[i].bank_class == bank_class) {
            return &profile->entries[i];
        }
    }

    return NULL;
}

static void add_entry(
    struct bank_profile *profile,
    uint64_t bank_class,
    uint64_t page_count)
{
    struct bank_profile_entry *entry;

    if (profile == NULL) {
        fail("profile is NULL");
    }

    if (profile->entry_count >= BANK_PROFILE_MAX_ENTRIES) {
        fail("too many test entries");
    }

    entry = &profile->entries[profile->entry_count];
    entry->bank_class = bank_class;
    entry->page_count = page_count;
    entry->byte_count = page_count * profile->page_size;

    profile->entry_count++;
}

static void make_profile_a(struct bank_profile *profile)
{
    bank_profile_reset(profile);

    profile->pid = 1001;
    profile->page_size = 4096U;

    /*
     * A:
     *   class 0x00 -> 3 pages
     *   class 0x10 -> 5 pages
     * total = 8 pages
     */
    add_entry(profile, 0x00U, 3U);
    add_entry(profile, 0x10U, 5U);

    profile->regions_seen = 3U;
    profile->regions_selected = 2U;
    profile->pages_seen = 20U;
    profile->pages_considered = 10U;
    profile->pages_sampled = 9U;
    profile->pages_translated = 8U;

    profile->pages_skipped_not_present = 1U;
    profile->pages_skipped_swapped = 0U;
    profile->pages_skipped_pfn_unavailable = 0U;
    profile->pages_skipped_translation_error = 0U;
    profile->pages_skipped_mapping_error = 0U;
}

static void make_profile_b(struct bank_profile *profile)
{
    bank_profile_reset(profile);

    profile->pid = 1002;
    profile->page_size = 4096U;

    /*
     * B:
     *   class 0x00 -> 2 pages
     *   class 0x20 -> 7 pages
     * total = 9 pages
     */
    add_entry(profile, 0x00U, 2U);
    add_entry(profile, 0x20U, 7U);

    profile->regions_seen = 4U;
    profile->regions_selected = 3U;
    profile->pages_seen = 30U;
    profile->pages_considered = 12U;
    profile->pages_sampled = 10U;
    profile->pages_translated = 9U;

    profile->pages_skipped_not_present = 0U;
    profile->pages_skipped_swapped = 1U;
    profile->pages_skipped_pfn_unavailable = 0U;
    profile->pages_skipped_translation_error = 0U;
    profile->pages_skipped_mapping_error = 0U;
}

int main(void)
{
    struct bank_profile a;
    struct bank_profile b;
    struct bank_profile merged;
    struct bank_profile_entry *entry;
    double score;
    uint64_t common_pages;
    int rc;

    make_profile_a(&a);
    make_profile_b(&b);

    bank_profile_reset(&merged);

    rc = bank_profile_merge(&merged, &a);
    if (rc != BANK_PROFILE_OK) {
        fprintf(stderr,
                "[FAIL] merge A failed: %s\n",
                bank_profile_strerror(rc));
        return 1;
    }

    rc = bank_profile_merge(&merged, &b);
    if (rc != BANK_PROFILE_OK) {
        fprintf(stderr,
                "[FAIL] merge B failed: %s\n",
                bank_profile_strerror(rc));
        return 1;
    }

    /*
     * Expected merged histogram:
     *
     * class 0x00: 3 + 2 = 5 pages
     * class 0x10: 5 pages
     * class 0x20: 7 pages
     *
     * total = 17 pages
     */
    expect_u64("merged.pid", (uint64_t)merged.pid, 0U);
    expect_u64("merged.page_size", merged.page_size, 4096U);
    expect_size("merged.entry_count", merged.entry_count, 3U);

    expect_u64("class 0x00 pages",
               bank_profile_count_for_class(&merged, 0x00U),
               5U);
    expect_u64("class 0x10 pages",
               bank_profile_count_for_class(&merged, 0x10U),
               5U);
    expect_u64("class 0x20 pages",
               bank_profile_count_for_class(&merged, 0x20U),
               7U);

    entry = find_entry(&merged, 0x00U);
    if (entry == NULL) {
        fail("missing class 0x00");
    }
    expect_u64("class 0x00 bytes", entry->byte_count, 5U * 4096U);

    entry = find_entry(&merged, 0x10U);
    if (entry == NULL) {
        fail("missing class 0x10");
    }
    expect_u64("class 0x10 bytes", entry->byte_count, 5U * 4096U);

    entry = find_entry(&merged, 0x20U);
    if (entry == NULL) {
        fail("missing class 0x20");
    }
    expect_u64("class 0x20 bytes", entry->byte_count, 7U * 4096U);

    expect_u64("regions_seen",
               merged.regions_seen,
               a.regions_seen + b.regions_seen);
    expect_u64("regions_selected",
               merged.regions_selected,
               a.regions_selected + b.regions_selected);
    expect_u64("pages_seen",
               merged.pages_seen,
               a.pages_seen + b.pages_seen);
    expect_u64("pages_considered",
               merged.pages_considered,
               a.pages_considered + b.pages_considered);
    expect_u64("pages_sampled",
               merged.pages_sampled,
               a.pages_sampled + b.pages_sampled);
    expect_u64("pages_translated",
               merged.pages_translated,
               a.pages_translated + b.pages_translated);

    expect_u64("skipped_not_present",
               merged.pages_skipped_not_present,
               a.pages_skipped_not_present +
                   b.pages_skipped_not_present);
    expect_u64("skipped_swapped",
               merged.pages_skipped_swapped,
               a.pages_skipped_swapped +
                   b.pages_skipped_swapped);

    /*
     * Dominant class should be 0x20 with 7 pages.
     * Total pages in histogram = 17.
     * dominant_fraction = 7 / 17.
     */
    expect_u64("dominant_bank_class",
               merged.dominant_bank_class,
               0x20U);
    expect_u64("dominant_page_count",
               merged.dominant_page_count,
               7U);
    expect_double_close("dominant_fraction",
                        merged.dominant_fraction,
                        7.0 / 17.0);

    /*
     * merged vs merged overlap must be exactly 1.0.
     */
    rc = bank_profile_overlap_score(&merged, &merged, &score, &common_pages);
    if (rc != BANK_PROFILE_OK) {
        fprintf(stderr,
                "[FAIL] overlap merged/merged failed: %s\n",
                bank_profile_strerror(rc));
        return 1;
    }

    expect_double_close("merged vs merged score", score, 1.0);
    expect_u64("merged vs merged common_pages", common_pages, 17U);

    /*
     * A vs merged:
     * intersection = min(3,5) + min(5,5) = 8
     * union = 17
     */
    rc = bank_profile_overlap_score(&a, &merged, &score, &common_pages);
    if (rc != BANK_PROFILE_OK) {
        fprintf(stderr,
                "[FAIL] overlap A/merged failed: %s\n",
                bank_profile_strerror(rc));
        return 1;
    }

    expect_double_close("A vs merged score", score, 8.0 / 17.0);
    expect_u64("A vs merged common_pages", common_pages, 8U);

    /*
     * B vs merged:
     * intersection = min(2,5) + min(7,7) = 9
     * union = 17
     */
    rc = bank_profile_overlap_score(&b, &merged, &score, &common_pages);
    if (rc != BANK_PROFILE_OK) {
        fprintf(stderr,
                "[FAIL] overlap B/merged failed: %s\n",
                bank_profile_strerror(rc));
        return 1;
    }

    expect_double_close("B vs merged score", score, 9.0 / 17.0);
    expect_u64("B vs merged common_pages", common_pages, 9U);

    printf("[PASS] bank_profile_merge_test passed\n");
    printf("[INFO] merged entry_count=%zu pages=%" PRIu64 "\n",
           merged.entry_count,
           merged.pages_translated);
    printf("[INFO] dominant class=0x%" PRIx64 " pages=%" PRIu64
           " fraction=%.6f\n",
           merged.dominant_bank_class,
           merged.dominant_page_count,
           merged.dominant_fraction);

    return 0;
}
