#define _GNU_SOURCE

#include "addr_translate.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    struct addr_translation tr;
    struct addr_translate_ctx at;
    long page_size_long;
    size_t page_size;
    unsigned char *buf;
    int rc;

    page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        fprintf(stderr, "sysconf(_SC_PAGESIZE) failed\n");
        return EXIT_FAILURE;
    }

    page_size = (size_t)page_size_long;

    buf = mmap(NULL,
               page_size,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    /*
     * Force physical allocation. Without touching the page, pagemap may show
     * it as not present.
     */
    memset(buf, 0x5a, page_size);

    addr_translate_reset(&at);

    rc = addr_translate_open(&at, getpid());
    if (rc != ADDR_TRANSLATE_OK) {
        fprintf(stderr,
                "addr_translate_open failed: %s\n",
                addr_translate_strerror(rc));
        (void)munmap(buf, page_size);
        return EXIT_FAILURE;
    }

    rc = addr_translate_va_to_pa(&at,
                                 (uint64_t)(uintptr_t)buf,
                                 &tr);
    addr_translate_close(&at);

    if (rc != ADDR_TRANSLATE_OK) {
        fprintf(stderr,
                "addr_translate_va_to_pa failed: %s\n",
                addr_translate_strerror(rc));
        fprintf(stderr,
                "hint: run with sudo/CAP_SYS_ADMIN and make sure the page "
                "was touched before translation.\n");
        (void)munmap(buf, page_size);
        return EXIT_FAILURE;
    }

    printf("pid              : %ld\n", (long)tr.pid);
    printf("page size        : %" PRIu64 "\n", tr.page_size);
    printf("VA               : 0x%016" PRIx64 "\n",
           tr.virtual_address);
    printf("pagemap entry    : 0x%016" PRIx64 "\n",
           tr.pagemap_entry);
    printf("present          : %s\n", tr.present ? "true" : "false");
    printf("swapped          : %s\n", tr.swapped ? "true" : "false");
    printf("file/shared anon : %s\n",
           tr.file_or_shared_anon ? "true" : "false");
    printf("exclusive        : %s\n",
           tr.exclusively_mapped ? "true" : "false");
    printf("PFN              : 0x%016" PRIx64 "\n", tr.pfn);
    printf("page offset      : 0x%016" PRIx64 "\n", tr.page_offset);
    printf("PA               : 0x%016" PRIx64 "\n",
           tr.physical_address);

    (void)munmap(buf, page_size);
    return EXIT_SUCCESS;
}
