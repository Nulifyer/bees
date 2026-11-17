#include "tests.h"

#include "../src/bees-extent-ref-order.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <vector>

using namespace std;

// Same ordering keys as BeesScanModeExtent::ExtentRef, without the parts that need btrfs
struct TestRef {
	uint64_t m_root;
	uint64_t m_inum;
	uint64_t m_offset;
	uint64_t m_length;
};

static
bool
less_ref(const TestRef &a, const TestRef &b)
{
	return bees_extent_ref_less(a, b);
}

static
void
test_irreflexive()
{
	const TestRef a = { .m_root = 5, .m_inum = 257, .m_offset = 4096, .m_length = 4096 };
	assert(!less_ref(a, a));
	// Length is not an ordering key, so these are equivalent
	const TestRef b = { .m_root = 5, .m_inum = 257, .m_offset = 4096, .m_length = 8192 };
	assert(!less_ref(a, b));
	assert(!less_ref(b, a));
}

static
void
test_inode_first()
{
	// Lower inode sorts first even with higher root and offset
	const TestRef a = { .m_root = 9, .m_inum = 1, .m_offset = 9, .m_length = 0 };
	const TestRef b = { .m_root = 1, .m_inum = 2, .m_offset = 1, .m_length = 0 };
	assert(less_ref(a, b));
	assert(!less_ref(b, a));
}

static
void
test_root_second()
{
	// Same inode: lower root sorts first even with higher offset
	const TestRef a = { .m_root = 1, .m_inum = 7, .m_offset = 9, .m_length = 0 };
	const TestRef b = { .m_root = 2, .m_inum = 7, .m_offset = 1, .m_length = 0 };
	assert(less_ref(a, b));
	assert(!less_ref(b, a));
}

static
void
test_offset_third()
{
	// Same inode and root: lower offset sorts first
	const TestRef a = { .m_root = 3, .m_inum = 7, .m_offset = 1, .m_length = 0 };
	const TestRef b = { .m_root = 3, .m_inum = 7, .m_offset = 2, .m_length = 0 };
	assert(less_ref(a, b));
	assert(!less_ref(b, a));
}

static
void
test_list_sort()
{
	// Every combination of 3 inodes, 3 roots, 3 offsets, inserted in a scrambled order
	vector<TestRef> all;
	for (uint64_t inum = 1; inum <= 3; ++inum) {
		for (uint64_t root = 1; root <= 3; ++root) {
			for (uint64_t offset = 1; offset <= 3; ++offset) {
				all.push_back(TestRef { .m_root = root, .m_inum = inum, .m_offset = offset * 4096, .m_length = 4096 });
			}
		}
	}
	list<TestRef> refs;
	for (size_t i = 0; i < all.size(); ++i) {
		// 10 is coprime with 27, so this visits every element once
		refs.push_back(all.at((i * 10) % all.size()));
	}
	// Add a duplicate to check ties
	refs.push_back(all.at(13));

	refs.sort(bees_extent_ref_less<TestRef>);

	assert(refs.size() == all.size() + 1);
	const TestRef *prev = nullptr;
	for (const auto &i : refs) {
		if (prev) {
			// Sorted: no element is less than the element before it
			assert(!less_ref(i, *prev));
			// Lexicographic on (inode, root, offset)
			assert(prev->m_inum < i.m_inum ||
				(prev->m_inum == i.m_inum && prev->m_root < i.m_root) ||
				(prev->m_inum == i.m_inum && prev->m_root == i.m_root && prev->m_offset <= i.m_offset));
		}
		prev = &i;
	}
	assert(refs.front().m_inum == 1 && refs.front().m_root == 1 && refs.front().m_offset == 4096);
	assert(refs.back().m_inum == 3 && refs.back().m_root == 3 && refs.back().m_offset == 3 * 4096);
}

int
main(int, char**)
{
	RUN_A_TEST(test_irreflexive());
	RUN_A_TEST(test_inode_first());
	RUN_A_TEST(test_root_second());
	RUN_A_TEST(test_offset_third());
	RUN_A_TEST(test_list_sort());

	exit(EXIT_SUCCESS);
}
