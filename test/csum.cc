#include "tests.h"

#include "crucible/csum.h"
#include "../src/bees-csum.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

using namespace crucible;
using namespace std;

static
vector<uint8_t>
pattern(size_t len, unsigned mul = 7, unsigned add = 3)
{
	vector<uint8_t> rv(len);
	for (size_t i = 0; i < len; ++i) {
		rv[i] = (i * mul + add) & 0xff;
	}
	return rv;
}

static
void
test_crc32c()
{
	// The CRC-32C check value, and values from a bitwise reference implementation
	assert(Digest::crc32c("123456789", 9) == 0xe3069283);
	assert(Digest::crc32c_sw("123456789", 9) == 0xe3069283);
	assert(Digest::crc32c("", 0) == 0);
	const auto p4096 = pattern(4096);
	assert(Digest::crc32c(p4096.data(), 4096) == 0xed96b643);
	assert(Digest::crc32c(p4096.data(), 4093) == 0x9af1f9ce);
	// btrfs' csum of a block of zeros
	const vector<uint8_t> zero(4096, 0);
	assert(Digest::crc32c(zero.data(), 4096) == 0x98f94189);
	// The SSE 4.2 path and the table agree at every length and alignment
	const auto p = pattern(9000, 131, 17);
	for (size_t off = 0; off < 9; ++off) {
		for (size_t len = 0; len < 300; ++len) {
			assert(Digest::crc32c(p.data() + off, len) == Digest::crc32c_sw(p.data() + off, len));
		}
		assert(Digest::crc32c(p.data() + off, 8000) == Digest::crc32c_sw(p.data() + off, 8000));
	}
}

static
void
test_xxh64()
{
	// Values from xxh64sum (xxHash 0.8)
	assert(Digest::xxh64("", 0) == 0xef46db3751d8e999ULL);
	assert(Digest::xxh64("abc", 3) == 0x44bc2cf5ad770999ULL);
	assert(Digest::xxh64("123456789", 9) == 0x8cb841db40e6ae83ULL);
	const auto p4096 = pattern(4096);
	assert(Digest::xxh64(p4096.data(), 4096) == 0x796398cd432797ccULL);
	assert(Digest::xxh64(p4096.data(), 4093) == 0x720a56546800ad6bULL);
}

static
void
test_kinds()
{
	using namespace bees_csum;
	assert(kind_from_text("") == KIND_CRC64);
	assert(kind_from_text("crc64\n") == KIND_CRC64);
	assert(kind_from_text("csum-crc32c\n") == KIND_CRC32C);
	assert(kind_from_text("  csum-xxhash\n") == KIND_XXHASH);
	assert(kind_from_text("sha256\n") == KIND_UNKNOWN);
	for (auto k : { KIND_CRC64, KIND_CRC32C, KIND_XXHASH }) {
		assert(kind_from_text(string(kind_name(k)) + "\n") == k);
	}
	assert(kind_for_fs(BTRFS_CRC32C, 4, 4096) == KIND_CRC32C);
	assert(kind_for_fs(BTRFS_XXHASH, 8, 4096) == KIND_XXHASH);
	assert(kind_for_fs(2, 32, 4096) == KIND_CRC64);	// sha256
	assert(kind_for_fs(3, 32, 4096) == KIND_CRC64);	// blake2
	assert(kind_for_fs(BTRFS_CRC32C, 4, 16384) == KIND_CRC64);
}

