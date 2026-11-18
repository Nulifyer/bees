#include "tests.h"

#include "../src/bees-hash-journal.h"

#include <cassert>
#include <cstdlib>
#include <tuple>
#include <vector>

using namespace std;
using namespace bees_hash_journal;

using Seen = vector<tuple<uint64_t, uint64_t, uint64_t>>;

static
Seen
as_seen(const vector<Record> &records)
{
	Seen rv;
	for (const auto &i : records) {
		rv.push_back(make_tuple(i.m_op, i.m_hash, i.m_addr));
	}
	return rv;
}

// Decode one batch and return what the visitor saw
static
bool
decode_batch(const vector<uint8_t> &batch, Seen &seen)
{
	return decode(batch.data(), batch.size(), [&](uint64_t op, uint64_t hash, uint64_t addr) {
		seen.push_back(make_tuple(op, hash, addr));
	});
}

// Replay a journal held in memory and return where it ends
static
uint64_t
replay_file(const vector<uint8_t> &file, Seen &seen)
{
	return replay(file.size(), [&](uint64_t pos, size_t len, uint8_t *buf) {
		assert(pos + len <= file.size());
		memcpy(buf, file.data() + pos, len);
	}, [&](uint64_t op, uint64_t hash, uint64_t addr) {
		seen.push_back(make_tuple(op, hash, addr));
	});
}

static
vector<Record>
some_records(const size_t count, const uint64_t seed)
{
	vector<Record> rv;
	uint64_t x = seed * 0x9e3779b97f4a7c15ULL + 1;
	for (size_t i = 0; i < count; ++i) {
		x = x * 6364136223846793005ULL + 1442695040888963407ULL;
		// Mostly inserts in long runs, as in real life, with the other two mixed in
		const uint64_t op = (x >> 60) < 13 ? OP_INSERT : (x >> 60) < 15 ? OP_FRONT : OP_ERASE;
		rv.push_back(Record { op, x, (x >> 8) | MIN_ADDR });
	}
	return rv;
}

static
void
append(vector<uint8_t> &file, const vector<uint8_t> &batch)
{
	file.insert(file.end(), batch.begin(), batch.end());
}

// Recompute the checksum of a batch after the test has tampered with its cells
static
void
fix_checksum(vector<uint8_t> &batch)
{
	const uint64_t cells = get_u64(batch.data() + 8);
	const uint64_t crc = crucible::Digest::CRC::crc64(batch.data() + HEADER_SIZE, cells * CELL_SIZE);
	memcpy(batch.data() + 16, &crc, sizeof(crc));
}

static
void
test_empty()
{
	// Nothing to write is not a batch
	assert(encode(vector<Record>()).empty());
	// An empty journal, and one that is all zeros
	Seen seen;
	assert(replay_file(vector<uint8_t>(), seen) == 0);
	assert(replay_file(vector<uint8_t>(3 * BATCH_ALIGN, 0), seen) == 0);
	assert(seen.empty());
	const vector<uint8_t> zeros(HEADER_SIZE, 0);
	assert(batch_size(zeros.data()) == 0);
}

static
void
test_roundtrip()
{
	for (const size_t count : { 1, 2, 3, 100, 253, 254, 255, 256, 257, 1000, 100000 }) {
		const auto records = some_records(count, count);
		const auto batch = encode(records);
		// Whole blocks, so the next batch starts on a block boundary
		assert(!batch.empty());
		assert(batch.size() % BATCH_ALIGN == 0);
		assert(batch_size(batch.data()) == batch.size());
		// Not more than a marker per record and a block of padding
		assert(batch.size() <= HEADER_SIZE + 2 * count * CELL_SIZE + BATCH_ALIGN);
		Seen seen;
		assert(decode_batch(batch, seen));
		assert(seen == as_seen(records));
	}
}

static
void
test_runs()
{
	// One run per change of operation, records in order
	const vector<Record> records {
		{ OP_INSERT, 1, 0x1000 },
		{ OP_INSERT, 2, 0x2000 },
		{ OP_ERASE,  1, 0x1000 },
		{ OP_FRONT,  2, 0x2000 },
		{ OP_FRONT,  3, 0x3000 },
		{ OP_INSERT, 1, 0x1000 },
	};
	const auto batch = encode(records);
	// 6 records and 4 markers
	assert(get_u64(batch.data() + 8) == 10);
	Seen seen;
	assert(decode_batch(batch, seen));
	assert(seen == as_seen(records));
}

