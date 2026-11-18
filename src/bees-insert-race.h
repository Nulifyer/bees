#ifndef BEES_INSERT_RACE_H
#define BEES_INSERT_RACE_H

#include <algorithm>
#include <cstdint>
#include <mutex>

// Two workers scanning identical new data.
//
// A worker scans an extent block by block: hash the block, look the hash up
// in the hash table, dedupe if another copy is found.  The extent's own
// hashes are inserted only after the whole extent was scanned, and only if
// the extent was kept (hashes of an extent that was deduped away would point
// nowhere).
//
// So when two workers scan two identical new extents at the same time, each
// one's lookups run before the other one's inserts.  Both find nothing, both
// insert, and neither extent is ever deduped: the crawl does not come back
// to an extent it has scanned.
//
// The fix is in the insert step.  A worker that kept its extent inserts the
// extent's hashes all or none, and one worker at a time:
//
//   1. Look at every hash once more.  If the table now holds one of them at
//      an address that the lookup during the scan did not return, somebody
//      inserted it since.  Insert nothing and tell the caller, who scans the
//      extent again; this time the lookups find the other copy.
//   2. Otherwise insert all hashes.
//
// Because steps 1 and 2 of one worker are not interleaved with those of
// another, the workers are ordered: of any number of identical extents the
// first one to get here is inserted and kept, and every later one sees it in
// step 1.  Nothing is inserted for an extent that is then deduped away, so
// no worker can pick as its dedupe source an extent that is about to go.
// (If each hash were checked and inserted on its own, two workers could each
// see the other's hashes on different blocks, and dedupe in both directions:
// every block shared, both extents still allocated.)
//
// A second scan that still can't dedupe ends here again.  What it found and
// rejected (a hash collision, a toxic or vanished address) is in its list of
// seen addresses now, so only yet another new copy sends it round again, and
// the caller limits the number of rounds.
//
// bees also inserts hashes when it has rewritten part of an extent (the part
// of a partly duplicate extent that was not duplicate is copied to a new
// extent).  Two workers that do this to two copies of the same data make two
// identical new extents.  The same step guards that insert, but there is no
// second scan: the worker that comes second inserts nothing, and the crawl,
// which scans every new extent, finds the first worker's copy when it gets
// to the second one.
//
// The templates need no hash table and no btrfs, so test/insert_race.cc can
// test them.

/// True if the cells [begin, end) hold `hash` at an address that is not in
/// `seen` and is not the caller's own block (physical(addr) == own_physical).
/// A cell has members e_hash and e_addr.
template <class CellIter, class SeenRange, class Physical>
bool
bees_hash_at_unseen_addr(CellIter begin, CellIter end, const uint64_t hash, const uint64_t own_physical, const SeenRange &seen, Physical physical)
{
	for (auto cell = begin; cell != end; ++cell) {
		if (cell->e_hash != hash) {
			continue;
		}
		if (physical(cell->e_addr) == own_physical) {
			continue;
		}
		if (std::find(seen.begin(), seen.end(), cell->e_addr) != seen.end()) {
			continue;
		}
		return true;
	}
	return false;
}

/// Insert all of `blocks` into `table`, or none of them if table.raced(block)
/// is true for any block.  Returns true if the blocks were inserted.
/// All callers must pass the same mutex.
template <class Table, class BlockRange>
bool
bees_insert_unless_raced(std::mutex &insert_mutex, Table &table, const BlockRange &blocks)
{
	std::unique_lock<std::mutex> lock(insert_mutex);
	for (const auto &block : blocks) {
		if (table.raced(block)) {
			return false;
		}
	}
	for (const auto &block : blocks) {
		table.insert(block);
	}
	return true;
}

#endif
