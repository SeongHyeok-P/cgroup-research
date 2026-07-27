#define _GNU_SOURCE

#include "bank_profile.h"
#include "calibration.h"
#include "dram_mapping.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_RESULT_PATH "/run/protected-daemon/dram-map.json"
#define DEFAULT_PAGES_A 4096U
#define DEFAULT_PAGES_B 4096U
#define DEFAULT_STRIDE 16U
#define DEFAULT_MAX_SAMPLES 0U
#define DEFAULT_PRINT_ENTRIES 16U

struct child_proc {
    const char *name;
    pid_t pid;
    int stop_write_fd;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--pages-a N] [--pages-b N] [--stride N] "
            "[--max-samples N] [--print N]\n"
            "\n"
            "This test:\n"
            "  1. Loads " DEFAULT_RESULT_PATH "\n"
            "  2. Initializes dram_mapping\n"
            "  3. Forks two child processes with anonymous memory footprints\n"
            "  4. Builds bank_class profiles for both child processes\n"
            "  5. Computes profile self-overlap and child-A vs child-B overlap\n"
            "\n"
            "Options:\n"
            "  --pages-a N      Anonymous pages touched by child A, default 4096\n"
            "  --pages-b N      Anonymous pages touched by child B, default 4096\n"
            "  --stride N       Sample every N pages in selected VMAs, default 16\n"
            "  --max-samples N  Maximum pages sampled per process, default 0 = unlimited\n"
            "  --print N        Number of histogram entries to print per profile, default 16\n",
            prog);
}

static int parse_size_arg(const char *s, size_t *out)
{
    char *end = NULL;
    unsigned long value;

    if (s == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }

    *out = (size_t)value;
    return 0;
}

static int parse_args(
    int argc,
    char **argv,
    size_t *pages_a,
    size_t *pages_b,
    size_t *stride,
    size_t *max_samples,
    size_t *print_entries)
{
    int i;

    if (pages_a == NULL || pages_b == NULL || stride == NULL ||
        max_samples == NULL || print_entries == NULL) {
        return -1;
    }

    *pages_a = DEFAULT_PAGES_A;
    *pages_b = DEFAULT_PAGES_B;
    *stride = DEFAULT_STRIDE;
    *max_samples = DEFAULT_MAX_SAMPLES;
    *print_entries = DEFAULT_PRINT_ENTRIES;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pages-a") == 0) {
            if (i + 1 >= argc ||
                parse_size_arg(argv[i + 1], pages_a) < 0 ||
                *pages_a == 0U) {
                return -1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--pages-b") == 0) {
            if (i + 1 >= argc ||
                parse_size_arg(argv[i + 1], pages_b) < 0 ||
                *pages_b == 0U) {
                return -1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--stride") == 0) {
            if (i + 1 >= argc ||
                parse_size_arg(argv[i + 1], stride) < 0 ||
                *stride == 0U) {
                return -1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--max-samples") == 0) {
            if (i + 1 >= argc ||
                parse_size_arg(argv[i + 1], max_samples) < 0) {
                return -1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--print") == 0) {
            if (i + 1 >= argc ||
                parse_size_arg(argv[i + 1], print_entries) < 0) {
                return -1;
            }
            i++;
            continue;
        }

        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }

        return -1;
    }

    return 0;
}

static int load_dram_mapping(
    struct calibration_result *calibration,
    struct dram_mapping *mapping)
{
    int rc;
    size_t i;

    if (calibration == NULL || mapping == NULL) {
        return EXIT_FAILURE;
    }

    calibration_result_reset(calibration);
    dram_mapping_reset(mapping);

    rc = calibration_load_result(DEFAULT_RESULT_PATH, calibration);
    if (rc != CALIBRATION_OK) {
        fprintf(stderr,
                "[ERROR] failed to load calibration JSON %s: %s\n",
                DEFAULT_RESULT_PATH,
                calibration_strerror(rc));
        fprintf(stderr,
                "[HINT] Run protected_daemon or addr_translate_dram_test first "
                "to create a valid dram-map.json.\n");
        return EXIT_FAILURE;
    }

    rc = dram_mapping_init_from_calibration(mapping, calibration);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr,
                "[ERROR] dram_mapping_init_from_calibration failed: %s\n",
                dram_mapping_strerror(rc));
        return EXIT_FAILURE;
    }

    printf("[INFO] DRAM mapping ready\n");
    printf("[INFO] mask_set_id=%s mask_count=%zu\n",
           mapping->mask_set_id,
           mapping->mask_count);
    printf("[INFO] holdout precision=%.4f recall=%.4f\n",
           calibration->holdout.precision,
           calibration->holdout.recall);

    for (i = 0; i < mapping->mask_count; i++) {
        printf("[INFO] bank_mask[%zu]=0x%016" PRIx64 "\n",
               i,
               mapping->masks[i]);
    }

    return EXIT_SUCCESS;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len > 0U) {
        ssize_t nwritten = write(fd, p, len);

        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (nwritten == 0) {
            return -1;
        }

        p += (size_t)nwritten;
        len -= (size_t)nwritten;
    }

    return 0;
}

