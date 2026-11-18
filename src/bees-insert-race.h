#ifndef BEES_INSERT_RACE_H
#define BEES_INSERT_RACE_H

#include <algorithm>
#include <cstddef>
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
// Not every hash that turns up is worth a second scan.  Some blocks are in
// many files (padding, a licence text, a table of constants), and with
// several workers there is nearly always one that has just inserted such a
// block.  bees does not dedupe a block or two out of an extent anyway: it
// would have to copy the rest, and it only does that if the dedupe frees at
// least as much as the copy costs.  So the second scan is made only if the
// blocks that turned up, together with the duplicates the first scan found,
// could pass that test (bees_race_rescan_worthwhile).  Otherwise the hashes
// are inserted as if nothing had happened, which is also what a single
// worker would have done with these extents, whichever came first.
//
// bees also inserts hashes when it has rewritten part of an extent (the part
// of a partly duplicate extent that was not duplicate is copied to a new
// extent).  Two workers that do this to two copies of the same data make two
// identical new extents.  The same step guards that insert: the worker that
// comes second inserts nothing and scans its new extent, which is deduped
// against the first worker's.
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
/// is true for any block.  Returns the number of blocks that raced: 0 means
/// the blocks were inserted.  All callers must pass the same mutex.
template <class Table, class BlockRange>
size_t
bees_insert_unless_raced(std::mutex &insert_mutex, Table &table, const BlockRange &blocks)
{
	std::unique_lock<std::mutex> lock(insert_mutex);
	size_t raced = 0;
	for (const auto &block : blocks) {
		if (table.raced(block)) {
			++raced;
		}
	}
	if (raced) {
		return raced;
	}
	for (const auto &block : blocks) {
		table.insert(block);
	}
	return 0;
}

/// Can a second scan of an extent end in a dedupe?  The extent has `nonzero`
/// blocks of data and `zero` blocks of zeros; the first scan found
/// duplicates for `dup` blocks, and `raced` more have turned up in the hash
/// table since.  bees dedupes part of an extent only if the bytes it frees
/// (deduped or zero) are at least the bytes it has to copy (the rest), see
/// "nuisance dedupes" in BeesContext::scan_one_extent_once.
inline
bool
bees_race_rescan_worthwhile(const size_t raced, const size_t dup, const size_t zero, const size_t nonzero)
{
	const size_t could_dedupe = std::min(nonzero, raced + dup);
	return nonzero - could_dedupe <= could_dedupe + zero;
}

#endif
