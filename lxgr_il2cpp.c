#include "lxgr_il2cpp.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef LXGR_IL2CPP_INDEX
#include "il2cpp_index.h"
#endif

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* binary search over a sorted (hash, rva) array */
static uint64_t lookup(const uint64_t *hash, const uint32_t *rva, int n,
                       const char *name)
{
    uint64_t h = fnv1a64(name);
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint64_t k = hash[mid];
        if (k < h)
            lo = mid + 1;
        else if (k > h)
            hi = mid - 1;
        else
            return rva[mid];
    }
    return 0;
}

#ifdef LXGR_IL2CPP_INDEX
uint64_t lxgr_il2cpp_method(uint64_t base, const char *name)
{
    uint64_t r = lookup(lxgr_il2cpp_index_methods_hash,
                        lxgr_il2cpp_index_methods_rva,
                        LXGR_IL2CPP_INDEX_METHODS, name);
    return r ? base + r : 0;
}

uint64_t lxgr_il2cpp_field(uint64_t base, const char *name)
{
    uint64_t r = lookup(lxgr_il2cpp_index_fields_hash,
                        lxgr_il2cpp_index_fields_rva,
                        LXGR_IL2CPP_INDEX_FIELDS, name);
    return r ? base + r : 0;
}
#else
uint64_t lxgr_il2cpp_method(uint64_t base, const char *name)
{
    (void)base;
    (void)name;
    return 0;
}
uint64_t lxgr_il2cpp_field(uint64_t base, const char *name)
{
    (void)base;
    (void)name;
    return 0;
}
#endif

/* ---- file-backed index ("LX2I" | u32 methods | u32 fields | entries) ---- */

struct index_blob {
    uint64_t *hash;
    uint32_t *rva;
    int n;
};

static int load_block(const unsigned char *p, long count, struct index_blob *b)
{
    long bytes = count * 12;
    if (count > 0 && bytes > 0 && p) {
        b->hash = (uint64_t *)(void *)p;
        b->rva = (uint32_t *)(void *)(p + count * 8);
        b->n = (int)count;
        return 0;
    }
    b->hash = NULL;
    b->rva = NULL;
    b->n = 0;
    return -1;
}

static int ensure_loaded(const char *path, struct index_blob *methods,
                         struct index_blob *fields)
{
    static struct index_blob sm, sf;
    static const char *cached = NULL;

    if (cached && strcmp(cached, path) == 0) {
        *methods = sm;
        *fields = sf;
        return 0;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 8) {
        close(fd);
        return -1;
    }
    unsigned char *m = (unsigned char *)mmap(NULL, (size_t)st.st_size,
                                             PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return -1;

    const unsigned char *p = m;
    if (memcmp(p, "LX2I", 4) != 0)
        return -1;
    p += 4;
    unsigned int nm = (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
                      ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
    unsigned int nf = (unsigned int)p[4] | ((unsigned int)p[5] << 8) |
                      ((unsigned int)p[6] << 16) | ((unsigned int)p[7] << 24);
    p += 8;

    if (load_block(p, (long)nm, &sm) != 0)
        return -1;
    p += (long)nm * 12;
    if (load_block(p, (long)nf, &sf) != 0)
        return -1;

    cached = path;
    *methods = sm;
    *fields = sf;
    return 0;
}

uint64_t lxgr_il2cpp_method_file(const char *index_path, uint64_t base,
                                 const char *name)
{
    struct index_blob m, f;
    if (!index_path || !name || ensure_loaded(index_path, &m, &f) != 0)
        return 0;
    uint64_t r = lookup(m.hash, m.rva, m.n, name);
    return r ? base + r : 0;
}

uint64_t lxgr_il2cpp_field_file(const char *index_path, uint64_t base,
                                const char *name)
{
    struct index_blob m, f;
    if (!index_path || !name || ensure_loaded(index_path, &m, &f) != 0)
        return 0;
    uint64_t r = lookup(f.hash, f.rva, f.n, name);
    return r ? base + r : 0;
}
