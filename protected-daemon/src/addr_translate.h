#ifndef PROTECTED_DAEMON_ADDR_TRANSLATE_H
#define PROTECTED_DAEMON_ADDR_TRANSLATE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum addr_translate_rc {
    ADDR_TRANSLATE_OK = 0,
    ADDR_TRANSLATE_ERR_INVALID_ARG = -1,
    ADDR_TRANSLATE_ERR_PAGE_SIZE = -2,
    ADDR_TRANSLATE_ERR_OPEN = -3,
    ADDR_TRANSLATE_ERR_READ = -4,
    ADDR_TRANSLATE_ERR_NOT_PRESENT = -5,
    ADDR_TRANSLATE_ERR_SWAPPED = -6,
    ADDR_TRANSLATE_ERR_PFN_UNAVAILABLE = -7,
    ADDR_TRANSLATE_ERR_OVERFLOW = -8
};

struct addr_translate_ctx {
    pid_t pid;
    int pagemap_fd;
    uint64_t page_size;
};

struct addr_translation {
    pid_t pid;

    uint64_t virtual_address;
    uint64_t physical_address;

    uint64_t page_size;
    uint64_t page_index;
    uint64_t page_offset;

    uint64_t pagemap_entry;
    uint64_t pfn;

    bool present;
    bool swapped;
    bool soft_dirty;
    bool exclusively_mapped;
    bool uffd_wp;
    bool guard_region;
    bool file_or_shared_anon;
};

void addr_translate_reset(struct addr_translate_ctx *ctx);

int addr_translate_open(struct addr_translate_ctx *ctx, pid_t pid);

void addr_translate_close(struct addr_translate_ctx *ctx);

int addr_translate_va_to_pa(
    struct addr_translate_ctx *ctx,
    uint64_t virtual_address,
    struct addr_translation *out);

/*
 * Convenience helper for one-shot translation.
 * For repeated translations from the same process, prefer:
 *
 *   addr_translate_open()
 *   addr_translate_va_to_pa()
 *   addr_translate_close()
 */
int addr_translate_pid_va_to_pa(
    pid_t pid,
    uint64_t virtual_address,
    struct addr_translation *out);

const char *addr_translate_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* PROTECTED_DAEMON_ADDR_TRANSLATE_H */