static
void
test_keys()
{
	using namespace bees_csum;
	for (auto k : { KIND_CRC32C, KIND_XXHASH }) {
		const auto p = pattern(4096, 13, 5);
		// The key from stored csum bytes equals the key from the data
		uint8_t sum[8];
		csum_of_data(k, p.data(), 4096, sum);
		assert(key_from_csum(k, sum, 4096) == key_from_data(k, p.data(), 4096));
		if (k == KIND_CRC32C) {
			uint32_t c = Digest::crc32c(p.data(), 4096);
			assert(memcmp(&c, sum, 4) == 0);
		} else {
			uint64_t c = Digest::xxh64(p.data(), 4096);
			assert(memcmp(&c, sum, 8) == 0);
		}
		// A partial block is checksummed with the zeros after it (as btrfs does at EOF)...
		auto padded = pattern(4096, 13, 5);
		memset(padded.data() + 1000, 0, 4096 - 1000);
		uint8_t psum[8];
		csum_of_data(k, p.data(), 1000, psum);
		uint8_t fsum[8];
		csum_of_data(k, padded.data(), 4096, fsum);
		assert(memcmp(psum, fsum, csum_size(k)) == 0);
		// ...but its key differs from the full block's, and from a partial block of another length
		// with the same padded content
		assert(key_from_csum(k, psum, 1000) != key_from_csum(k, fsum, 4096));
		assert(key_from_data(k, padded.data(), 1000) != key_from_data(k, padded.data(), 1001));
		// The zero block
		const vector<uint8_t> zero(4096, 0);
		uint8_t zsum[8];
		csum_of_data(k, zero.data(), 4096, zsum);
		assert(is_zero_block_csum(k, zsum));
		assert(!is_zero_block_csum(k, sum));
		// Different data, different keys (a few thousand blocks; crc32c collides at 2^-32)
		set<uint64_t> keys;
		for (unsigned i = 0; i < 4000; ++i) {
			vector<uint8_t> q(4096);
			uint64_t x = i * 0x9e3779b97f4a7c15ULL + 1;
			for (auto &c : q) {
				x = x * 6364136223846793005ULL + 1442695040888963407ULL;
				c = x >> 56;
			}
			keys.insert(key_from_data(k, q.data(), 4096));
		}
		assert(keys.size() == 4000);
	}
	// mix64 is a bijection: distinct inputs, distinct outputs (spot check), and spreads low bits
	set<uint64_t> mixed;
	for (uint64_t i = 0; i < 65536; ++i) {
		mixed.insert(mix64(i));
	}
	assert(mixed.size() == 65536);
	unsigned high_set = 0;
	for (uint64_t i = 0; i < 1024; ++i) {
		high_set += mix64(i) >> 63;
	}
	assert(high_set > 400 && high_set < 624);
}

static
void
test_extent_sums()
{
	using namespace bees_csum;
	// An extent of 10 blocks at 1 MiB; csum items cover it in pieces, one block is missing
	const uint64_t start = 1024 * 1024;
	ExtentSums s(KIND_CRC32C, start, 10);
	assert(s.missing() == 10);
	vector<uint8_t> item(4 * 20);
	for (size_t i = 0; i < 20; ++i) {
		const uint32_t v = 1000 + i;
		memcpy(item.data() + 4 * i, &v, 4);
	}
	// An item that starts 2 blocks before the extent: get_sums hands over only the overlap,
	// but anything before the start must be ignored, not misplaced
	s.add(start - 2 * 4096, item.data(), 8);
	assert(s.missing() == 10);
	// Blocks 0..3
	s.add(start, item.data(), 16);
	// Blocks 5..9 and more past the end (ignored)
	s.add(start + 5 * 4096, item.data() + 20, 4 * 8);
	// Not on a block boundary: ignored
	s.add(start + 100, item.data(), 4);
	assert(s.missing() == 1);
	assert(!s.have(4));
	assert(!s.have(10));
	for (size_t b : { 0, 1, 2, 3 }) {
		uint32_t v;
		memcpy(&v, s.sum(b), 4);
		assert(s.have(b) && v == 1000 + b);
	}
	for (size_t b : { 5, 6, 7, 8, 9 }) {
		uint32_t v;
		memcpy(&v, s.sum(b), 4);
		assert(s.have(b) && v == 1000 + 5 + (b - 5));
	}
	// xxhash: 8 bytes per block
	ExtentSums x(KIND_XXHASH, start, 3);
	vector<uint8_t> xitem(8 * 3);
	for (size_t i = 0; i < 3; ++i) {
		const uint64_t v = 0x1111111111111111ULL * (i + 1);
		memcpy(xitem.data() + 8 * i, &v, 8);
	}
	x.add(start, xitem.data(), xitem.size());
	assert(x.missing() == 0);
	uint64_t v2;
	memcpy(&v2, x.sum(2), 8);
	assert(v2 == 0x3333333333333333ULL);
}

int
main(int, char**)
{
	RUN_A_TEST(test_crc32c());
	RUN_A_TEST(test_xxh64());
	RUN_A_TEST(test_kinds());
	RUN_A_TEST(test_keys());
	RUN_A_TEST(test_extent_sums());

	exit(EXIT_SUCCESS);
}
