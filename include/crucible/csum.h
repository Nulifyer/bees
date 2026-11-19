#ifndef CRUCIBLE_CSUM_H
#define CRUCIBLE_CSUM_H

#include <cstddef>
#include <cstdint>

namespace crucible {
	namespace Digest {
		/// CRC-32C (Castagnoli) as btrfs stores it for data blocks:
		/// initial value ~0, final value inverted ("123456789" gives 0xe3069283).
		/// Uses the SSE 4.2 instruction when the CPU has it.
		uint32_t crc32c(const void *p, size_t len);
		/// The table-driven version, for tests.
		uint32_t crc32c_sw(const void *p, size_t len);

		/// XXH64 with seed 0, as btrfs stores it with `mkfs.btrfs --csum xxhash`.
		uint64_t xxh64(const void *p, size_t len);
	};
};

#endif
