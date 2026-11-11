// Unit test for the round-8 Vulkan renderer upload memos (third_party/rpcs3/rpcs3/Emu/RSX/port_r2memo.h):
// index upload dedup (R2_IDX_DEDUP), transform-constant memo (R2_XFORM_MEMO), exact decode-parameter packing,
// and the env switch parsing. Every reuse must be impossible unless the upstream path would write identical bytes.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/port_r2memo.h"

#include <cstdio>
#include <cstdlib>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

using r2memo::index_dedup;

static void test_index_dedup()
{
	static index_dedup t; // large: keep off the stack
	unsigned short src[64];
	for (int i = 0; i < 64; ++i) src[i] = static_cast<unsigned short>(i * 3);
	const uint64_t p = r2memo::index_params(1, 5, false, false, 0);
	const r2memo::index_result r{ 0x1200, 3, 189, 64 };
	r2memo::index_result out{};

	CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::miss);
	t.store(src, sizeof(src), p, 128, 7, r);
	CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::hit);
	CHECK(out.offset == 0x1200 && out.min_index == 3 && out.max_index == 189 && out.index_count == 64);

	// any key difference is a miss
	CHECK(t.lookup(src, sizeof(src), p, 128, 8, out) == index_dedup::result::miss);                 // heap epoch moved
	CHECK(t.lookup(src, sizeof(src), p, 256, 7, out) == index_dedup::result::miss);                 // different upload size
	CHECK(t.lookup(src, sizeof(src) - 2, p, 128, 7, out) == index_dedup::result::miss);             // different source range
	CHECK(t.lookup(src + 1, sizeof(src) - 2, p, 128, 7, out) == index_dedup::result::miss);         // different source start
	CHECK(t.lookup(src, sizeof(src), r2memo::index_params(1, 6, false, false, 0), 128, 7, out) == index_dedup::result::miss);

	// same key, changed guest bytes: stale (re-upload), then the refreshed entry hits again
	src[17] ^= 1;
	CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::stale);
	t.store(src, sizeof(src), p, 128, 7, { 0x2400, 3, 189, 64 });
	CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::hit && out.offset == 0x2400);

	// size limits: empty and oversized ranges are never cached
	static unsigned char big[index_dedup::max_bytes + 4];
	t.store(big, sizeof(big), p, sizeof(big), 7, r);
	CHECK(t.lookup(big, sizeof(big), p, sizeof(big), 7, out) == index_dedup::result::miss);
	t.store(src, 0, p, 0, 7, r);
	CHECK(t.lookup(src, 0, p, 0, 7, out) == index_dedup::result::miss);
	t.store(big, index_dedup::max_bytes, p, index_dedup::max_bytes, 7, r);
	CHECK(t.lookup(big, index_dedup::max_bytes, p, index_dedup::max_bytes, 7, out) == index_dedup::result::hit);

	// a colliding store evicts: the evicted key must miss, never return the other entry's result
	unsigned short other[64] = {};
	uint32_t n = 0;
	const uint32_t want = index_dedup::slot_of(src, sizeof(src), p);
	const unsigned char* base = reinterpret_cast<const unsigned char*>(big);
	const unsigned char* coll = nullptr;
	for (uint32_t off = 0; off + sizeof(other) <= sizeof(big) && !coll; off += 2, ++n)
		if (base + off != reinterpret_cast<const unsigned char*>(src) && index_dedup::slot_of(base + off, sizeof(other), p) == want) coll = base + off;
	CHECK(coll != nullptr);
	if (coll)
	{
		t.store(coll, sizeof(other), p, 128, 7, { 0x9900, 0, 0, 64 });
		CHECK(t.lookup(src, sizeof(src), p, 128, 7, out) == index_dedup::result::miss);
		CHECK(t.lookup(coll, sizeof(other), p, 128, 7, out) == index_dedup::result::hit && out.offset == 0x9900);
	}
}

