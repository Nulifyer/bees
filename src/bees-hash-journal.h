#ifndef BEES_HASH_JOURNAL_H
#define BEES_HASH_JOURNAL_H

#include "crucible/crc64.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// The hash table journal ($BEESHOME/beeshash.journal).
//
// A new hash goes into a bucket chosen by the hash, so a few thousand new
// blocks touch most of the table's 128K extents, and writing those extents
// out means rewriting most of beeshash.dat: 64M to 1G for a few megabytes of
// new data, again and again on a machine that keeps writing.  Writing single
// 4K buckets instead would be smaller, but still 4K per new 4K block, and it
// would cut the table's extents on disk to pieces.
//
// So changes to the table are written as what they are: 16 bytes each,
// appended to a journal.  beeshash.dat is only rewritten (in whole 128K
// extents, as before) when the journal has grown to a fraction of the
// table's size; then the journal starts over.  At startup the journal is
// applied to the table that was read from beeshash.dat.
//
// The table is a cache: an entry that is missing or stale costs a dedupe
// that is not found or a lookup that fails, never data.  The journal is
// therefore simple:
//  - A record is an operation (insert, move to front, erase) with a hash
//    and an address.  Replaying a record that is already in the table does
//    nothing, so it does not matter whether a crash left beeshash.dat
//    older or newer than the journal.
//  - Records are written in batches.  A batch has a checksum; replay stops
//    at the first batch that is damaged or cut short, which is the tail
//    that a crash left.
//  - A batch is padded to 4K, so appending the next one never rewrites a
//    block that is already on disk.
//  - An older bees ignores the file and finds a hash table that lacks the
//    newest entries.
//
// Layout of a batch, all integers in host byte order like beeshash.dat:
//   header, 32 bytes:  magic, number of cells, CRC64 of the cells, 0
//   cells, 16 bytes each (hash, address), in runs:
//     a marker cell { number of records in the run, operation },
//     then that many records { hash, address }
//   zero padding to a multiple of 4096 bytes
// An address is never below 0x1000, an operation always is, so a marker
// can't be taken for a record.
//
// This file has the format only, with no I/O, so test/hash_journal.cc can
// test it.

namespace bees_hash_journal {

	const uint64_t MAGIC = 0x314c4e4a53454542ULL;	// "BEESJNL1"
	const size_t BATCH_ALIGN = 4096;
	const size_t HEADER_SIZE = 32;
	const size_t CELL_SIZE = 16;
	/// No batch is larger than this many cells; a header that says otherwise is damaged
	const uint64_t MAX_BATCH_CELLS = 16 * 1024 * 1024;
	/// Smallest value that is an address and not an operation
	const uint64_t MIN_ADDR = 0x1000;

	enum Op : uint64_t {
		OP_INSERT = 1,	// BeesHashTable::push_random_hash_addr
		OP_FRONT = 2,	// BeesHashTable::push_front_hash_addr
		OP_ERASE = 3,	// BeesHashTable::erase_hash_addr
	};

	struct Record {
		uint64_t	m_op;
		uint64_t	m_hash;
		uint64_t	m_addr;
	};

	inline
	uint64_t
	get_u64(const uint8_t *p)
	{
		uint64_t rv;
		memcpy(&rv, p, sizeof(rv));
		return rv;
	}

	inline
	void
	put_u64(std::vector<uint8_t> &v, const uint64_t value)
	{
		const auto p = reinterpret_cast<const uint8_t *>(&value);
		v.insert(v.end(), p, p + sizeof(value));
	}

