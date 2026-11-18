#include "tests.h"

#include "crucible/btrfs-tree.h"
#include "crucible/btrfs.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace crucible;
using namespace std;

// Build extent items the way the kernel lays them out:
// btrfs_extent_item, then inline refs, each a type byte followed by the ref.

using Bytes = vector<uint8_t>;

static
void
put(Bytes &v, const void *p, size_t len)
{
	const auto bp = static_cast<const uint8_t *>(p);
	v.insert(v.end(), bp, bp + len);
}

static
Bytes
extent_item(uint64_t refs, uint64_t generation, uint64_t flags)
{
	Bytes v;
	const btrfs_extent_item ei = {
		.refs = htole64(refs),
		.generation = htole64(generation),
		.flags = htole64(flags),
	};
	put(v, &ei, sizeof(ei));
	return v;
}

static
void
add_data_ref(Bytes &v, uint64_t root, uint64_t objectid, uint64_t offset, uint32_t count)
{
	const uint8_t type = BTRFS_EXTENT_DATA_REF_KEY;
	put(v, &type, 1);
	const btrfs_extent_data_ref dr = {
		.root = htole64(root),
		.objectid = htole64(objectid),
		.offset = htole64(offset),
		.count = htole32(count),
	};
	put(v, &dr, sizeof(dr));
}

static
void
add_shared_ref(Bytes &v, uint64_t parent, uint32_t count)
{
	const uint8_t type = BTRFS_SHARED_DATA_REF_KEY;
	put(v, &type, 1);
	const uint64_t le_parent = htole64(parent);
	put(v, &le_parent, sizeof(le_parent));
	const uint32_t le_count = htole32(count);
	put(v, &le_count, sizeof(le_count));
}

static
BtrfsTreeItem
make_item(Bytes v, uint8_t type = BTRFS_EXTENT_ITEM_KEY)
{
	BtrfsIoctlSearchHeader hdr;
	hdr.objectid = 0x100000;
	hdr.offset = 4096;
	hdr.type = type;
	hdr.transid = 7;
	hdr.len = v.size();
	hdr.m_data = ByteVector(v.data(), v.data() + v.size());
	return BtrfsTreeItem(hdr);
}

static
void
test_one_ref()
{
	// A freshly written extent: one file, one ref
	auto v = extent_item(1, 7, BTRFS_EXTENT_FLAG_DATA);
	add_data_ref(v, 257, 1234, 8192, 1);
	const auto refs = make_item(v).extent_data_refs();
	assert(refs.size() == 1);
	assert(refs.at(0).m_root == 257);
	assert(refs.at(0).m_objectid == 1234);
	assert(refs.at(0).m_offset == 8192);
	assert(refs.at(0).m_count == 1);
}

static
void
test_two_files()
{
	// Reflinked into a second file, both refs inline
	auto v = extent_item(3, 7, BTRFS_EXTENT_FLAG_DATA);
	add_data_ref(v, 257, 1234, 0, 2);
	add_data_ref(v, 5, 99, 4096, 1);
	const auto refs = make_item(v).extent_data_refs();
	assert(refs.size() == 2);
	assert(refs.at(0).m_objectid == 1234 && refs.at(0).m_count == 2);
	assert(refs.at(1).m_root == 5 && refs.at(1).m_objectid == 99);
}

static
void
test_refs_elsewhere()
{
	// The item says 5 refs but only 2 are inline: the rest are in separate items,
	// so the item can't name every file
	auto v = extent_item(5, 7, BTRFS_EXTENT_FLAG_DATA);
	add_data_ref(v, 257, 1234, 0, 2);
	assert(make_item(v).extent_data_refs().empty());

	// No inline refs at all
	const auto bare = extent_item(1, 7, BTRFS_EXTENT_FLAG_DATA);
	assert(make_item(bare).extent_data_refs().empty());
}

static
void
test_shared_ref()
{
	// A shared ref (after a snapshot) names a metadata block, not a file
	auto v = extent_item(2, 7, BTRFS_EXTENT_FLAG_DATA);
	add_data_ref(v, 257, 1234, 0, 1);
	add_shared_ref(v, 0x4000, 1);
	assert(make_item(v).extent_data_refs().empty());
}

static
void
test_not_data()
{
	// Metadata extents have no file refs
	auto v = extent_item(1, 7, BTRFS_EXTENT_FLAG_TREE_BLOCK);
	assert(make_item(v).extent_data_refs().empty());

	// Wrong item type throws
	bool threw = false;
	try {
		make_item(extent_item(1, 7, BTRFS_EXTENT_FLAG_DATA), BTRFS_EXTENT_DATA_KEY).extent_data_refs();
	} catch (const invalid_argument &) {
		threw = true;
	}
	assert(threw);
}

static
void
test_truncated()
{
	// A ref cut short must throw, not read past the end
	auto v = extent_item(1, 7, BTRFS_EXTENT_FLAG_DATA);
	add_data_ref(v, 257, 1234, 0, 1);
	v.erase(v.end() - 3, v.end());
	bool threw = false;
	try {
		make_item(v).extent_data_refs();
	} catch (const out_of_range &) {
		threw = true;
	}
	assert(threw);
}

int
main(int, char**)
{
	RUN_A_TEST(test_one_ref());
	RUN_A_TEST(test_two_files());
	RUN_A_TEST(test_refs_elsewhere());
	RUN_A_TEST(test_shared_ref());
	RUN_A_TEST(test_not_data());
	RUN_A_TEST(test_truncated());

	exit(EXIT_SUCCESS);
}
