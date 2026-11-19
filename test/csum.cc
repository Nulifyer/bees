#include "tests.h"

#include "crucible/csum.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
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

int
main(int, char**)
{
	RUN_A_TEST(test_crc32c());
	RUN_A_TEST(test_xxh64());

	exit(EXIT_SUCCESS);
}
