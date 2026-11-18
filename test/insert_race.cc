#include "tests.h"

#include "../src/bees-insert-race.h"

#include <atomic>
#include <cassert>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace std;

// A cell as in BeesHashTable
struct Cell {
	uint64_t e_hash;
	uint64_t e_addr;
};

// Addresses in these tests: extent number * 0x100000 + block number * 0x1000
static
uint64_t
extent_of(const uint64_t addr)
{
	return addr / 0x100000;
}

static
uint64_t
physical(const uint64_t addr)
{
	return addr;
}

struct Block {
	uint64_t		m_hash;
	uint64_t		m_addr;
	bool			m_looked_up;
	vector<uint64_t>	m_seen;
};

// A hash table with what the templates need: every operation locks, as the
// real table locks a bucket; nothing spans two operations.
class Table {
	mutex		m_mutex;
	vector<Cell>	m_cells;
public:
	vector<uint64_t> find(const uint64_t hash)
	{
		unique_lock<mutex> lock(m_mutex);
		vector<uint64_t> rv;
		for (const auto &i : m_cells) {
			if (i.e_hash == hash) {
				rv.push_back(i.e_addr);
			}
		}
		return rv;
	}
	bool raced(const Block &block)
	{
		if (!block.m_looked_up) {
			return false;
		}
		unique_lock<mutex> lock(m_mutex);
		return bees_hash_at_unseen_addr(m_cells.begin(), m_cells.end(), block.m_hash, physical(block.m_addr), block.m_seen, physical);
	}
	void insert(const Block &block)
	{
		unique_lock<mutex> lock(m_mutex);
		m_cells.push_back(Cell { block.m_hash, block.m_addr });
	}
	size_t count_extent(const uint64_t extent)
	{
		unique_lock<mutex> lock(m_mutex);
		size_t rv = 0;
		for (const auto &i : m_cells) {
			if (extent_of(i.e_addr) == extent) {
				++rv;
			}
		}
		return rv;
	}
};

static
void
test_unseen()
{
	const vector<Cell> cells {
		{ 100, 0x101000 },
		{ 200, 0x102000 },
		{ 100, 0x201000 },
	};
	const vector<uint64_t> none;
	// Another hash: not there
	assert(!bees_hash_at_unseen_addr(cells.begin(), cells.end(), 300, 0x301000, none, physical));
	// Our hash at two addresses we have not seen
	assert(bees_hash_at_unseen_addr(cells.begin(), cells.end(), 100, 0x301000, none, physical));
	// One of them seen: the other one still counts
	assert(bees_hash_at_unseen_addr(cells.begin(), cells.end(), 100, 0x301000, vector<uint64_t> { 0x101000 }, physical));
	// Both seen
	assert(!bees_hash_at_unseen_addr(cells.begin(), cells.end(), 100, 0x301000, vector<uint64_t> { 0x101000, 0x201000 }, physical));
	// Our own block is not somebody else's
	assert(!bees_hash_at_unseen_addr(cells.begin(), cells.end(), 200, 0x102000, none, physical));
	assert(bees_hash_at_unseen_addr(cells.begin(), cells.end(), 100, 0x101000, none, physical));
	assert(!bees_hash_at_unseen_addr(cells.begin(), cells.end(), 100, 0x101000, vector<uint64_t> { 0x201000 }, physical));
	// An empty table has nothing
	assert(!bees_hash_at_unseen_addr(cells.begin(), cells.begin(), 100, 0x301000, none, physical));
}

