/* Pipeline persistence helpers for the Vulkan backend (RPCS3 rsx_cache.h
 * shaders_cache): a 64-bit content key per pipeline, a key -> u64 map, and the
 * on-disk record of which (pass class, topology) variants each pipeline was
 * linked for, so a later run can link exactly those before the first draw.
 * Plain C, no Vulkan: unit-tested in tests/test_vk_pipestore.c. */
#ifndef RSX_VK_PIPESTORE_H
#define RSX_VK_PIPESTORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKPS_FNV_BASIS   14695981039346656037ull   /* FNV-1a 64 offset basis 0xcbf29ce484222325 */
#define VKPS_TOPOLOGIES  5u
/* pass classes x topologies must fit the u64 variant mask */
#define VKPS_MAX_PC      12u

#define VKPS_FILE_MAGIC   0x56505356u   /* "VSPV" */
/* 2: keys use the canonical FNV basis (v1 files used a truncated one)
 * 3: decompiler/key changes (1D texture masks, alpha-ref format, bx2 lanes,
 *    VP start offset): drop variant masks recorded by older builds */
#define VKPS_FILE_VERSION 3u

uint64_t vkps_fnv(uint64_t h, const void* p, size_t n);

/* Bit index of variant (pass class pc, topology t) in a u64 mask; 64 when out of range. */
static inline uint32_t vkps_variant_bit(uint32_t pc, uint32_t t)
{
    return (pc < VKPS_MAX_PC && t < VKPS_TOPOLOGIES) ? pc * VKPS_TOPOLOGIES + t : 64u;
}

/* Open-addressing map; key 0 is the empty marker (vkps_key never returns it). */
typedef struct { uint64_t key, val; } vkps_ent;
typedef struct { vkps_ent* e; uint32_t cap, n; } vkps_map;

static inline uint64_t vkps_key(uint64_t h) { return h ? h : 1u; }
uint64_t* vkps_map_get(const vkps_map* m, uint64_t key);    /* NULL when absent */
uint64_t* vkps_map_put(vkps_map* m, uint64_t key);          /* existing or new (val 0); NULL on OOM */
void      vkps_map_free(vkps_map* m);

/* Variant file: header {magic, version}, then {u64 key, u64 bits} records,
 * OR-ed into `m` on load. Returns the record count, or -1 when the file is
 * missing, foreign or truncated mid-record (the caller starts it again;
 * records read before the damage stay in `m`). */
int   vkps_variants_load(const char* path, vkps_map* m);
/* `valid`: append to the existing file, else create it with a header. */
FILE* vkps_variants_open(const char* path, int valid);
int   vkps_variants_append(FILE* f, uint64_t key, uint64_t bits);

#ifdef __cplusplus
}
#endif

#endif