static void test_index_params()
{
	std::set<uint64_t> seen;
	for (uint32_t type = 0; type < 2; ++type)
		for (uint32_t prim = 0; prim < 11; ++prim)
			for (int re = 0; re < 2; ++re)
				for (int em = 0; em < 2; ++em)
					for (uint32_t ri : { 0u, 0xFFFFu, 0xFFFFFFFFu })
					{
						const uint64_t k = r2memo::index_params(type, prim, re != 0, em != 0, ri);
						if (re) CHECK(seen.insert(k).second); // every distinct parameter set packs distinctly
						else seen.insert(k);
					}
	// with restart disabled the restart index does not change the upload, so it must not split the key
	CHECK(r2memo::index_params(1, 5, false, false, 0) == r2memo::index_params(1, 5, false, false, 0xFFFF));
	CHECK(r2memo::index_params(1, 5, true, false, 0) != r2memo::index_params(1, 5, true, false, 0xFFFF));
	CHECK(r2memo::index_params(1, 5, true, false, 0xFFFF) != r2memo::index_params(1, 5, true, true, 0xFFFF));
}

static void test_xform_memo()
{
	r2memo::xform_memo m;
	int prog_a = 0, prog_b = 0;
	CHECK(!m.hit(&prog_a, 1, 1));
	CHECK(!m.hit(nullptr, 0, 0)); // empty memo never hits, even for a null key
	m.store(&prog_a, 5, 9);
	CHECK(m.hit(&prog_a, 5, 9));
	CHECK(!m.hit(&prog_b, 5, 9));  // program changed
	CHECK(!m.hit(&prog_a, 6, 9));  // constants written
	CHECK(!m.hit(&prog_a, 5, 10)); // heap epoch / frame moved
	m.clear();
	CHECK(!m.hit(&prog_a, 5, 9));

	const uint64_t g = r2memo::g_xform_gen, e = r2memo::g_heap_epoch;
	r2memo::bump_xform_gen();
	r2memo::bump_heap_epoch();
	CHECK(r2memo::g_xform_gen == g + 1 && r2memo::g_heap_epoch == e + 1);
}

static void test_env_switch()
{
	_putenv("R2MEMO_TEST_SW=");
	CHECK(r2memo::env_switch("R2MEMO_TEST_SW", true) == true);
	CHECK(r2memo::env_switch("R2MEMO_TEST_SW", false) == false);
	_putenv("R2MEMO_TEST_SW=0");
	CHECK(r2memo::env_switch("R2MEMO_TEST_SW", true) == false);
	_putenv("R2MEMO_TEST_SW=1");
	CHECK(r2memo::env_switch("R2MEMO_TEST_SW", false) == true);
}

static void test_pipe_cache_header()
{
	uint8_t uuid[16];
	for (int i = 0; i < 16; ++i) uuid[i] = static_cast<uint8_t>(0xA0 + i);
	std::vector<uint8_t> blob(64, 0x55);
	const uint32_t h[4] = { 32, 1, 0x10DE, 0x2684 };
	std::memcpy(blob.data(), h, sizeof(h));
	std::memcpy(blob.data() + 16, uuid, 16);
	CHECK(r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2684, uuid));
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), 31, 0x10DE, 0x2684, uuid));      // truncated
	CHECK(!r2memo::pipe_cache_header_ok(nullptr, 0, 0x10DE, 0x2684, uuid));
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x1002, 0x2684, uuid)); // other vendor
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2204, uuid)); // other device
	uint8_t uuid2[16];
	std::memcpy(uuid2, uuid, 16);
	uuid2[15] ^= 1;
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2684, uuid2)); // other driver build
	blob[4] = 2;
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2684, uuid)); // header version
	blob[4] = 1;
	blob[0] = 16;
	CHECK(!r2memo::pipe_cache_header_ok(blob.data(), blob.size(), 0x10DE, 0x2684, uuid)); // header size too small
}

int main()
{
	test_index_dedup();
	test_index_params();
	test_xform_memo();
	test_env_switch();
	test_pipe_cache_header();
	std::printf("%s\n", g_fail ? "test_r2memo: FAIL" : "test_r2memo: PASS");
	return g_fail;
}
