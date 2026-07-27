#define _POSIX_C_SOURCE 200809L

#include "addr_translate.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PAGEMAP_ENTRY_BYTES 8ULL

#define PAGEMAP_PFN_MASK          ((1ULL << 55U) - 1ULL)
#define PAGEMAP_SOFT_DIRTY_BIT    (1ULL << 55U)
#define PAGEMAP_EXCLUSIVE_BIT     (1ULL << 56U)
#define PAGEMAP_UFFD_WP_BIT       (1ULL << 57U)
#define PAGEMAP_GUARD_BIT         (1ULL << 58U)
#define PAGEMAP_FILE_SHARED_BIT   (1ULL << 61U)
#define PAGEMAP_SWAPPED_BIT       (1ULL << 62U)
#define PAGEMAP_PRESENT_BIT       (1ULL << 63U)

static uint64_t get_page_size(void)
{
    long ps = sysconf(_SC_PAGESIZE);

    if (ps <= 0) {
        return 0U;
    }

    return (uint64_t)ps;
}

void addr_translate_reset(struct addr_translate_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }

    ctx->pid = -1;
    ctx->pagemap_fd = -1;
    ctx->page_size = 0U;
}

int addr_translate_open(struct addr_translate_ctx *ctx, pid_t pid)
{
    char path[64];
    uint64_t page_size;
    int fd;
    int n;

    if (ctx == NULL || pid <= 0) {
        return ADDR_TRANSLATE_ERR_INVALID_ARG;
    }

    addr_translate_reset(ctx);

    page_size = get_page_size();
    if (page_size == 0U) {
        return ADDR_TRANSLATE_ERR_PAGE_SIZE;
    }

    n = snprintf(path, sizeof(path), "/proc/%ld/pagemap", (long)pid);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return ADDR_TRANSLATE_ERR_INVALID_ARG;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return ADDR_TRANSLATE_ERR_OPEN;
    }

    ctx->pid = pid;
    ctx->pagemap_fd = fd;
    ctx->page_size = page_size;

    return ADDR_TRANSLATE_OK;
}

void addr_translate_close(struct addr_translate_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->pagemap_fd >= 0) {
        (void)close(ctx->pagemap_fd);
    }

    addr_translate_reset(ctx);
}

static uint64_t decode_u64_le(const unsigned char bytes[8])
{
    return ((uint64_t)bytes[0]) |
           ((uint64_t)bytes[1] << 8U) |
           ((uint64_t)bytes[2] << 16U) |
           ((uint64_t)bytes[3] << 24U) |
           ((uint64_t)bytes[4] << 32U) |
           ((uint64_t)bytes[5] << 40U) |
           ((uint64_t)bytes[6] << 48U) |
           ((uint64_t)bytes[7] << 56U);
}

static int read_pagemap_entry(const struct addr_translate_ctx *ctx, uint64_t page_index,uint64_t *entry_out)
{
    unsigned char bytes[8];
    uint64_t offset;
    ssize_t nread;

    if (ctx == NULL || entry_out == NULL || ctx->pagemap_fd < 0) {
        return ADDR_TRANSLATE_ERR_INVALID_ARG;
    }

    if (page_index > UINT64_MAX / PAGEMAP_ENTRY_BYTES) {
        return ADDR_TRANSLATE_ERR_OVERFLOW;
    }

    offset = page_index * PAGEMAP_ENTRY_BYTES;

    if (offset > (uint64_t)LLONG_MAX) {
        return ADDR_TRANSLATE_ERR_OVERFLOW;
    }

    nread = pread(ctx->pagemap_fd,
                  bytes,
                  sizeof(bytes),
                  (off_t)offset);

    if (nread != (ssize_t)sizeof(bytes)) {
        return ADDR_TRANSLATE_ERR_READ;
    }

    *entry_out = decode_u64_le(bytes);
    return ADDR_TRANSLATE_OK;
}