	/// Encode the records [begin, end) as one batch.  No records, no batch.
	/// At most MAX_BATCH_CELLS / 2 records fit in a batch.
	inline
	std::vector<uint8_t>
	encode(const Record *const begin, const Record *const end)
	{
		std::vector<uint8_t> rv;
		if (begin == end) {
			return rv;
		}
		rv.reserve(HEADER_SIZE + (end - begin + 16) * CELL_SIZE + BATCH_ALIGN);
		rv.resize(HEADER_SIZE, 0);
		for (auto i = begin; i < end; ) {
			auto run_end = i;
			while (run_end < end && run_end->m_op == i->m_op) {
				++run_end;
			}
			put_u64(rv, run_end - i);
			put_u64(rv, i->m_op);
			for (; i < run_end; ++i) {
				put_u64(rv, i->m_hash);
				put_u64(rv, i->m_addr);
			}
		}
		const uint64_t cells = (rv.size() - HEADER_SIZE) / CELL_SIZE;
		const uint64_t header[4] = {
			MAGIC,
			cells,
			crucible::Digest::CRC::crc64(rv.data() + HEADER_SIZE, cells * CELL_SIZE),
			0,
		};
		memcpy(rv.data(), header, HEADER_SIZE);
		rv.resize((rv.size() + BATCH_ALIGN - 1) / BATCH_ALIGN * BATCH_ALIGN, 0);
		return rv;
	}

	inline
	std::vector<uint8_t>
	encode(const std::vector<Record> &records)
	{
		return encode(records.data(), records.data() + records.size());
	}

	/// Size in bytes, padding included, of the batch whose first HEADER_SIZE
	/// bytes are at `header`.  0 if that is not the header of a batch.
	inline
	size_t
	batch_size(const uint8_t *header)
	{
		if (get_u64(header) != MAGIC) {
			return 0;
		}
		const uint64_t cells = get_u64(header + 8);
		if (cells == 0 || cells > MAX_BATCH_CELLS) {
			return 0;
		}
		return (HEADER_SIZE + cells * CELL_SIZE + BATCH_ALIGN - 1) / BATCH_ALIGN * BATCH_ALIGN;
	}

	/// Decode the batch at `batch`, which is `size` bytes long (as batch_size
	/// said), and call visit(op, hash, addr) for each record in order.
	/// Returns false, without calling visit, if the batch is damaged.
	template <class Visitor>
	bool
	decode(const uint8_t *batch, const size_t size, Visitor visit)
	{
		if (size < HEADER_SIZE || batch_size(batch) != size) {
			return false;
		}
		const uint64_t cells = get_u64(batch + 8);
		const uint8_t *const first = batch + HEADER_SIZE;
		if (crucible::Digest::CRC::crc64(first, cells * CELL_SIZE) != get_u64(batch + 16)) {
			return false;
		}
		// The checksum covers what was written.  Check the structure as
		// well before anything is applied.
		for (uint64_t i = 0; i < cells; ) {
			const uint64_t count = get_u64(first + i * CELL_SIZE);
			const uint64_t op = get_u64(first + i * CELL_SIZE + 8);
			if (op != OP_INSERT && op != OP_FRONT && op != OP_ERASE) {
				return false;
			}
			if (count == 0 || count > cells - i - 1) {
				return false;
			}
			for (uint64_t j = i + 1; j <= i + count; ++j) {
				if (get_u64(first + j * CELL_SIZE + 8) < MIN_ADDR) {
					return false;
				}
			}
			i += count + 1;
		}
		for (uint64_t i = 0; i < cells; ) {
			const uint64_t count = get_u64(first + i * CELL_SIZE);
			const uint64_t op = get_u64(first + i * CELL_SIZE + 8);
			for (uint64_t j = i + 1; j <= i + count; ++j) {
				visit(op, get_u64(first + j * CELL_SIZE), get_u64(first + j * CELL_SIZE + 8));
			}
			i += count + 1;
		}
		return true;
	}

	/// Apply a journal of `size` bytes: decode batch after batch from the
	/// start and call visit(op, hash, addr) for each record, until the end
	/// or the first batch that is damaged or cut short.  read(pos, len, buf)
	/// must put the `len` bytes at offset `pos` into `buf`.
	/// Returns the offset after the last good batch: the journal continues there.
	template <class Reader, class Visitor>
	uint64_t
	replay(const uint64_t size, Reader read, Visitor visit)
	{
		uint64_t pos = 0;
		std::vector<uint8_t> batch;
		while (size - pos >= HEADER_SIZE) {
			uint8_t header[HEADER_SIZE];
			read(pos, HEADER_SIZE, header);
			const uint64_t len = batch_size(header);
			if (!len || len > size - pos) {
				break;
			}
			batch.resize(len);
			read(pos, len, batch.data());
			if (!decode(batch.data(), len, visit)) {
				break;
			}
			pos += len;
		}
		return pos;
	}

}

#endif
