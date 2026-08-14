#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "addr_translate.h"
#include "calibration.h"
#include "dram_mapping.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RESULT_PATH "/run/protected-daemon/dram-map.json"

#define MIB_BYTES UINT64_C(1048576)
#define CACHE_LINE_BYTES 64U

static volatile uintptr_t sink_pointer = 0U;


static uint64_t rng_next(uint64_t *state)
{
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;

    *state = x;

    return x * UINT64_C(2685821657736338717);
}


static double elapsed_sec(
    const struct timespec *start,
    const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) /
               1000000000.0;
}


static int parse_u64(
    const char *text,
    int base,
    uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out == NULL) {
        return -1;
    }

    errno = 0;

    value = strtoull(
        text,
        &end,
        base);

    if (errno != 0 ||
        end == text ||
        *end != '\0') {

        return -1;
    }

    *out = (uint64_t)value;

    return 0;
}


static int load_mapping(
    struct dram_mapping *mapping)
{
    struct calibration_result result;
    int rc;

    if (mapping == NULL) {
        return -1;
    }

    calibration_result_reset(&result);
    dram_mapping_reset(mapping);

    rc = calibration_load_result(
        RESULT_PATH,
        &result);

    if (rc != CALIBRATION_OK) {

        fprintf(
            stderr,
            "error: calibration_load_result: %s\n",
            calibration_strerror(rc));

        return -1;
    }

    rc = dram_mapping_init_from_calibration(
        mapping,
        &result);

    if (rc != DRAM_MAPPING_OK) {

        fprintf(
            stderr,
            "error: dram_mapping_init_from_calibration: %s\n",
            dram_mapping_strerror(rc));

        return -1;
    }

    return 0;
}


static int find_minimum_mask_bit(
    const struct dram_mapping *mapping,
    unsigned int *bit_out)
{
    unsigned int minimum = 64U;
    size_t i;

    if (mapping == NULL ||
        bit_out == NULL ||
        !dram_mapping_is_ready(mapping)) {

        return -1;
    }

    for (i = 0U;
         i < mapping->mask_count;
         i++) {

        unsigned int bit;

        for (bit = 0U;
             bit < 64U;
             bit++) {

            if ((mapping->masks[i] &
                 (UINT64_C(1) << bit)) != 0U) {

                if (bit < minimum) {
                    minimum = bit;
                }

                break;
            }
        }
    }

    if (minimum >= 64U) {
        return -1;
    }

    *bit_out = minimum;

    return 0;
}


static void shuffle_lines(
    uintptr_t *lines,
    size_t count)
{
    uint64_t state =
        UINT64_C(0x123456789abcdef);

    size_t i;

    if (lines == NULL ||
        count < 2U) {

        return;
    }

    for (i = count - 1U;
         i > 0U;
         i--) {

        size_t j;

        uintptr_t tmp;

        j = (size_t)(
            rng_next(&state) %
            (uint64_t)(i + 1U));

        tmp = lines[i];

        lines[i] = lines[j];
        lines[j] = tmp;
    }
}


static int release_unselected_pages(
    unsigned char *buffer,
    const unsigned char *keep_page,
    size_t scanned_pages,
    size_t page_size)
{
    size_t i = 0U;

    while (i < scanned_pages) {

        size_t start;
        size_t length_pages;

        if (keep_page[i] != 0U) {

            i++;

            continue;
        }

        start = i;

        while (i < scanned_pages &&
               keep_page[i] == 0U) {

            i++;
        }

        length_pages =
            i - start;

        if (madvise(
                buffer + start * page_size,
                length_pages * page_size,
                MADV_DONTNEED) != 0) {

            return -1;
        }
    }

    return 0;
}