static
void
test_all_or_none()
{
	mutex insert_mutex;
	Table table;
	const vector<Block> first {
		{ 100, 0x100000, true, {} },
		{ 101, 0x101000, true, {} },
	};
	assert(bees_insert_unless_raced(insert_mutex, table, first) == 0);
	assert(table.count_extent(1) == 2);

	// The last block's hash was inserted by someone else: nothing goes in,
	// not even the blocks before it
	const vector<Block> second {
		{ 300, 0x200000, true, {} },
		{ 301, 0x201000, true, {} },
		{ 101, 0x202000, true, {} },
	};
	assert(bees_insert_unless_raced(insert_mutex, table, second) == 1);
	assert(table.count_extent(2) == 0);

	// Every block that raced is counted
	const vector<Block> both {
		{ 100, 0x400000, true, {} },
		{ 500, 0x401000, true, {} },
		{ 101, 0x402000, true, {} },
	};
	assert(bees_insert_unless_raced(insert_mutex, table, both) == 2);
	assert(table.count_extent(4) == 0);

	// Once the lookup has seen that address, it is not news any more
	auto second_seen = second;
	second_seen.at(2).m_seen.push_back(0x101000);
	assert(bees_insert_unless_raced(insert_mutex, table, second_seen) == 0);
	assert(table.count_extent(2) == 3);

	// A block that was not looked up is not checked
	const vector<Block> third {
		{ 100, 0x300000, false, {} },
	};
	assert(bees_insert_unless_raced(insert_mutex, table, third) == 0);
	assert(table.count_extent(3) == 1);
}

static
void
test_worthwhile()
{
	// (raced, duplicates found by the first scan, zero blocks, blocks of data)
	// A whole extent turned up: scan again
	assert(bees_race_rescan_worthwhile(32, 0, 0, 32));
	assert(bees_race_rescan_worthwhile(1, 0, 0, 1));
	// Half of it: deduping 16 blocks costs a copy of 16, which bees accepts
	assert(bees_race_rescan_worthwhile(16, 0, 0, 32));
	// Less than half: the copy would cost more than the dedupe frees
	assert(!bees_race_rescan_worthwhile(15, 0, 0, 32));
	// One block that is in every other file, in a 128M extent
	assert(!bees_race_rescan_worthwhile(1, 0, 0, 32768));
	// The first scan found duplicates but not enough of them; with the new ones it is enough
	assert(!bees_race_rescan_worthwhile(4, 8, 0, 32));
	assert(bees_race_rescan_worthwhile(8, 8, 0, 32));
	// Zero blocks are freed (holes) by the same rewrite, so they count for it
	assert(!bees_race_rescan_worthwhile(8, 0, 15, 32));
	assert(bees_race_rescan_worthwhile(8, 0, 16, 32));
	// More reported than there is: no underflow
	assert(bees_race_rescan_worthwhile(40, 40, 0, 32));
	// Nothing raced, nothing to gain
	assert(!bees_race_rescan_worthwhile(0, 0, 0, 32));
}

// What a worker did with its extent
struct Outcome {
	bool		m_kept = false;
	uint64_t	m_dedupe_src = 0;
	unsigned	m_scans = 0;
};

// One worker scanning one extent the way BeesContext::scan_one_extent does:
// look up every block, dedupe against the first copy found, else insert.
// With check == false it inserts the way bees did before: every hash, no questions.
static
Outcome
scan_extent(mutex &insert_mutex, Table &table, const uint64_t extent, const vector<uint64_t> &hashes, const bool check, const atomic<bool> &go)
{
	while (!go) {
		this_thread::yield();
	}
	Outcome rv;
	for (unsigned rescans = 0; ; ++rescans) {
		++rv.m_scans;
		vector<Block> blocks;
		for (size_t n = 0; n < hashes.size(); ++n) {
			Block block { hashes.at(n), extent * 0x100000 + n * 0x1000, true, {} };
			block.m_seen = table.find(block.m_hash);
			for (const auto &addr : block.m_seen) {
				if (extent_of(addr) != extent) {
					// Found a copy: this extent is deduped away, nothing is inserted
					rv.m_dedupe_src = extent_of(addr);
					return rv;
				}
			}
			blocks.push_back(block);
		}
		if (!check) {
			for (const auto &block : blocks) {
				table.insert(block);
			}
			rv.m_kept = true;
			return rv;
		}
		if (bees_insert_unless_raced(insert_mutex, table, blocks) == 0) {
			rv.m_kept = true;
			return rv;
		}
		// Somebody else's copy went in behind our lookups.  A second scan must find it.
		assert(rescans < 1);
	}
}