static int read_one_byte(int fd, char *out)
{
    ssize_t nread;

    if (out == NULL) {
        return -1;
    }

    do {
        nread = read(fd, out, 1U);
    } while (nread < 0 && errno == EINTR);

    return nread == 1 ? 0 : -1;
}

static void touch_child_pages(unsigned char *buf, size_t pages, size_t page_size, unsigned char seed)
{
    size_t i;
    volatile uint64_t checksum = 0U;

    for (i = 0; i < pages; i++) {
        size_t offset = i * page_size;

        /*
         * Touch a few cache lines per page. This forces physical allocation and
         * gives each child a real anonymous footprint visible through pagemap.
         */
        buf[offset] = (unsigned char)(seed + (i & 0x7fU));
        buf[offset + 64U] = (unsigned char)(seed ^ (i & 0x7fU));
        checksum += buf[offset];
        checksum += buf[offset + 64U];
    }

    /*
     * Prevent the compiler from proving the writes useless.
     */
    if (checksum == 0xdeadbeefU) {
        (void)write(STDERR_FILENO, "checksum\n", 9U);
    }
}

static int child_main(
    const char *name,
    size_t pages,
    int ready_write_fd,
    int stop_read_fd,
    unsigned char seed)
{
    long page_size_long;
    size_t page_size;
    size_t mapping_size;
    unsigned char *buf;
    char ready = 'R';
    char stop = '\0';

    page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        return EXIT_FAILURE;
    }

    page_size = (size_t)page_size_long;

    if (pages > SIZE_MAX / page_size) {
        return EXIT_FAILURE;
    }

    mapping_size = pages * page_size;

    buf = mmap(NULL,
               mapping_size,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);
    if (buf == MAP_FAILED) {
        return EXIT_FAILURE;
    }

    touch_child_pages(buf, pages, page_size, seed);

    printf("[CHILD %s] pid=%ld touched pages=%zu bytes=%zu ptr=%p\n",
           name,
           (long)getpid(),
           pages,
           mapping_size,
           (void *)buf);
    fflush(stdout);

    if (write_all(ready_write_fd, &ready, sizeof(ready)) < 0) {
        (void)munmap(buf, mapping_size);
        return EXIT_FAILURE;
    }

    /*
     * Stay alive while the parent reads /proc/[pid]/maps and pagemap.
     */
    while (read(stop_read_fd, &stop, 1U) < 0 && errno == EINTR) {
    }

    touch_child_pages(buf, pages, page_size, (unsigned char)(seed + 1U));

    (void)munmap(buf, mapping_size);
    return EXIT_SUCCESS;
}

static int spawn_child(
    const char *name,
    size_t pages,
    unsigned char seed,
    struct child_proc *child)
{
    int ready_pipe[2] = {-1, -1};
    int stop_pipe[2] = {-1, -1};
    pid_t pid;
    char ready;

    if (name == NULL || child == NULL || pages == 0U) {
        return -1;
    }

    if (pipe(ready_pipe) < 0) {
        perror("pipe ready");
        return -1;
    }