int main(
    int argc,
    char **argv)
{
    struct dram_mapping mapping;
    struct addr_translate_ctx at;

    uint64_t target_class;
    uint64_t candidate_mib;
    uint64_t selected_mib;
    uint64_t run_seconds;

    uint64_t candidate_bytes_u64;
    uint64_t selected_bytes_u64;

    unsigned int minimum_mask_bit;

    size_t class_granularity;

    long page_size_long;

    size_t page_size;
    size_t page_count;
    size_t candidate_bytes;

    size_t required_blocks;
    size_t required_lines;

    unsigned char *buffer =
        MAP_FAILED;

    unsigned char *keep_page =
        NULL;

    uintptr_t *lines =
        NULL;

    size_t selected_blocks =
        0U;

    size_t selected_lines =
        0U;

    size_t retained_pages =
        0U;

    size_t scanned_pages =
        0U;

    size_t page_index;

    int rc;

    uintptr_t current;

    uint64_t total_reads =
        0U;

    const uint64_t chunk_reads =
        UINT64_C(100000);

    struct timespec start;
    struct timespec now;

    double seconds;
    double ns_per_read;

    bool wait_for_cont =
        false;


    if (argc != 5 &&
        argc != 6) {

        fprintf(
            stderr,
            "usage: %s "
            "<target_class> "
            "<candidate_MiB> "
            "<selected_MiB> "
            "<run_seconds> "
            "[--wait]\n",
            argv[0]);

        return EXIT_FAILURE;
    }


    if (argc == 6) {

        if (strcmp(
                argv[5],
                "--wait") != 0) {

            fprintf(
                stderr,
                "error: unknown option: %s\n",
                argv[5]);

            return EXIT_FAILURE;
        }

        wait_for_cont = true;
    }


    if (parse_u64(
            argv[1],
            0,
            &target_class) != 0 ||

        parse_u64(
            argv[2],
            10,
            &candidate_mib) != 0 ||

        parse_u64(
            argv[3],
            10,
            &selected_mib) != 0 ||

        parse_u64(
            argv[4],
            10,
            &run_seconds) != 0 ||

        candidate_mib == 0U ||
        selected_mib == 0U ||
        run_seconds == 0U) {

        fprintf(
            stderr,
            "error: invalid argument\n");

        return EXIT_FAILURE;
    }


    if (load_mapping(
            &mapping) != 0) {

        return EXIT_FAILURE;
    }


    if (mapping.mask_count >= 64U ||

        target_class >=
            (UINT64_C(1) <<
             mapping.mask_count)) {

        fprintf(
            stderr,
            "error: target class "
            "0x%" PRIx64
            " is outside class space "
            "for %zu masks\n",
            target_class,
            mapping.mask_count);

        return EXIT_FAILURE;
    }


    if (find_minimum_mask_bit(
            &mapping,
            &minimum_mask_bit) != 0) {

        fprintf(
            stderr,
            "error: failed to determine "
            "class granularity\n");

        return EXIT_FAILURE;
    }


    if (minimum_mask_bit >=
        sizeof(size_t) * 8U) {

        fprintf(
            stderr,
            "error: class granularity "
            "overflow\n");

        return EXIT_FAILURE;
    }


    class_granularity =
        ((size_t)1U) <<
        minimum_mask_bit;


    if (class_granularity <
            CACHE_LINE_BYTES ||

        class_granularity %
            CACHE_LINE_BYTES != 0U) {

        fprintf(
            stderr,
            "error: class granularity %zu "
            "is incompatible with "
            "%u-byte cache lines\n",
            class_granularity,
            CACHE_LINE_BYTES);

        return EXIT_FAILURE;
    }


    page_size_long =
        sysconf(_SC_PAGESIZE);

    if (page_size_long <= 0) {

        fprintf(
            stderr,
            "error: sysconf(_SC_PAGESIZE) "
            "failed\n");

        return EXIT_FAILURE;
    }


    page_size =
        (size_t)page_size_long;


    if (class_granularity >
            page_size ||

        page_size %
            class_granularity != 0U) {

        fprintf(
            stderr,
            "error: unsupported class "
            "granularity %zu for "
            "page size %zu\n",
            class_granularity,
            page_size);

        return EXIT_FAILURE;
    }


    if (candidate_mib >
            UINT64_MAX / MIB_BYTES ||

        selected_mib >
            UINT64_MAX / MIB_BYTES) {

        fprintf(
            stderr,
            "error: MiB conversion "
            "overflow\n");

        return EXIT_FAILURE;
    }


    candidate_bytes_u64 =
        candidate_mib *
        MIB_BYTES;

    selected_bytes_u64 =
        selected_mib *
        MIB_BYTES;


    if (candidate_bytes_u64 >
            (uint64_t)SIZE_MAX ||

        selected_bytes_u64 >
            (uint64_t)SIZE_MAX) {

        fprintf(
            stderr,
            "error: size exceeds "
            "addressable range\n");

        return EXIT_FAILURE;
    }


    if (selected_bytes_u64 %
            class_granularity != 0U ||

        selected_bytes_u64 %
            CACHE_LINE_BYTES != 0U) {

        fprintf(
            stderr,
            "error: selected size must "
            "align to class/cache-line "
            "granularity\n");

        return EXIT_FAILURE;
    }


    candidate_bytes =
        (size_t)candidate_bytes_u64;


    if (candidate_bytes %
            page_size != 0U) {

        fprintf(
            stderr,
            "error: candidate size "
            "is not page aligned\n");

        return EXIT_FAILURE;
    }


    page_count =
        candidate_bytes /
        page_size;


    required_blocks =
        (size_t)(
            selected_bytes_u64 /
            class_granularity);


    required_lines =
        (size_t)(
            selected_bytes_u64 /
            CACHE_LINE_BYTES);


    keep_page =
        calloc(
            page_count,
            sizeof(*keep_page));


    lines =
        malloc(
            required_lines *
            sizeof(*lines));


    if (keep_page == NULL ||
        lines == NULL) {

        perror("allocation");

        free(keep_page);
        free(lines);

        return EXIT_FAILURE;
    }


    buffer =
        mmap(
            NULL,
            candidate_bytes,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0);


    if (buffer ==
        MAP_FAILED) {

        perror("mmap");

        free(keep_page);
        free(lines);

        return EXIT_FAILURE;
    }


#ifdef MADV_NOHUGEPAGE

    if (madvise(
            buffer,
            candidate_bytes,
            MADV_NOHUGEPAGE) != 0) {

        perror(
            "warning: "
            "madvise(MADV_NOHUGEPAGE)");
    }

#endif


    addr_translate_reset(&at);


    rc =
        addr_translate_open(
            &at,
            getpid());


    if (rc !=
        ADDR_TRANSLATE_OK) {

        fprintf(
            stderr,
            "error: addr_translate_open: "
            "%s\n",
            addr_translate_strerror(rc));

        (void)munmap(
            buffer,
            candidate_bytes);

        free(keep_page);
        free(lines);

        return EXIT_FAILURE;
    }


    for (page_index = 0U;
         page_index < page_count &&
         selected_blocks <
             required_blocks;
         page_index++) {

        struct addr_translation tr;

        unsigned char *page;

        size_t block_offset;


        page =
            buffer +
            page_index *
            page_size;


        /*
         * Fault this 4 KiB page in before
         * reading pagemap.
         */
        page[0] =
            (unsigned char)(
                page_index & 0xffU);


        scanned_pages =
            page_index + 1U;


        rc =
            addr_translate_va_to_pa(
                &at,
                (uint64_t)(
                    uintptr_t)page,
                &tr);


        if (rc !=
            ADDR_TRANSLATE_OK) {

            fprintf(
                stderr,
                "error: translation "
                "failed at page %zu: %s\n",
                page_index,
                addr_translate_strerror(rc));

            addr_translate_close(&at);

            (void)munmap(
                buffer,
                candidate_bytes);

            free(keep_page);
            free(lines);

            return EXIT_FAILURE;
        }


        for (block_offset = 0U;
             block_offset < page_size &&
             selected_blocks <
                 required_blocks;
             block_offset +=
                 class_granularity) {

            uint64_t bank_class;


            bank_class =
                dram_mapping_bank_class_fast(
                    &mapping,
                    tr.physical_address +
                    (uint64_t)block_offset);


            if (bank_class ==
                target_class) {

                size_t line_offset;


                if (keep_page[
                        page_index] == 0U) {

                    keep_page[
                        page_index] = 1U;

                    retained_pages++;
                }


                /*
                 * minimum mask bit is 8,
                 * so no recovered-class bit
                 * changes within this 256 B
                 * block.
                 *
                 * Use every 64 B cache line:
                 *
                 * +0
                 * +64
                 * +128
                 * +192
                 */
                for (line_offset = 0U;
                     line_offset <
                         class_granularity;
                     line_offset +=
                         CACHE_LINE_BYTES) {

                    lines[
                        selected_lines++] =
                        (uintptr_t)(
                            page +
                            block_offset +
                            line_offset);
                }


                selected_blocks++;
            }
        }
    }


    addr_translate_close(&at);


    if (selected_blocks !=
            required_blocks ||

        selected_lines !=
            required_lines) {

        fprintf(
            stderr,
            "error: target class "
            "0x%" PRIx64
            " capacity insufficient: "
            "blocks=%zu/%zu "
            "lines=%zu/%zu\n",
            target_class,
            selected_blocks,
            required_blocks,
            selected_lines,
            required_lines);

        (void)munmap(
            buffer,
            candidate_bytes);

        free(keep_page);
        free(lines);

        return EXIT_FAILURE;
    }


    /*
     * Drop candidate pages that contain
     * no selected target-class block.
     *
     * Selected PFNs stay resident.
     */
    if (release_unselected_pages(
            buffer,
            keep_page,
            scanned_pages,
            page_size) != 0) {

        perror(
            "madvise(MADV_DONTNEED)");

        (void)munmap(
            buffer,
            candidate_bytes);

        free(keep_page);
        free(lines);

        return EXIT_FAILURE;
    }


    free(keep_page);
    keep_page = NULL;


    /*
     * Same deterministic shuffle for
     * every target class.
     */
    shuffle_lines(
        lines,
        selected_lines);


    /*
     * Store next-node VA in each selected
     * cache line.
     */
    for (page_index = 0U;
         page_index <
             selected_lines;
         page_index++) {

        size_t next_index;

        uintptr_t *slot;


        next_index =
            (page_index + 1U) %
            selected_lines;


        slot =
            (uintptr_t *)(
                uintptr_t)
                lines[page_index];


        *slot =
            lines[next_index];
    }


    /*
     * One full chain warm-up before READY.
     */
    current =
        lines[0];


    for (page_index = 0U;
         page_index <
             selected_lines;
         page_index++) {

        current =
            *(volatile uintptr_t *)(
                uintptr_t)current;
    }


    sink_pointer =
        current;


    fprintf(
        stderr,
        "READY "
        "pid=%ld "
        "class=0x%02" PRIx64 " "
        "candidate=%" PRIu64 "MiB "
        "selected=%" PRIu64 "MiB "
        "scanned_pages=%zu "
        "retained_pages=%zu "
        "blocks=%zu "
        "lines=%zu\n",
        (long)getpid(),
        target_class,
        candidate_mib,
        selected_mib,
        scanned_pages,
        retained_pages,
        selected_blocks,
        selected_lines);


    fflush(stderr);


    /*
     * C2 synchronization mode.
     *
     * Default C1 smoke test does not stop.
     */
    if (wait_for_cont) {

        if (raise(SIGSTOP) != 0) {

            perror("raise(SIGSTOP)");

            (void)munmap(
                buffer,
                candidate_bytes);

            free(lines);

            return EXIT_FAILURE;
        }
    }


    if (clock_gettime(
            CLOCK_MONOTONIC_RAW,
            &start) != 0) {

        perror("clock_gettime");

        (void)munmap(
            buffer,
            candidate_bytes);

        free(lines);

        return EXIT_FAILURE;
    }


    now =
        start;


    for (;;) {

        uint64_t i;


        for (i = 0U;
             i < chunk_reads;
             i++) {

            current =
                *(volatile uintptr_t *)(
                    uintptr_t)current;
        }


        total_reads +=
            chunk_reads;


        if (clock_gettime(
                CLOCK_MONOTONIC_RAW,
                &now) != 0) {

            perror("clock_gettime");

            (void)munmap(
                buffer,
                candidate_bytes);

            free(lines);

            return EXIT_FAILURE;
        }


        if (elapsed_sec(
                &start,
                &now) >=
            (double)run_seconds) {

            break;
        }
    }


    sink_pointer =
        current;


    seconds =
        elapsed_sec(
            &start,
            &now);


    ns_per_read =
        (seconds *
         1000000000.0) /
        (double)total_reads;


    printf(
        "mask_set_id,%s\n",
        mapping.mask_set_id);

    printf(
        "target_class,0x%02"
        PRIx64 "\n",
        target_class);

    printf(
        "candidate_mib,%"
        PRIu64 "\n",
        candidate_mib);

    printf(
        "selected_mib,%"
        PRIu64 "\n",
        selected_mib);

    printf(
        "scanned_pages,%zu\n",
        scanned_pages);

    printf(
        "retained_pages,%zu\n",
        retained_pages);

    printf(
        "selected_blocks,%zu\n",
        selected_blocks);

    printf(
        "selected_lines,%zu\n",
        selected_lines);

    printf(
        "elapsed_sec,%.6f\n",
        seconds);

    printf(
        "total_reads,%"
        PRIu64 "\n",
        total_reads);

    printf(
        "ns_per_read,%.3f\n",
        ns_per_read);


    (void)munmap(
        buffer,
        candidate_bytes);

    free(lines);

    return EXIT_SUCCESS;
}
