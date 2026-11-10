#include "rsx_vk_pipestore.h"

#include <stdlib.h>
#include <string.h>

uint64_t vkps_fnv(uint64_t h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint32_t vkps_slot(uint64_t key, uint32_t cap)
{
    return (uint32_t)((key ^ (key >> 29)) * 0x9E3779B97F4A7C15ull >> 32) & (cap - 1u);
}

uint64_t* vkps_map_get(const vkps_map* m, uint64_t key)
{
    if (!m->e || !key) return NULL;
    for (uint32_t i = vkps_slot(key, m->cap);; i = (i + 1u) & (m->cap - 1u)) {
        if (m->e[i].key == key) return &m->e[i].val;
        if (!m->e[i].key) return NULL;
    }
}

static int vkps_grow(vkps_map* m)
{
    const uint32_t cap = m->cap ? m->cap * 2u : 256u;
    vkps_ent* e = (vkps_ent*)calloc(cap, sizeof *e);
    if (!e) return -1;
    for (uint32_t i = 0; i < m->cap; i++) {
        if (!m->e[i].key) continue;
        uint32_t j = vkps_slot(m->e[i].key, cap);
        while (e[j].key) j = (j + 1u) & (cap - 1u);
        e[j] = m->e[i];
    }
    free(m->e);
    m->e = e;
    m->cap = cap;
    return 0;
}

uint64_t* vkps_map_put(vkps_map* m, uint64_t key)
{
    if (!key) return NULL;
    uint64_t* v = vkps_map_get(m, key);
    if (v) return v;
    if ((m->n + 1u) * 2u > m->cap && vkps_grow(m)) return NULL;   /* load <= 1/2 */
    uint32_t i = vkps_slot(key, m->cap);
    while (m->e[i].key) i = (i + 1u) & (m->cap - 1u);
    m->e[i].key = key;
    m->e[i].val = 0;
    m->n++;
    return &m->e[i].val;
}

void vkps_map_free(vkps_map* m)
{
    free(m->e);
    memset(m, 0, sizeof *m);
}

int vkps_variants_load(const char* path, vkps_map* m)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t hdr[2];
    int n = -1;
    if (fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == VKPS_FILE_MAGIC && hdr[1] == VKPS_FILE_VERSION) {
        n = 0;
        for (;;) {
            uint64_t r[2];
            const size_t got = fread(r, 1, sizeof r, f);
            if (got == 0) break;                                 /* clean end */
            if (got != sizeof r || !r[0]) { n = -1; break; }
            uint64_t* v = vkps_map_put(m, r[0]);
            if (!v) { n = -1; break; }
            *v |= r[1];
            n++;
        }
    }
    fclose(f);
    return n;
}

FILE* vkps_variants_open(const char* path, int valid)
{
    FILE* f = fopen(path, valid ? "ab" : "wb");
    if (f && !valid) {
        const uint32_t hdr[2] = { VKPS_FILE_MAGIC, VKPS_FILE_VERSION };
        if (fwrite(hdr, sizeof hdr, 1, f) != 1) { fclose(f); return NULL; }
        fflush(f);
    }
    return f;
}

int vkps_variants_append(FILE* f, uint64_t key, uint64_t bits)
{
    if (!f || !key) return -1;
    const uint64_t r[2] = { key, bits };
    if (fwrite(r, sizeof r, 1, f) != 1) return -1;
    fflush(f);
    return 0;
}
