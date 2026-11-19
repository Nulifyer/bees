#include "crucible/csum.h"

#include <cstring>

#if defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace crucible {
	namespace Digest {

		static const uint32_t s_crc32c_poly = 0x82f63b78;	// reflected Castagnoli polynomial

		struct Crc32cTable {
			uint32_t t[8][256];
			Crc32cTable() {
				for (uint32_t n = 0; n < 256; ++n) {
					uint32_t c = n;
					for (int k = 0; k < 8; ++k) {
						c = (c & 1) ? (c >> 1) ^ s_crc32c_poly : c >> 1;
					}
					t[0][n] = c;
				}
				for (uint32_t n = 0; n < 256; ++n) {
					uint32_t c = t[0][n];
					for (int k = 1; k < 8; ++k) {
						c = t[0][c & 0xff] ^ (c >> 8);
						t[k][n] = c;
					}
				}
			}
		};

		static const Crc32cTable &
		crc32c_table()
		{
			static const Crc32cTable s_table;
			return s_table;
		}

		uint32_t
		crc32c_sw(const void *p, size_t len)
		{
			const auto &t = crc32c_table().t;
			const uint8_t *b = static_cast<const uint8_t *>(p);
			uint32_t crc = ~uint32_t(0);
			while (len >= 8) {
				uint64_t w;
				memcpy(&w, b, sizeof(w));
				w ^= crc;	// little-endian
				crc = t[7][w & 0xff] ^ t[6][(w >> 8) & 0xff] ^ t[5][(w >> 16) & 0xff] ^ t[4][(w >> 24) & 0xff]
					^ t[3][(w >> 32) & 0xff] ^ t[2][(w >> 40) & 0xff] ^ t[1][(w >> 48) & 0xff] ^ t[0][w >> 56];
				b += 8;
				len -= 8;
			}
			while (len--) {
				crc = t[0][(crc ^ *b++) & 0xff] ^ (crc >> 8);
			}
			return ~crc;
		}

#if defined(__x86_64__)
		__attribute__((target("sse4.2")))
		static uint32_t
		crc32c_hw(const void *p, size_t len)
		{
			const uint8_t *b = static_cast<const uint8_t *>(p);
			uint64_t crc = ~uint32_t(0);
			while (len >= 8) {
				uint64_t w;
				memcpy(&w, b, sizeof(w));
				crc = _mm_crc32_u64(crc, w);
				b += 8;
				len -= 8;
			}
			uint32_t crc32 = crc;
			while (len--) {
				crc32 = _mm_crc32_u8(crc32, *b++);
			}
			return ~crc32;
		}

		static bool
		have_sse42()
		{
			static const bool s_have = __builtin_cpu_supports("sse4.2");
			return s_have;
		}
#endif

		uint32_t
		crc32c(const void *p, size_t len)
		{
#if defined(__x86_64__)
			if (have_sse42()) {
				return crc32c_hw(p, len);
			}
#endif
			return crc32c_sw(p, len);
		}

		static const uint64_t P1 = 0x9e3779b185ebca87ULL;
		static const uint64_t P2 = 0xc2b2ae3d27d4eb4fULL;
		static const uint64_t P3 = 0x165667b19e3779f9ULL;
		static const uint64_t P4 = 0x85ebca77c2b2ae63ULL;
		static const uint64_t P5 = 0x27d4eb2f165667c5ULL;

		static inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
		static inline uint64_t read64(const uint8_t *b) { uint64_t v; memcpy(&v, b, sizeof(v)); return v; }
		static inline uint32_t read32(const uint8_t *b) { uint32_t v; memcpy(&v, b, sizeof(v)); return v; }
		static inline uint64_t xxh_round(uint64_t acc, uint64_t input) { acc += input * P2; acc = rotl64(acc, 31); return acc * P1; }
		static inline uint64_t xxh_merge(uint64_t acc, uint64_t val) { acc ^= xxh_round(0, val); return acc * P1 + P4; }

		uint64_t
		xxh64(const void *p, size_t len)
		{
			const uint8_t *b = static_cast<const uint8_t *>(p);
			const uint8_t *const end = b + len;
			const uint64_t seed = 0;
			uint64_t h;
			if (len >= 32) {
				uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
				const uint8_t *const limit = end - 32;
				do {
					v1 = xxh_round(v1, read64(b));
					v2 = xxh_round(v2, read64(b + 8));
					v3 = xxh_round(v3, read64(b + 16));
					v4 = xxh_round(v4, read64(b + 24));
					b += 32;
				} while (b <= limit);
				h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
				h = xxh_merge(h, v1);
				h = xxh_merge(h, v2);
				h = xxh_merge(h, v3);
				h = xxh_merge(h, v4);
			} else {
				h = seed + P5;
			}
			h += len;
			while (b + 8 <= end) {
				h ^= xxh_round(0, read64(b));
				h = rotl64(h, 27) * P1 + P4;
				b += 8;
			}
			if (b + 4 <= end) {
				h ^= uint64_t(read32(b)) * P1;
				h = rotl64(h, 23) * P2 + P3;
				b += 4;
			}
			while (b < end) {
				h ^= uint64_t(*b++) * P5;
				h = rotl64(h, 11) * P1;
			}
			h ^= h >> 33;
			h *= P2;
			h ^= h >> 29;
			h *= P3;
			h ^= h >> 32;
			return h;
		}
	};
};
