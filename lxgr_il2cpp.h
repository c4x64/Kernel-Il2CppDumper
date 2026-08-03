#ifndef LXGR_IL2CPP_H
#define LXGR_IL2CPP_H

/* lxgr_il2cpp.h — resolve il2cpp API names to runtime addresses directly in
 * code, using an index built from an Il2CppDumper script.json.
 *
 * Index keys are the mangled method/field `Name` from script.json (the same
 * names that appear in dump.cs). Runtime address = il2cpp_base + RVA.
 *
 * Two sources:
 *   - embedded: compile with -DLXGR_IL2CPP_INDEX and include the generated
 *     il2cpp_index.h (from il2cpp_index.py). lxgr_il2cpp_method() is O(log n).
 *   - file:     lxgr_il2cpp_method_file() mmaps the generated .bin index.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Embedded lookup (needs -DLXGR_IL2CPP_INDEX + il2cpp_index.h). Returns
 * il2cpp_base + RVA for a mangled method name, or 0 if unknown. */
uint64_t lxgr_il2cpp_method(uint64_t il2cpp_base, const char *name);

/* Embedded field lookup. */
uint64_t lxgr_il2cpp_field(uint64_t il2cpp_base, const char *name);

/* File-backed lookup; loads the .bin index on first use. Returns runtime
 * address (il2cpp_base + RVA) or 0. kind: "method" or "field". */
uint64_t lxgr_il2cpp_method_file(const char *index_path, uint64_t il2cpp_base,
                                 const char *name);
uint64_t lxgr_il2cpp_field_file(const char *index_path, uint64_t il2cpp_base,
                                const char *name);

#ifdef __cplusplus
}
#endif

#endif /* LXGR_IL2CPP_H */
