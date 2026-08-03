#define _GNU_SOURCE
#include "lxgr_dump.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef SYS_process_vm_readv
#define SYS_process_vm_readv 270
#endif

#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)
#define PAMAP_MAX_PAGES (1u << 22) /* cap a bounded PA->VA scan */

static long page_size(void)
{
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? ps : 4096;
}

long lxgr_read(int pid, uint64_t va, void *out, size_t size)
{
    char *dst = (char *)out;
    size_t done = 0;

    while (done < size) {
        struct iovec lv = { dst + done, size - done };
        struct iovec rv = { (void *)(uintptr_t)(va + done), size - done };
        ssize_t n = syscall(SYS_process_vm_readv, pid, &lv, 1, &rv, 1, 0);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        break;
    }
    if (done == size)
        return (long)done;

    /* fallback: /proc/<pid>/mem */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return done ? (long)done : -1;
    while (done < size) {
        ssize_t n = pread(fd, dst + done, size - done, (off_t)(va + done));
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        break;
    }
    close(fd);
    return done ? (long)done : -1;
}

uint64_t lxgr_va_to_pa(int pid, uint64_t va)
{
    long ps = page_size();
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    uint64_t ent = 0;
    uint64_t idx = (va / (uint64_t)ps) * sizeof(ent);
    ssize_t n = pread(fd, &ent, sizeof(ent), (off_t)idx);
    close(fd);
    if (n != (ssize_t)sizeof(ent) || !(ent & PAGEMAP_PRESENT))
        return 0;
    uint64_t pfn = ent & PAGEMAP_PFN_MASK;
    return (pfn << 12) | (va & ((uint64_t)ps - 1));
}

uint64_t lxgr_pa_to_va(int pid, uint64_t pa)
{
    long ps = page_size();
    uint64_t want_pfn = (pa >> 12) & PAGEMAP_PFN_MASK;

    char ppath[64];
    snprintf(ppath, sizeof(ppath), "/proc/%d/pagemap", pid);
    int fd = open(ppath, O_RDONLY);
    if (fd < 0)
        return 0;

    char mpath[64];
    snprintf(mpath, sizeof(mpath), "/proc/%d/maps", pid);
    FILE *mf = fopen(mpath, "r");
    if (!mf) {
        close(fd);
        return 0;
    }

    uint64_t found = 0;
    uint64_t scanned = 0;
    char line[512];
    while (fgets(line, sizeof(line), mf)) {
        unsigned long start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) < 2 || end <= start)
            continue;
        uint64_t pg = (uint64_t)start >> 12;
        uint64_t epg = ((uint64_t)end + (uint64_t)ps - 1) >> 12;
        uint64_t ent = 0;
        for (uint64_t i = pg; i < epg; i++) {
            ssize_t n = pread(fd, &ent, sizeof(ent), (off_t)(i * sizeof(ent)));
            if (n == (ssize_t)sizeof(ent) && (ent & PAGEMAP_PRESENT) &&
                (ent & PAGEMAP_PFN_MASK) == want_pfn) {
                found = (i << 12) | (pa & ((uint64_t)ps - 1));
                goto done;
            }
            if (++scanned >= PAMAP_MAX_PAGES)
                goto done;
        }
    }
done:
    fclose(mf);
    close(fd);
    return found;
}

static int parse_hex(const char *s, uint64_t *out)
{
    if (!s || !*s)
        return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 16);
    if (errno || !end || *end)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

uint64_t lxgr_resolve_chain(int pid, uint64_t base, const char *spec)
{
    if (!spec || !*spec)
        return 0;

    char buf[256];
    size_t blen = strlen(spec);
    if (blen >= sizeof(buf))
        return 0;
    memcpy(buf, spec, blen + 1);

    char *save = NULL;
    char *tok = strtok_r(buf, ">", &save);
    if (!tok)
        return 0;

    uint64_t cur;
    if (strcmp(tok, "B") == 0 || strcmp(tok, "b") == 0) {
        cur = base;
    } else if (parse_hex(tok, &cur) != 0) {
        return 0;
    }

    while ((tok = strtok_r(NULL, ">", &save)) != NULL) {
        uint64_t off;
        if (parse_hex(tok, &off) != 0)
            return 0;
        if (cur == 0)
            return 0;
        if (lxgr_read(pid, cur + off, &cur, sizeof(cur)) != (long)sizeof(cur))
            return 0;
    }
    return cur;
}

long lxgr_dump_range(int pid, uint64_t va, uint64_t size, int out_fd,
                     uint64_t file_off)
{
    static const unsigned char zero[65536];
    unsigned char *buf = malloc(65536);
    if (!buf)
        return -1;

    uint64_t done = 0;
    uint64_t pos = file_off;
    long total = 0;

    while (done < size) {
        uint64_t chunk = size - done;
        if (chunk > 65536)
            chunk = 65536;
        size_t want = chunk;
        long got = lxgr_read(pid, va + done, buf, want);
        if (got < 0) {
            memcpy(buf, zero, chunk);
            got = (long)chunk;
        } else if ((uint64_t)got < chunk) {
            memset(buf + got, 0, chunk - (size_t)got);
            got = (long)chunk;
        }

        ssize_t w = pwrite(out_fd, buf, (size_t)got, (off_t)pos);
        if (w != (ssize_t)got) {
            free(buf);
            return -1;
        }
        done += (uint64_t)got;
        pos += (uint64_t)got;
        total += got;
    }
    free(buf);
    return total;
}

int lxgr_pamap_range(int pid, uint64_t va, uint64_t size, FILE *pf)
{
    long ps = page_size();
    char ppath[64];
    snprintf(ppath, sizeof(ppath), "/proc/%d/pagemap", pid);
    int fd = open(ppath, O_RDONLY);
    if (fd < 0)
        return -1;

    uint64_t end = va + size;
    uint64_t pg = va >> 12;
    uint64_t epg = (end + (uint64_t)ps - 1) >> 12;

    for (uint64_t i = pg; i < epg; i++) {
        uint64_t ent = 0;
        ssize_t n = pread(fd, &ent, sizeof(ent), (off_t)(i * sizeof(ent)));
        if (n != (ssize_t)sizeof(ent))
            continue;
        uint64_t vp = i << 12;
        if (ent & PAGEMAP_PRESENT) {
            uint64_t pp = ((ent & PAGEMAP_PFN_MASK) << 12) |
                          (vp & ((uint64_t)ps - 1));
            fprintf(pf, "0x%llx 0x%llx\n", (unsigned long long)vp,
                    (unsigned long long)pp);
        } else {
            fprintf(pf, "0x%llx -\n", (unsigned long long)vp);
        }
    }
    close(fd);
    return 0;
}
