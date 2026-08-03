/* lxgr_dump CLI — standalone PA/VA memory dumper (builds alone or as part of
 * any project; lxgr_dump.c is the reusable library, this is the CLI). */

#include "lxgr_dump.h"
#include "lxgr_il2cpp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct seg {
    uint64_t start, end, off;
    char perm[5];
};

static const char *find_arg(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return NULL;
}

static int has_flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return 1;
    return 0;
}

static int parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s)
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || !end || *end)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int dump_lib(int pid, const char *name, const char *outpath, int pamap)
{
    char mpath[64];
    snprintf(mpath, sizeof(mpath), "/proc/%d/maps", pid);
    FILE *mf = fopen(mpath, "r");
    if (!mf) {
        perror("maps");
        return 1;
    }

    struct seg segs[512];
    int nsegs = 0;
    uint64_t total = 0;
    char line[1024];
    while (fgets(line, sizeof(line), mf)) {
        if (!strstr(line, name))
            continue;
        unsigned long long start, end, off;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &off) != 4)
            continue;
        if (!strchr(perms, 'r'))
            continue;
        if (nsegs >= (int)(sizeof(segs) / sizeof(segs[0])))
            break;
        segs[nsegs].start = start;
        segs[nsegs].end = end;
        segs[nsegs].off = off;
        snprintf(segs[nsegs].perm, sizeof(segs[nsegs].perm), "%s", perms);
        uint64_t sz = end - start;
        if (off + sz > total)
            total = off + sz;
        nsegs++;
    }
    fclose(mf);

    if (nsegs == 0) {
        fprintf(stderr, "no readable segments for '%s'\n", name);
        return 1;
    }

    int fd = open(outpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("out");
        return 1;
    }
    if (ftruncate(fd, (off_t)total) != 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    long written = 0;
    for (int i = 0; i < nsegs; i++) {
        long w = lxgr_dump_range(pid, segs[i].start,
                                 segs[i].end - segs[i].start, fd, segs[i].off);
        if (w < 0) {
            fprintf(stderr, "dump seg %d (0x%llx) failed\n", i,
                    (unsigned long long)segs[i].start);
            close(fd);
            return 1;
        }
        written += w;
        printf("seg off=0x%llx size=%llu got=%ld\n",
               (unsigned long long)segs[i].off,
               (unsigned long long)(segs[i].end - segs[i].start), w);
    }
    close(fd);
    printf("total=%ld bytes -> %s\n", written, outpath);

    if (pamap) {
        char pampath[1024];
        snprintf(pampath, sizeof(pampath), "%s.pamap", outpath);
        FILE *pf = fopen(pampath, "w");
        if (!pf) {
            perror("pamap");
            return 1;
        }
        fprintf(pf, "# VA PA\n");
        for (int i = 0; i < nsegs; i++)
            lxgr_pamap_range(pid, segs[i].start, segs[i].end - segs[i].start,
                             pf);
        fclose(pf);
        printf("pamap -> %s\n", pampath);
    }
    return 0;
}

static int dump_range(int pid, uint64_t base, uint64_t size, const char *outpath,
                      int pamap)
{
    if (size == 0) {
        fprintf(stderr, "--size must be > 0\n");
        return 1;
    }
    int fd = open(outpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("out");
        return 1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }
    long w = lxgr_dump_range(pid, base, size, fd, 0);
    close(fd);
    if (w < 0) {
        fprintf(stderr, "dump failed\n");
        return 1;
    }
    printf("dumped %ld bytes (0x%llx @ va 0x%llx) -> %s\n", w,
           (unsigned long long)w, (unsigned long long)base, outpath);

    if (pamap) {
        char pampath[1024];
        snprintf(pampath, sizeof(pampath), "%s.pamap", outpath);
        FILE *pf = fopen(pampath, "w");
        if (!pf) {
            perror("pamap");
            return 1;
        }
        fprintf(pf, "# VA PA\n");
        lxgr_pamap_range(pid, base, size, pf);
        fclose(pf);
        printf("pamap -> %s\n", pampath);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  lxgr_dump --pid <n> --lib <name> --out <file> [--pamap]\n"
        "      dump every readable segment of a mapped library (libil2cpp.so,\n"
        "      libgame.so, ...) into <file> at each segment's file offset\n"
        "  lxgr_dump --pid <n> --base <hex> --size <hex> --out <file> [--pamap]\n"
        "      dump a raw virtual range\n"
        "  lxgr_dump --pid <n> --chain <spec> [--size <hex> --out <file>] [--pamap]\n"
        "      resolve a pointer chain (B+0x10>0x8>0x20; B = il2cpp base).\n"
        "      With --size/--out it dumps from the resolved address; alone it\n"
        "      prints the resolved absolute VA + PA.\n"
        "  --il2cpp  shorthand for --lib libil2cpp.so\n");
}

static uint64_t find_il2cpp_base(int pid)
{
    char mpath[64];
    snprintf(mpath, sizeof(mpath), "/proc/%d/maps", pid);
    FILE *mf = fopen(mpath, "r");
    if (!mf)
        return 0;
    uint64_t base = 0;
    char line[1024];
    while (fgets(line, sizeof(line), mf)) {
        if (strstr(line, "libil2cpp.so")) {
            unsigned long long start, end, off;
            char perms[8];
            if (sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &off) == 4 &&
                off == 0) {
                base = (uint64_t)start;
                break;
            }
        }
    }
    fclose(mf);
    return base;
}