static
void
test_replay()
{
	const auto first = some_records(1000, 1);
	const auto second = some_records(10, 2);
	const auto third = some_records(70000, 3);
	vector<uint8_t> file;
	append(file, encode(first));
	const auto end_of_first = file.size();
	append(file, encode(second));
	const auto end_of_second = file.size();
	append(file, encode(third));

	auto all = as_seen(first);
	const auto first_and_second_count = first.size() + second.size();
	for (const auto &i : as_seen(second)) all.push_back(i);
	for (const auto &i : as_seen(third)) all.push_back(i);

	// The whole journal
	{
		Seen seen;
		assert(replay_file(file, seen) == file.size());
		assert(seen == all);
	}

	// A crash while the third batch was being written: any shorter file
	// gives the first two batches and says the journal continues after them
	for (const size_t cut : { size_t(1), size_t(HEADER_SIZE - 1), size_t(HEADER_SIZE), size_t(BATCH_ALIGN), file.size() - end_of_second - BATCH_ALIGN, file.size() - end_of_second - 1 }) {
		auto torn = file;
		torn.resize(end_of_second + cut);
		Seen seen;
		assert(replay_file(torn, seen) == end_of_second);
		assert(seen.size() == first_and_second_count);
		assert(seen == Seen(all.begin(), all.begin() + first_and_second_count));
	}

	// The same crash, but the file already has its full length and the
	// last blocks never made it to disk
	{
		auto torn = file;
		fill(torn.end() - 2 * BATCH_ALIGN, torn.end(), 0);
		Seen seen;
		assert(replay_file(torn, seen) == end_of_second);
		assert(seen.size() == first_and_second_count);
	}

	// One flipped bit in the second batch: only the first is applied,
	// and nothing of the damaged batch
	{
		auto damaged = file;
		damaged.at(end_of_first + HEADER_SIZE + 5 * CELL_SIZE + 3) ^= 0x10;
		Seen seen;
		assert(replay_file(damaged, seen) == end_of_first);
		assert(seen == as_seen(first));
	}

	// Damaged header fields
	for (const size_t offset : { 0, 7, 8, 15, 16, 23 }) {
		auto damaged = file;
		damaged.at(end_of_first + offset) ^= 0x01;
		Seen seen;
		assert(replay_file(damaged, seen) == end_of_first);
		assert(seen == as_seen(first));
	}

	// A new journal written over the start of an old one (the journal starts over
	// by truncation, but suppose the truncation was lost): what follows the new
	// batch is either an old batch, which is harmless to apply again, or not a
	// batch.  Here the old first batch is longer than the new one, so the replay
	// lands inside it and stops.
	{
		auto reused = file;
		const auto fresh = encode(second);
		assert(fresh.size() < end_of_first);
		copy(fresh.begin(), fresh.end(), reused.begin());
		Seen seen;
		assert(replay_file(reused, seen) == fresh.size());
		assert(seen == as_seen(second));
	}
}

static
void
test_structure()
{
	const vector<Record> records {
		{ OP_INSERT, 1, 0x1000 },
		{ OP_INSERT, 2, 0x2000 },
		{ OP_ERASE,  3, 0x3000 },
	};
	const auto good = encode(records);
	Seen seen;
	assert(decode_batch(good, seen));
	seen.clear();

	// A valid checksum over cells that make no sense must not be applied, not even in part.
	// Cell 0 is the first marker { count 2, OP_INSERT }, cells 1-2 are records,
	// cell 3 is the marker { count 1, OP_ERASE }, cell 4 its record.
	const auto cell = [](vector<uint8_t> &batch, size_t n, size_t word) {
		return batch.data() + HEADER_SIZE + n * CELL_SIZE + word * 8;
	};
	const auto put = [](uint8_t *p, uint64_t v) {
		memcpy(p, &v, sizeof(v));
	};
	{
		// Unknown operation in the second run
		auto bad = good;
		put(cell(bad, 3, 1), 7);
		fix_checksum(bad);
		assert(!decode_batch(bad, seen));
	}
	{
		// A run that claims more records than the batch has
		auto bad = good;
		put(cell(bad, 3, 0), 2);
		fix_checksum(bad);
		assert(!decode_batch(bad, seen));
	}
	{
		// An empty run
		auto bad = good;
		put(cell(bad, 0, 0), 0);
		fix_checksum(bad);
		assert(!decode_batch(bad, seen));
	}
	{
		// An address that can't be one
		auto bad = good;
		put(cell(bad, 4, 1), 0xfff);
		fix_checksum(bad);
		assert(!decode_batch(bad, seen));
	}
	{
		// A cell count that does not match the size we were given
		auto bad = good;
		bad.resize(bad.size() + BATCH_ALIGN, 0);
		assert(!decode_batch(bad, seen));
	}
	{
		// A header that asks for an absurd amount of memory
		auto bad = good;
		put(bad.data() + 8, MAX_BATCH_CELLS + 1);
		assert(batch_size(bad.data()) == 0);
		put(bad.data() + 8, 0);
		assert(batch_size(bad.data()) == 0);
	}
	assert(seen.empty());
}

int
main(int, char**)
{
	RUN_A_TEST(test_empty());
	RUN_A_TEST(test_roundtrip());
	RUN_A_TEST(test_runs());
	RUN_A_TEST(test_replay());
	RUN_A_TEST(test_structure());

	exit(EXIT_SUCCESS);
}