// Run `workers` threads, each scanning its own extent, all with the same hashes.
// Returns the number of extents that were kept.
static
size_t
race_identical(const size_t workers, const size_t blocks, const bool check)
{
	mutex insert_mutex;
	Table table;
	vector<uint64_t> hashes;
	for (size_t n = 0; n < blocks; ++n) {
		hashes.push_back(1000 + n);
	}
	vector<Outcome> outcomes(workers);
	vector<thread> threads;
	atomic<bool> go(false);
	for (size_t w = 0; w < workers; ++w) {
		threads.emplace_back([&, w]() {
			outcomes.at(w) = scan_extent(insert_mutex, table, w + 1, hashes, check, go);
		});
	}
	go = true;
	for (auto &t : threads) {
		t.join();
	}
	size_t kept = 0;
	for (size_t w = 0; w < workers; ++w) {
		if (outcomes.at(w).m_kept) {
			++kept;
			// A kept extent is in the table in full
			assert(table.count_extent(w + 1) == blocks);
		} else {
			// Nothing of an extent that was deduped away is in the table
			assert(table.count_extent(w + 1) == 0 || !check);
		}
	}
	if (check) {
		// Every extent that was not kept was deduped against one that was
		for (size_t w = 0; w < workers; ++w) {
			if (!outcomes.at(w).m_kept) {
				const auto src = outcomes.at(w).m_dedupe_src;
				assert(src >= 1 && src <= workers);
				assert(outcomes.at(src - 1).m_kept);
			}
		}
	}
	return kept;
}

static
void
test_race()
{
	// Identical extents scanned at the same time: exactly one is kept,
	// however the threads interleave
	for (const size_t workers : { 2, 3, 4, 8 }) {
		for (unsigned round = 0; round < 500; ++round) {
			assert(race_identical(workers, 1 + round % 17, true) == 1);
		}
	}
}

static
void
test_race_without_check()
{
	// The same race with the insert that bees had before: more than one
	// extent kept means copies that were never deduped.  This is only
	// reported, not asserted: on a single CPU the threads may never
	// interleave, and then test_race had nothing to catch either.
	size_t races_lost = 0;
	const unsigned rounds = 2000;
	for (unsigned round = 0; round < rounds; ++round) {
		if (race_identical(4, 64, false) > 1) {
			++races_lost;
		}
	}
	cerr << "without the check, " << races_lost << " of " << rounds << " rounds kept more than one copy ";
}

static
void
test_overlap()
{
	// Two extents that share only some hashes, in a different order.
	// One is inserted, the other one finds the shared blocks in its second scan.
	for (unsigned round = 0; round < 500; ++round) {
		mutex insert_mutex;
		Table table;
		const vector<uint64_t> hashes_a { 1, 2, 3, 4, 50, 51 };
		const vector<uint64_t> hashes_b { 60, 61, 50, 51, 62 };
		Outcome a, b;
		atomic<bool> go(false);
		thread ta([&]() { a = scan_extent(insert_mutex, table, 1, hashes_a, true, go); });
		thread tb([&]() { b = scan_extent(insert_mutex, table, 2, hashes_b, true, go); });
		go = true;
		ta.join();
		tb.join();
		// Never both, never neither
		assert(a.m_kept != b.m_kept);
		if (a.m_kept) {
			assert(b.m_dedupe_src == 1);
			assert(table.count_extent(1) == hashes_a.size());
			assert(table.count_extent(2) == 0);
		} else {
			assert(a.m_dedupe_src == 2);
			assert(table.count_extent(2) == hashes_b.size());
			assert(table.count_extent(1) == 0);
		}
	}
}

int
main(int, char**)
{
	RUN_A_TEST(test_unseen());
	RUN_A_TEST(test_all_or_none());
	RUN_A_TEST(test_worthwhile());
	RUN_A_TEST(test_race());
	RUN_A_TEST(test_race_without_check());
	RUN_A_TEST(test_overlap());

	exit(EXIT_SUCCESS);
}
