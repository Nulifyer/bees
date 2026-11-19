#ifndef BEES_CSUM_H
#define BEES_CSUM_H

// Hash table keys made from btrfs data checksums (--scan-csums).
//
// Without the option a block's key in the hash table is bees' own CRC64 of
// its data, so every block has to be read to be looked up.  With it the key
// is made from the checksum btrfs stores for the block (crc32c, or xxhash64
// with `mkfs.btrfs --csum xxhash`), which bees can fetch from the csum tree
// for a whole extent at a time without reading the data.  For an extent with
// no match in the table, nothing is read at all.
//
// btrfs checksums what is on disk.  For an uncompressed extent that is the
// data, block by block, so bees can use the stored checksums as they are.
// For a compressed extent it is the compressed bytes, which say nothing about
// the data; bees reads those blocks and computes the same checksum of the
// data itself.  Both ways give the same key for the same 4K of data, so an
// uncompressed copy and a compressed copy of a block still find each other.
//
// A key is only ever a candidate: crc32c has 32 bits, and two blocks with the
// same key are compared byte for byte (by bees, then by the kernel's dedupe)
// before anything is shared.  A false candidate costs a lookup and a read.
//
// This file has the parts without I/O, for the unit test.

#include "crucible/csum.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bees_csum {

	// What the keys in a hash table were made from.  Stored in $BEESHOME/beeshash.type.
	enum Kind {
		KIND_CRC64 = 0,		// bees' own hash of the data (no file, or "crc64")
		KIND_CRC32C = 1,	// --scan-csums on a crc32c filesystem
		KIND_XXHASH = 2,	// --scan-csums on an xxhash64 filesystem
		KIND_UNKNOWN = 99,
	};

	// btrfs csum types (BTRFS_CSUM_TYPE_*)
	const uint16_t BTRFS_CRC32C = 0;
	const uint16_t BTRFS_XXHASH = 1;

	// The block size of the csum tree that bees can use (bees' BLOCK_SIZE_SUMS)
	const size_t BLOCK = 4096;

	inline const char *
	kind_name(Kind k)
	{
		switch (k) {
			case KIND_CRC64: return "crc64";
			case KIND_CRC32C: return "csum-crc32c";
			case KIND_XXHASH: return "csum-xxhash";
			default: return "unknown";
		}
	}

	/// The kind named on the first line of beeshash.type.  An empty or missing file is KIND_CRC64:
	/// every hash table made before the file existed was keyed by bees' own hash.
	inline Kind
	kind_from_text(const std::string &text)
	{
		std::string word;
		for (const char c : text) {
			if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
				if (word.empty()) continue;
				break;
			}
			word += c;
		}
		if (word.empty() || word == "crc64") return KIND_CRC64;
		if (word == "csum-crc32c") return KIND_CRC32C;
		if (word == "csum-xxhash") return KIND_XXHASH;
		return KIND_UNKNOWN;
	}

	/// The kind --scan-csums can use on a filesystem with this csum type and sector size,
	/// or KIND_CRC64 if it can't (sha256 and blake2 are not done; nor are sectors other than 4K).
	inline Kind
	kind_for_fs(uint16_t csum_type, size_t csum_size, size_t sector_size)
	{
		if (sector_size != BLOCK) return KIND_CRC64;
		if (csum_type == BTRFS_CRC32C && csum_size == 4) return KIND_CRC32C;
		if (csum_type == BTRFS_XXHASH && csum_size == 8) return KIND_XXHASH;
		return KIND_CRC64;
	}

	inline size_t
	csum_size(Kind k)
	{
		return k == KIND_CRC32C ? 4 : k == KIND_XXHASH ? 8 : 0;
	}

	/// A bijection on 64 bits (the splitmix64 finalizer), so that 32-bit crc32c
	/// values spread over the whole key space like bees' own hashes do.
	inline uint64_t
	mix64(uint64_t z)
	{
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		return z ^ (z >> 31);
	}

	/// The hash table key of a block from its btrfs checksum (csum_size bytes, as
	/// stored: little-endian) and the length of the block's data.  btrfs checksums
	/// the whole 4K sector, so the zeros after EOF in a file's last block are in it;
	/// the length goes into the key so that blocks of different lengths never meet
	/// (bees only dedupes a partial block with one of the same length, at EOF).
	inline uint64_t
	key_from_csum(Kind k, const uint8_t *csum, size_t data_len)
	{
		uint64_t v = 0;
		if (k == KIND_CRC32C) {
			uint32_t c;
			memcpy(&c, csum, 4);
			v = c;
		} else {
			memcpy(&v, csum, 8);
		}
		if (data_len != BLOCK) {
			v ^= uint64_t(data_len) << 50;
		}
		return mix64(v ^ 0x6265657363737521ULL);
	}

	/// The checksum btrfs would store for a block holding this data (zero-padded to 4K),
	/// little-endian, in out[0..csum_size(k)).
	inline void
	csum_of_data(Kind k, const uint8_t *data, size_t len, uint8_t *out)
	{
		uint8_t padded[BLOCK];
		const uint8_t *p = data;
		if (len < BLOCK) {
			memcpy(padded, data, len);
			memset(padded + len, 0, BLOCK - len);
			p = padded;
		}
		if (k == KIND_CRC32C) {
			const uint32_t c = crucible::Digest::crc32c(p, BLOCK);
			memcpy(out, &c, 4);
		} else {
			const uint64_t c = crucible::Digest::xxh64(p, BLOCK);
			memcpy(out, &c, 8);
		}
	}

	/// The key of a block from its data: the same as key_from_csum of what btrfs stores for it
	inline uint64_t
	key_from_data(Kind k, const uint8_t *data, size_t len)
	{
		uint8_t sum[8];
		csum_of_data(k, data, len, sum);
		return key_from_csum(k, sum, len);
	}

	/// Is this the checksum of a block of 4K zeros?  (Those are read: bees punches holes for zeros.)
	inline bool
	is_zero_block_csum(Kind k, const uint8_t *csum)
	{
		static const uint8_t s_zero[BLOCK] = { 0 };
		static const uint32_t s_crc32c = crucible::Digest::crc32c(s_zero, BLOCK);
		static const uint64_t s_xxh64 = crucible::Digest::xxh64(s_zero, BLOCK);
		if (k == KIND_CRC32C) return memcmp(&s_crc32c, csum, 4) == 0;
		return memcmp(&s_xxh64, csum, 8) == 0;
	}

	/// Checksums of the blocks [start, start + count * BLOCK) of one uncompressed extent,
	/// collected from the csum tree's items: one slot per block, empty if the tree had none.
	class ExtentSums {
		Kind			m_kind;
		uint64_t		m_start;
		size_t			m_count;
		std::vector<uint8_t>	m_sums;
		std::vector<bool>	m_have;
	public:
		ExtentSums(Kind k, uint64_t start, size_t count) :
			m_kind(k), m_start(start), m_count(count), m_sums(count * csum_size(k)), m_have(count, false) { }

		/// What BtrfsCsumTreeFetcher::get_sums hands over: the sums of the blocks from `logical` on.
		/// Anything outside the extent, or not on a block boundary, is ignored.
		void add(uint64_t logical, const uint8_t *buf, size_t bytes)
		{
			const size_t sz = csum_size(m_kind);
			if (logical < m_start || (logical - m_start) % BLOCK) return;
			const uint64_t first = (logical - m_start) / BLOCK;
			for (size_t i = 0; i < bytes / sz; ++i) {
				const uint64_t b = first + i;
				if (b >= m_count) break;
				memcpy(m_sums.data() + b * sz, buf + i * sz, sz);
				m_have[b] = true;
			}
		}

		size_t count() const { return m_count; }
		bool have(size_t block) const { return block < m_count && m_have[block]; }
		const uint8_t *sum(size_t block) const { return m_sums.data() + block * csum_size(m_kind); }
		size_t missing() const
		{
			size_t n = 0;
			for (size_t i = 0; i < m_count; ++i) n += !m_have[i];
			return n;
		}
	};

};

#endif // BEES_CSUM_H
