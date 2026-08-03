#ifndef LXGR_DUMP_H
#define LXGR_DUMP_H

/* lxgr_dump.h — public API of the lxgr-dump physical-address dumper.
 *
 * This header + lxgr_dump.c form an independent, standalone memory-dump
 * library/tool. Build it alone (ndk-build) for the `lxgr_dump` CLI, or link
 * lxgr_dump.c into any other program (the LXGR overlay does exactly that).
 *
 * Requires root: cross-process reads need CAP_SYS_PTRACE and pagemap reads
 * need CAP_SYS_ADMIN.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read `size` bytes from pid at virtual address va. Returns bytes read or -1.
 * Uses process_vm_readv first, falls back to /proc/<pid>/mem pread. */
long lxgr_read(int pid, uint64_t va, void *out, size_t size);

/* VA -> PA for a single address (via /proc/<pid>/pagemap). Returns 0 when the
 * page is not present or pid/va is invalid. PA = PFN<<12 | (va & 0xfff). */
uint64_t lxgr_va_to_pa(int pid, uint64_t va);

/* PA -> VA: returns the first virtual address in pid whose page maps the
 * physical page containing `pa`, or 0 when none. Bounded scan. */
uint64_t lxgr_pa_to_va(int pid, uint64_t pa);

/* Resolve a pointer chain to an absolute VA.
 * spec tokens separated by '>'; first token is "B" (use `base`) or a literal
 * hex address. Each following token is a hex offset added before deref.
 *   "B+0x100>0x8>0x20" -> read(base+0x100), then read(ptr+0x8), then +0x20.
 * Returns the final absolute VA, or 0 on parse/read failure. */
uint64_t lxgr_resolve_chain(int pid, uint64_t base, const char *spec);

/* Dump [va, va+size) of pid into the already-open fd at byte offset file_off.
 * Writes the raw image (unreadable pages become zero). Returns bytes written
 * or -1. */
long lxgr_dump_range(int pid, uint64_t va, uint64_t size, int out_fd,
                     uint64_t file_off);

/* Append one "VA PA perm" line per page of [va, va+size) to `pf` (the .pamap).
 * Returns 0 on success, -1 on error. */
int lxgr_pamap_range(int pid, uint64_t va, uint64_t size, FILE *pf);

#ifdef __cplusplus
}
#endif

#endif /* LXGR_DUMP_H */
