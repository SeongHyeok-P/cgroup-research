#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "interference_detector.h"

#include "calibration.h"
#include "dram_mapping.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_RESULT_PATH "/run/protected-daemon/dram-map.json"
#define TEST_PROCESS_COUNT 2U
#define TEST_CANDIDATE_CAP 8U

static void sleep_ms(long ms)
{
    struct timespec req;
    struct timespec rem;

    req.tv_sec = ms / 1000L;
    req.tv_nsec = (ms % 1000L) * 1000000L;

    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) {
            return;
        }
        req = rem;
    }
}

static void child_memory_worker(size_t pages)
{
    long page_size_long;
    size_t page_size;
    size_t bytes;
    volatile unsigned char *mem;
    size_t i;

    page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        _exit(2);
    }

    page_size = (size_t)page_size_long;
    bytes = pages * page_size;

    mem = mmap(NULL,
               bytes,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);
    if (mem == MAP_FAILED) {
        _exit(3);
    }

    for (i = 0U; i < pages; i++) {
        mem[i * page_size] = (unsigned char)i;
    }

    for (;;) {
        for (i = 0U; i < pages; i++) {
            mem[i * page_size]++;
        }
    }
}

static pid_t spawn_worker(size_t pages)
{
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        child_memory_worker(pages);
        _exit(0);
    }

    return pid;
}

static void stop_worker(pid_t pid)
{
    int status;

    if (pid <= 0) {
        return;
    }

    (void)kill(pid, SIGTERM);
    (void)waitpid(pid, &status, 0);
}

static int load_mapping(struct dram_mapping *mapping)
{
    struct calibration_result calibration;
    int rc;

    calibration_result_reset(&calibration);
    dram_mapping_reset(mapping);

    rc = calibration_load_result(TEST_RESULT_PATH, &calibration);
    if (rc != CALIBRATION_OK) {
        fprintf(stderr,
                "[FAIL] failed to load calibration JSON %s: %s\n",
                TEST_RESULT_PATH,
                calibration_strerror(rc));
        return 1;
    }

    rc = dram_mapping_init_from_calibration(mapping, &calibration);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr,
                "[FAIL] failed to initialize DRAM mapping: %s\n",
                dram_mapping_strerror(rc));
        return 1;
    }

    return 0;
}

int main(void)
{
    struct dram_mapping mapping;
    struct interference_detector_config cfg;
    struct interference_process_state processes[TEST_PROCESS_COUNT];
    struct interference_candidate candidates[TEST_CANDIDATE_CAP];
    struct interference_detector_stats stats;
    size_t candidate_count = 0U;
    pid_t protected_pid = -1;
    pid_t normal_pid = -1;
    int rc;
    size_t i;

    if (load_mapping(&mapping) != 0) {
        return 1;
    }

    interference_detector_config_default(&cfg);

    cfg.max_normal_candidates = 4U;
    cfg.activity_score_threshold = 0.0;
    cfg.overlap_score_threshold = 0.30;
    cfg.throttle_score_threshold = 0.20;
    cfg.profile_config.max_pages_to_sample = 1024U;
    cfg.profile_config.sample_stride_pages = 16U;

    protected_pid = spawn_worker(4096U);
    if (protected_pid < 0) {
        fprintf(stderr, "[FAIL] failed to spawn protected worker\n");
        return 1;
    }

    normal_pid = spawn_worker(4096U);
    if (normal_pid < 0) {
        fprintf(stderr, "[FAIL] failed to spawn normal worker\n");
        stop_worker(protected_pid);
        return 1;
    }

    sleep_ms(300L);

    interference_process_state_reset(&processes[0]);
    processes[0].pid = protected_pid;
    processes[0].role = INTERFERENCE_PROCESS_PROTECTED;
    processes[0].alive = true;

    interference_process_state_reset(&processes[1]);
    processes[1].pid = normal_pid;
    processes[1].role = INTERFERENCE_PROCESS_NORMAL;
    processes[1].alive = true;

    /*
     * First run seeds activity samples for normal processes.
     * A normal process generally needs two samples before a delta exists.
     */
    rc = interference_detector_run_once(
        processes,
        TEST_PROCESS_COUNT,
        &mapping,
        &cfg,
        candidates,
        TEST_CANDIDATE_CAP,
        &candidate_count,
        &stats);

    if (rc != INTERFERENCE_DETECTOR_OK) {
        fprintf(stderr,
                "[FAIL] first detector run failed: %s reason=%s\n",
                interference_detector_strerror(rc),
                stats.last_error_reason);
        stop_worker(protected_pid);
        stop_worker(normal_pid);
        return 1;
    }

    sleep_ms(500L);

    rc = interference_detector_run_once(
        processes,
        TEST_PROCESS_COUNT,
        &mapping,
        &cfg,
        candidates,
        TEST_CANDIDATE_CAP,
        &candidate_count,
        &stats);

    stop_worker(protected_pid);
    stop_worker(normal_pid);

    if (rc != INTERFERENCE_DETECTOR_OK) {
        fprintf(stderr,
                "[FAIL] second detector run failed: %s reason=%s\n",
                interference_detector_strerror(rc),
                stats.last_error_reason);
        return 1;
    }

    printf("[INFO] stats: process_count=%zu protected=%zu normal=%zu\n",
           stats.process_count,
           stats.protected_processes,
           stats.normal_processes);
    printf("[INFO] stats: protected_profiles=%zu activity_samples=%zu active_normals=%zu normal_profiles=%zu outputs=%zu\n",
           stats.protected_profiles_built,
           stats.activity_samples,
           stats.active_normal_candidates,
           stats.normal_profiles_built,
           stats.output_candidates);

    for (i = 0U; i < candidate_count; i++) {
        printf("[CANDIDATE] pid=%ld activity=%.4f cpu=%.4f fault=%.4f rss=%.4f overlap=%.4f final=%.4f common=%" PRIu64 " throttle=%s\n",
               (long)candidates[i].pid,
               candidates[i].activity_score,
               candidates[i].cpu_score,
               candidates[i].fault_score,
               candidates[i].rss_score,
               candidates[i].overlap_score,
               candidates[i].final_score,
               candidates[i].common_pages,
               candidates[i].should_throttle ? "true" : "false");
        printf("[REASON] %s\n", candidates[i].reason);
    }

    if (candidate_count == 0U) {
        fprintf(stderr, "[FAIL] expected at least one candidate\n");
        return 1;
    }

    printf("[PASS] interference_detector_test passed\n");

    return 0;
}