    if (pipe(stop_pipe) < 0) {
        perror("pipe stop");
        (void)close(ready_pipe[0]);
        (void)close(ready_pipe[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        (void)close(ready_pipe[0]);
        (void)close(ready_pipe[1]);
        (void)close(stop_pipe[0]);
        (void)close(stop_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        (void)close(ready_pipe[0]);
        (void)close(stop_pipe[1]);

        exit(child_main(name,
                        pages,
                        ready_pipe[1],
                        stop_pipe[0],
                        seed));
    }

    (void)close(ready_pipe[1]);
    (void)close(stop_pipe[0]);

    if (read_one_byte(ready_pipe[0], &ready) < 0 || ready != 'R') {
        fprintf(stderr, "[ERROR] child %s did not signal ready\n", name);
        (void)kill(pid, SIGKILL);
        (void)close(ready_pipe[0]);
        (void)close(stop_pipe[1]);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }

    (void)close(ready_pipe[0]);

    child->name = name;
    child->pid = pid;
    child->stop_write_fd = stop_pipe[1];

    printf("[INFO] child %s ready: pid=%ld pages=%zu\n",
           name,
           (long)pid,
           pages);

    return 0;
}

static void stop_child(struct child_proc *child)
{
    char stop = 'S';
    int status;

    if (child == NULL || child->pid <= 0) {
        return;
    }

    if (child->stop_write_fd >= 0) {
        (void)write_all(child->stop_write_fd, &stop, sizeof(stop));
        (void)close(child->stop_write_fd);
        child->stop_write_fd = -1;
    }

    if (waitpid(child->pid, &status, 0) < 0) {
        perror("waitpid");
    } else if (WIFEXITED(status)) {
        printf("[INFO] child %s exited with code %d\n",
               child->name,
               WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("[INFO] child %s killed by signal %d\n",
               child->name,
               WTERMSIG(status));
    }

    child->pid = -1;
}

static int build_and_print_profile(
    const char *name,
    pid_t pid,
    const struct dram_mapping *mapping,
    const struct bank_profile_config *cfg,
    size_t print_entries,
    struct bank_profile *profile)
{
    int rc;

    printf("\n[INFO] building profile for %s pid=%ld\n",
           name,
           (long)pid);

    rc = bank_profile_build(pid, mapping, cfg, profile);
    if (rc != BANK_PROFILE_OK) {
        fprintf(stderr,
                "[ERROR] bank_profile_build failed for %s: %s\n",
                name,
                bank_profile_strerror(rc));
        fprintf(stderr,
                "[ERROR] detail: %s\n",
                profile->error_message);
        return -1;
    }

    bank_profile_print(stdout, profile, print_entries);
    return 0;
}

static int print_overlap(
    const char *label,
    const struct bank_profile *a,
    const struct bank_profile *b)
{
    double score = 0.0;
    uint64_t common_pages = 0U;
    int rc;

    rc = bank_profile_overlap_score(a, b, &score, &common_pages);
    if (rc != BANK_PROFILE_OK) {
        fprintf(stderr,
                "[ERROR] bank_profile_overlap_score failed for %s: %s\n",
                label,
                bank_profile_strerror(rc));
        return -1;
    }

    printf("[OVERLAP] %-12s score=%.6f common_pages=%" PRIu64 "\n",
           label,
           score,
           common_pages);

    if (score < 0.0 || score > 1.0) {
        fprintf(stderr,
                "[ERROR] overlap score out of range for %s: %.6f\n",
                label,
                score);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct calibration_result calibration;
    struct dram_mapping mapping;
    struct bank_profile_config cfg;
    struct bank_profile profile_a;
    struct bank_profile profile_b;
    struct child_proc child_a = {0};
    struct child_proc child_b = {0};
    size_t pages_a;
    size_t pages_b;
    size_t stride;
    size_t max_samples;
    size_t print_entries;
    int exit_code = EXIT_FAILURE;

    if (parse_args(argc,
                   argv,
                   &pages_a,
                   &pages_b,
                   &stride,
                   &max_samples,
                   &print_entries) < 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    printf("[INFO] bank_profile_overlap_test starting\n");
    printf("[INFO] parent pid=%ld\n", (long)getpid());

    if (load_dram_mapping(&calibration, &mapping) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }

    bank_profile_config_default(&cfg);
    cfg.sample_stride_pages = stride;
    cfg.max_pages_to_sample = max_samples;
    cfg.include_anonymous = true;
    cfg.include_heap = true;
    cfg.include_stack = true;
    cfg.include_file_backed = false;
    cfg.include_readonly = false;

    printf("[INFO] profile config: stride=%zu max_samples=%zu "
           "anon=%s heap=%s stack=%s file_backed=%s readonly=%s\n",
           cfg.sample_stride_pages,
           cfg.max_pages_to_sample,
           cfg.include_anonymous ? "true" : "false",
           cfg.include_heap ? "true" : "false",
           cfg.include_stack ? "true" : "false",
           cfg.include_file_backed ? "true" : "false",
           cfg.include_readonly ? "true" : "false");

    if (spawn_child("A", pages_a, 0x31U, &child_a) < 0) {
        goto cleanup;
    }

    if (spawn_child("B", pages_b, 0x71U, &child_b) < 0) {
        goto cleanup;
    }

    if (build_and_print_profile("child-A",
                                child_a.pid,
                                &mapping,
                                &cfg,
                                print_entries,
                                &profile_a) < 0) {
        goto cleanup;
    }

    if (build_and_print_profile("child-B",
                                child_b.pid,
                                &mapping,
                                &cfg,
                                print_entries,
                                &profile_b) < 0) {
        goto cleanup;
    }

    printf("\n[INFO] overlap scores\n");

    if (print_overlap("A vs A", &profile_a, &profile_a) < 0) {
        goto cleanup;
    }

    if (print_overlap("B vs B", &profile_b, &profile_b) < 0) {
        goto cleanup;
    }

    if (print_overlap("A vs B", &profile_a, &profile_b) < 0) {
        goto cleanup;
    }

    printf("\n[INFO] sanity checks\n");
    printf("[INFO] profile A translated pages=%" PRIu64
           " unique_classes=%zu dominant=0x%" PRIx64 "\n",
           profile_a.pages_translated,
           profile_a.entry_count,
           profile_a.dominant_bank_class);
    printf("[INFO] profile B translated pages=%" PRIu64
           " unique_classes=%zu dominant=0x%" PRIx64 "\n",
           profile_b.pages_translated,
           profile_b.entry_count,
           profile_b.dominant_bank_class);

    if (profile_a.pages_translated == 0U ||
        profile_b.pages_translated == 0U ||
        profile_a.entry_count == 0U ||
        profile_b.entry_count == 0U) {
        fprintf(stderr, "[ERROR] empty profile detected\n");
        goto cleanup;
    }

    printf("\n[INFO] bank_profile_overlap_test passed\n");
    exit_code = EXIT_SUCCESS;

cleanup:
    stop_child(&child_b);
    stop_child(&child_a);

    return exit_code;
}
