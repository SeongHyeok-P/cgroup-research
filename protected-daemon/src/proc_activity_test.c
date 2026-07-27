#define _POSIX_C_SOURCE 200809L

#include "proc_activity.h"

#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

static void burn_cpu(void)
{
    volatile uint64_t x = 0U;
    uint64_t i;

    for (i = 0U; i < 200000000ULL; i++) {
        x += i ^ (x << 1U);
    }

    if (x == 42U) {
        printf("x=%" PRIu64 "\n", x);
    }
}

int main(void)
{
    struct proc_activity_config cfg;
    struct proc_activity_sample s1;
    struct proc_activity_sample s2;
    struct proc_activity_delta d;
    int rc;
    pid_t pid = getpid();

    proc_activity_config_default(&cfg);

    rc = proc_activity_sample_now(pid, &s1);
    if (rc != PROC_ACTIVITY_OK) {
        fprintf(stderr, "sample1 failed: %s\n", proc_activity_strerror(rc));
        return 1;
    }

    burn_cpu();

    rc = proc_activity_sample_now(pid, &s2);
    if (rc != PROC_ACTIVITY_OK) {
        fprintf(stderr, "sample2 failed: %s\n", proc_activity_strerror(rc));
        return 1;
    }

    rc = proc_activity_compute_delta(&s1, &s2, &cfg, &d);
    if (rc != PROC_ACTIVITY_OK) {
        fprintf(stderr, "delta failed: %s\n", proc_activity_strerror(rc));
        return 1;
    }

    printf("pid=%ld comm=%s state=%c\n", (long)s2.pid, s2.comm, s2.state);
    printf("elapsed_sec=%.6f\n", d.elapsed_sec);
    printf("cpu_ticks_delta=%" PRIu64 "\n", d.cpu_ticks_delta);
    printf("cpu_seconds=%.6f\n", d.cpu_seconds);
    printf("cpu_ratio=%.6f\n", d.cpu_ratio);
    printf("minflt_delta=%" PRIu64 "\n", d.minflt_delta);
    printf("majflt_delta=%" PRIu64 "\n", d.majflt_delta);
    printf("weighted_faults=%.2f\n", d.weighted_faults);
    printf("faults_per_sec=%.2f\n", d.faults_per_sec);
    printf("rss_mib=%.2f\n", d.rss_mib);
    printf("cpu_score=%.4f fault_score=%.4f rss_score=%.4f total_score=%.4f\n",
           d.cpu_score,
           d.fault_score,
           d.rss_score,
           d.total_score);
    printf("active=%s\n", d.active ? "true" : "false");
    printf("reason=%s\n", d.reason);

    return 0;
}
