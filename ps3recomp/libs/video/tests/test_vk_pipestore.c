/*
 * ps3recomp - tests for rsx_vk_pipestore (the Vulkan backend's persisted
 * pipeline variants, RPCS3 shaders_cache equivalent).
 *
 *   cc -I libs/video -o t libs/video/tests/test_vk_pipestore.c libs/video/rsx_vk_pipestore.c && ./t
 */
#include "rsx_vk_pipestore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
                        g_fail++; } } while (0)

static void test_variant_bits(void)
{
    /* Every pass class x topology has its own bit in the u64 mask. */
    CHECK(vkps_variant_bit(0, 0) == 0);
    CHECK(vkps_variant_bit(0, 4) == 4);
    CHECK(vkps_variant_bit(1, 0) == 5);
    CHECK(vkps_variant_bit(VKPS_MAX_PC - 1u, VKPS_TOPOLOGIES - 1u) == 59);
    CHECK(vkps_variant_bit(VKPS_MAX_PC, 0) == 64);
    CHECK(vkps_variant_bit(0, VKPS_TOPOLOGIES) == 64);
}

static void test_map(void)
{
    vkps_map m;
    memset(&m, 0, sizeof m);
    CHECK(vkps_map_get(&m, 5) == NULL);
    CHECK(vkps_map_put(&m, 0) == NULL);          /* 0 is the empty marker */
    CHECK(vkps_key(0) == 1);
    /* Through several growths, every key keeps its value. */
    for (unsigned long long k = 1; k <= 5000; k++) {
        unsigned long long* v = vkps_map_put(&m, k * 0x9E3779B97F4A7C15ull);
        CHECK(v && *v == 0);
        if (v) *v = k;
    }
    CHECK(m.n == 5000);
    int bad = 0;
    for (unsigned long long k = 1; k <= 5000; k++) {
        unsigned long long* v = vkps_map_get(&m, k * 0x9E3779B97F4A7C15ull);
        if (!v || *v != k) bad++;
    }
    CHECK(bad == 0);
    /* put of an existing key returns its slot, no duplicate */
    unsigned long long* v = vkps_map_put(&m, 3 * 0x9E3779B97F4A7C15ull);
    CHECK(v && *v == 3 && m.n == 5000);
    vkps_map_free(&m);
    CHECK(m.e == NULL && m.n == 0);
}

static void test_fnv(void)
{
    /* Stable across runs (the file key), and content-sensitive. */
    CHECK(VKPS_FNV_BASIS == 0xcbf29ce484222325ull);
    CHECK(vkps_fnv(VKPS_FNV_BASIS, "", 0) == VKPS_FNV_BASIS);
    CHECK(vkps_fnv(VKPS_FNV_BASIS, "a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(vkps_fnv(VKPS_FNV_BASIS, "ab", 2) != vkps_fnv(VKPS_FNV_BASIS, "ba", 2));
}

static void test_file_roundtrip(void)
{
    const char* path = "test_vk_pipestore.bin";
    remove(path);
    vkps_map m;
    memset(&m, 0, sizeof m);
    CHECK(vkps_variants_load(path, &m) == -1);         /* missing: start one */

    FILE* f = vkps_variants_open(path, 0);
    CHECK(f != NULL);
    CHECK(vkps_variants_append(f, 0x1111, 1ull << vkps_variant_bit(0, 3)) == 0);
    CHECK(vkps_variants_append(f, 0x2222, 1ull << vkps_variant_bit(3, 4)) == 0);
    CHECK(vkps_variants_append(f, 0x1111, 1ull << vkps_variant_bit(4, 3)) == 0);
    CHECK(vkps_variants_append(f, 0, 1) == -1);        /* no empty-marker records */
    fclose(f);

    /* Records of one key OR together. */
    CHECK(vkps_variants_load(path, &m) == 3);
    unsigned long long* a = vkps_map_get(&m, 0x1111);
    unsigned long long* b = vkps_map_get(&m, 0x2222);
    CHECK(a && *a == ((1ull << 3) | (1ull << 23)));
    CHECK(b && *b == (1ull << 19));
    vkps_map_free(&m);

    /* Appending to a valid file keeps what was there. */
    f = vkps_variants_open(path, 1);
    CHECK(f && vkps_variants_append(f, 0x3333, 2) == 0);
    if (f) fclose(f);
    CHECK(vkps_variants_load(path, &m) == 4);
    CHECK(vkps_map_get(&m, 0x3333) && *vkps_map_get(&m, 0x3333) == 2);
    vkps_map_free(&m);

    /* A record cut short (process killed mid-write): invalid, earlier records kept. */
    f = fopen(path, "ab");
    fwrite("xxxx", 1, 4, f);
    fclose(f);
    CHECK(vkps_variants_load(path, &m) == -1);
    CHECK(vkps_map_get(&m, 0x2222) != NULL);
    vkps_map_free(&m);

    /* A foreign file is not read. */
    f = fopen(path, "wb");
    const unsigned int junk[4] = { 1, 2, 3, 4 };
    fwrite(junk, sizeof junk, 1, f);
    fclose(f);
    CHECK(vkps_variants_load(path, &m) == -1);
    CHECK(m.n == 0);
    vkps_map_free(&m);
    remove(path);
}

int main(void)
{
    test_variant_bits();
    test_map();
    test_fnv();
    test_file_roundtrip();
    if (g_fail) { printf("%d FAILED\n", g_fail); return 1; }
    printf("ok\n");
    return 0;
}
