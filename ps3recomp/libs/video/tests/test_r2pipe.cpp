// Unit test for the pipeline-stutter helpers (third_party/rpcs3/rpcs3/Emu/RSX/port_r2pipe.h):
// [pipe] stall window + histogram, report timer, GPL part-cache byte keys, first-use preload order and boot split,
// the two-level compiler queue, and the R2_FOSSILIZE mode parser.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/port_r2pipe.h"

#include <cstdio>
#include <unordered_map>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static void test_buckets()
{
	CHECK(r2pipe::stall_bucket(0.0) == -1);
	CHECK(r2pipe::stall_bucket(1.999) == -1);
	CHECK(r2pipe::stall_bucket(2.0) == 0);
	CHECK(r2pipe::stall_bucket(3.9) == 0);
	CHECK(r2pipe::stall_bucket(4.0) == 1);
	CHECK(r2pipe::stall_bucket(8.0) == 2);
	CHECK(r2pipe::stall_bucket(16.0) == 3);
	CHECK(r2pipe::stall_bucket(32.9) == 3);
	CHECK(r2pipe::stall_bucket(33.0) == 4);
	CHECK(r2pipe::stall_bucket(500.0) == 4);
}

static void test_window()
{
	r2pipe::stall_window w;
	w.add_stall(0.5);
	w.add_stall(3.0);
	w.end_frame();              // frame 1: 3.5 ms
	w.end_frame();              // frame 2: none
	w.add_stall(40.0);
	w.add_stall(-1.0);          // clock glitch clamps to 0
	w.end_frame();              // frame 3: 40 ms
	CHECK(w.frames == 3);
	CHECK(w.stalls == 4);
	CHECK(w.total_ms == 43.5);
	CHECK(w.worst_frame_ms == 40.0);
	CHECK(w.worst_stall_ms == 40.0);
	CHECK(w.frames_with_stall == 2);
	CHECK(w.hist[0] == 1 && w.hist[1] == 0 && w.hist[2] == 0 && w.hist[3] == 0 && w.hist[4] == 1);
	CHECK(w.per_frame_ms() == 14.5);

	w.add_stall(5.0);           // unfinished frame carries into the next window
	w.reset();
	CHECK(w.frames == 0 && w.stalls == 0 && w.total_ms == 0.0 && w.hist[1] == 0);
	w.end_frame();
	CHECK(w.worst_frame_ms == 5.0 && w.frames_with_stall == 1);
	CHECK(r2pipe::stall_window{}.per_frame_ms() == 0.0);
}

static void test_timer()
{
	r2pipe::report_timer t;
	CHECK(!t.due(1000));                          // arms
	CHECK(!t.due(1000 + 4'999'999'999ull));
	CHECK(t.due(1000 + 5'000'000'000ull));
	CHECK(!t.due(1000 + 9'000'000'000ull));
	CHECK(t.due(1000 + 10'000'000'001ull));
}

static void test_keys()
{
	r2pipe::byte_key a, b;
	a.add(uint32_t{1}).add(uint64_t{0x1234});
	b.add(uint32_t{1}).add(uint64_t{0x1234});
	CHECK(a == b && a.hash() == b.hash());
	b.add(uint8_t{0});
	CHECK(!(a == b));

	// length-prefixed variable parts: {AB}{C} differs from {A}{BC}
	r2pipe::byte_key c, d;
	c.add_bytes("AB", 2).add_bytes("C", 1);
	d.add_bytes("A", 1).add_bytes("BC", 2);
	CHECK(!(c == d));
	CHECK(c.hash() != d.hash());

	// map lookups compare full bytes, not only the hash
	std::unordered_map<r2pipe::byte_key, int, r2pipe::byte_key_hash> m;
	m[a] = 7;
	r2pipe::byte_key a2;
	a2.add(uint32_t{1}).add(uint64_t{0x1234});
	CHECK(m.count(a2) == 1 && m[a2] == 7);
	CHECK(m.count(c) == 0);

	// FNV-1a reference values
	CHECK(r2pipe::fnv1a64("", 0) == 0xcbf29ce484222325ull);
	CHECK(r2pipe::fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
}

static void test_preload_order()
{
	const std::vector<int64_t> mt = { 50, 10, 30, 10, 20 };
	const auto o = r2pipe::first_use_order(mt);
	CHECK(o.size() == 5);
	CHECK(o[0] == 1 && o[1] == 3 && o[2] == 4 && o[3] == 2 && o[4] == 0); // stable on ties
	CHECK(r2pipe::first_use_order({}).empty());

	CHECK(r2pipe::boot_count(2035, -1) == 2035);
	CHECK(r2pipe::boot_count(2035, 0) == 0);
	CHECK(r2pipe::boot_count(2035, 384) == 384);
	CHECK(r2pipe::boot_count(100, 384) == 100);
}

static void test_queue()
{
	r2pipe::two_level_queue<int> q;
	int v = 0;
	CHECK(!q.pop(v) && q.empty());
	q.push(1, true);
	q.push(2, false);
	q.push(3, true);
	q.push(4, false);
	CHECK(q.size() == 4);
	CHECK(q.pop(v) && v == 2);
	CHECK(q.pop(v) && v == 4);
	q.push(5, false);
	CHECK(q.pop(v) && v == 5);
	CHECK(q.pop(v) && v == 1);
	CHECK(q.pop(v) && v == 3);
	CHECK(!q.pop(v));
	q.push(9, true);
	q.clear();
	CHECK(q.empty());
}

static void test_foz_mode()
{
	using m = r2pipe::foz_mode;
	CHECK(r2pipe::parse_foz_mode(nullptr) == m::auto_replay);
	CHECK(r2pipe::parse_foz_mode("") == m::auto_replay);
	CHECK(r2pipe::parse_foz_mode("auto") == m::auto_replay);
	CHECK(r2pipe::parse_foz_mode("0") == m::off);
	CHECK(r2pipe::parse_foz_mode("OFF") == m::off);
	CHECK(r2pipe::parse_foz_mode("record") == m::record);
	CHECK(r2pipe::parse_foz_mode("Record") == m::record);
	CHECK(r2pipe::parse_foz_mode("replay") == m::replay);
	CHECK(r2pipe::parse_foz_mode("1") == m::replay);
}

static void test_env()
{
	_putenv("R2PIPE_TEST_INT=");
	CHECK(r2pipe::env_int("R2PIPE_TEST_INT", 42) == 42);
	_putenv("R2PIPE_TEST_INT=-1");
	CHECK(r2pipe::env_int("R2PIPE_TEST_INT", 42) == -1);
	_putenv("R2PIPE_TEST_INT=300");
	CHECK(r2pipe::env_int("R2PIPE_TEST_INT", 42) == 300);
	_putenv("R2PIPE_TEST_INT=abc");
	CHECK(r2pipe::env_int("R2PIPE_TEST_INT", 42) == 42);
}

int main()
{
	test_buckets();
	test_window();
	test_timer();
	test_keys();
	test_preload_order();
	test_queue();
	test_foz_mode();
	test_env();
	std::printf("%s\n", g_fail ? "test_r2pipe: FAIL" : "test_r2pipe: PASS");
	return g_fail;
}