static int resolve_name(int pid, const char *name, const char *index,
                        int is_field)
{
    uint64_t base = find_il2cpp_base(pid);
    if (!base) {
        fprintf(stderr, "libil2cpp.so base not found\n");
        return 1;
    }
    uint64_t addr = 0;
    if (index) {
        addr = is_field ? lxgr_il2cpp_field_file(index, base, name)
                        : lxgr_il2cpp_method_file(index, base, name);
    } else {
        addr = is_field ? lxgr_il2cpp_field(base, name)
                        : lxgr_il2cpp_method(base, name);
    }
    if (!addr) {
        fprintf(stderr, "name not in index: %s\n", name);
        return 1;
    }
    uint64_t pa = lxgr_va_to_pa(pid, addr);
    printf("%s '%s' VA = 0x%llx (base 0x%llx + 0x%llx)  PA = 0x%llx\n",
           is_field ? "field" : "method", name, (unsigned long long)addr,
           (unsigned long long)base,
           (unsigned long long)(addr - base), (unsigned long long)pa);
    return 0;
}

int main(int argc, char **argv)
{
    const char *pid_s = find_arg(argc, argv, "--pid");
    if (!pid_s) {
        usage();
        return 1;
    }
    int pid = atoi(pid_s);
    int pamap = has_flag(argc, argv, "--pamap");

    const char *lib = find_arg(argc, argv, "--lib");
    if (!lib && has_flag(argc, argv, "--il2cpp"))
        lib = "libil2cpp.so";

    const char *chain = find_arg(argc, argv, "--chain");
    if (chain) {
        uint64_t il2cpp_base = 0;
        /* discover il2cpp base from maps so "B" works out of the box */
        char mpath[64];
        snprintf(mpath, sizeof(mpath), "/proc/%d/maps", pid);
        FILE *mf = fopen(mpath, "r");
        if (mf) {
            char line[1024];
            while (fgets(line, sizeof(line), mf)) {
                if (strstr(line, "libil2cpp.so")) {
                    unsigned long long start, end, off;
                    char perms[8];
                    if (sscanf(line, "%llx-%llx %7s %llx", &start, &end,
                               perms, &off) == 4 && off == 0) {
                        il2cpp_base = (uint64_t)start;
                        break;
                    }
                }
            }
            fclose(mf);
        }
        uint64_t va = lxgr_resolve_chain(pid, il2cpp_base, chain);
        if (va == 0) {
            fprintf(stderr, "chain resolve failed (il2cpp base 0x%llx)\n",
                    (unsigned long long)il2cpp_base);
            return 1;
        }
        uint64_t pa = lxgr_va_to_pa(pid, va);
        printf("resolved VA = 0x%llx  PA = 0x%llx%s\n",
               (unsigned long long)va, (unsigned long long)pa,
               pa ? "" : " (not present)");
        const char *out = find_arg(argc, argv, "--out");
        const char *size_s = find_arg(argc, argv, "--size");
        if (out && size_s) {
            uint64_t size;
            if (parse_u64(size_s, &size) != 0)
                return 1;
            return dump_range(pid, va, size, out, pamap);
        }
        return 0;
    }

    if (lib) {
        const char *out = find_arg(argc, argv, "--out");
        if (!out) {
            usage();
            return 1;
        }
        return dump_lib(pid, lib, out, pamap);
    }

    const char *v2p = find_arg(argc, argv, "--v2p");
    if (v2p) {
        uint64_t va, pa;
        if (parse_u64(v2p, &va) != 0)
            return 1;
        pa = lxgr_va_to_pa(pid, va);
        printf("VA 0x%llx -> PA 0x%llx%s\n", (unsigned long long)va,
               (unsigned long long)pa, pa ? "" : " (not present)");
        return 0;
    }
    const char *p2v = find_arg(argc, argv, "--p2v");
    if (p2v) {
        uint64_t pa, va;
        if (parse_u64(p2v, &pa) != 0)
            return 1;
        va = lxgr_pa_to_va(pid, pa);
        printf("PA 0x%llx -> VA 0x%llx%s\n", (unsigned long long)pa,
               (unsigned long long)va, va ? "" : " (not found)");
        return 0;
    }

    const char *base_s = find_arg(argc, argv, "--base");
    const char *size_s = find_arg(argc, argv, "--size");
    const char *out = find_arg(argc, argv, "--out");
    if (base_s && size_s && out) {
        uint64_t base, size;
        if (parse_u64(base_s, &base) != 0 || parse_u64(size_s, &size) != 0)
            return 1;
        return dump_range(pid, base, size, out, pamap);
    }

    const char *ir = find_arg(argc, argv, "--il2cpp-resolve");
    if (ir)
        return resolve_name(pid, ir, find_arg(argc, argv, "--index"), 0);
    const char *if_ = find_arg(argc, argv, "--il2cpp-field");
    if (if_)
        return resolve_name(pid, if_, find_arg(argc, argv, "--index"), 1);

    usage();
    return 1;
}