int addr_translate_va_to_pa(struct addr_translate_ctx *ctx,uint64_t virtual_address, struct addr_translation *out)
{
    uint64_t page_index;
    uint64_t page_offset;
    uint64_t entry;
    uint64_t pfn;
    int rc;

    if (ctx == NULL || out == NULL || ctx->pid <= 0 || ctx->pagemap_fd < 0 || ctx->page_size == 0U) {
        return ADDR_TRANSLATE_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    page_index = virtual_address / ctx->page_size;
    page_offset = virtual_address % ctx->page_size;

    rc = read_pagemap_entry(ctx, page_index, &entry);
    if (rc != ADDR_TRANSLATE_OK) {
        return rc;
    }

    out->pid = ctx->pid;
    out->virtual_address = virtual_address;
    out->page_size = ctx->page_size;
    out->page_index = page_index;
    out->page_offset = page_offset;
    out->pagemap_entry = entry;

    out->present = (entry & PAGEMAP_PRESENT_BIT) != 0U;
    out->swapped = (entry & PAGEMAP_SWAPPED_BIT) != 0U;
    out->soft_dirty = (entry & PAGEMAP_SOFT_DIRTY_BIT) != 0U;
    out->exclusively_mapped = (entry & PAGEMAP_EXCLUSIVE_BIT) != 0U;
    out->uffd_wp = (entry & PAGEMAP_UFFD_WP_BIT) != 0U;
    out->guard_region = (entry & PAGEMAP_GUARD_BIT) != 0U;
    out->file_or_shared_anon = (entry & PAGEMAP_FILE_SHARED_BIT) != 0U;

    if (!out->present) {
        if (out->swapped) {
            return ADDR_TRANSLATE_ERR_SWAPPED;
        }

        return ADDR_TRANSLATE_ERR_NOT_PRESENT;
    }

    pfn = entry & PAGEMAP_PFN_MASK;
    out->pfn = pfn;

    /*
     * Since Linux 4.2, PFN can be zeroed for callers without CAP_SYS_ADMIN.
     * Also, real zero-page mappings can have PFN 0. For this project, PFN 0
     * is not useful for DRAM bank classification, so fail closed.
     */
    if (pfn == 0U) {
        return ADDR_TRANSLATE_ERR_PFN_UNAVAILABLE;
    }

    if (pfn > (UINT64_MAX - page_offset) / ctx->page_size) {
        return ADDR_TRANSLATE_ERR_OVERFLOW;
    }

    out->physical_address = pfn * ctx->page_size + page_offset;

    return ADDR_TRANSLATE_OK;
}

int addr_translate_pid_va_to_pa(pid_t pid, uint64_t virtual_address, struct addr_translation *out)
{
    struct addr_translate_ctx ctx;
    int rc;

    addr_translate_reset(&ctx);

    rc = addr_translate_open(&ctx, pid);
    if (rc != ADDR_TRANSLATE_OK) {
        return rc;
    }

    rc = addr_translate_va_to_pa(&ctx, virtual_address, out);

    addr_translate_close(&ctx);
    return rc;
}

const char *addr_translate_strerror(int rc)
{
    switch (rc) {
    case ADDR_TRANSLATE_OK:
        return "success";
    case ADDR_TRANSLATE_ERR_INVALID_ARG:
        return "invalid argument";
    case ADDR_TRANSLATE_ERR_PAGE_SIZE:
        return "failed to determine page size";
    case ADDR_TRANSLATE_ERR_OPEN:
        return "failed to open /proc/[pid]/pagemap";
    case ADDR_TRANSLATE_ERR_READ:
        return "failed to read pagemap entry";
    case ADDR_TRANSLATE_ERR_NOT_PRESENT:
        return "page is not present";
    case ADDR_TRANSLATE_ERR_SWAPPED:
        return "page is swapped out";
    case ADDR_TRANSLATE_ERR_PFN_UNAVAILABLE:
        return "PFN unavailable or zero page";
    case ADDR_TRANSLATE_ERR_OVERFLOW:
        return "address calculation overflow";
    default:
        return "unknown address-translation error";
    }
}
