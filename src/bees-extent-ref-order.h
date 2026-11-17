#ifndef BEES_EXTENT_REF_ORDER_H
#define BEES_EXTENT_REF_ORDER_H

#include <tuple>

// Strict weak ordering for extent refs, used by BeesScanModeExtent to sort
// the refs of one extent before scanning them.  Any type with m_inum,
// m_root and m_offset members can be ordered this way.
//
// Sort by inode first to avoid thrashing inode locks.
// Do all the reflinks on one subvol at a time so that we don't have a lot of open trees.
// Logically sequential read order because that's what both scan_one_extent and btrfs are optimized for.
template <class T>
bool
bees_extent_ref_less(const T &a, const T &b)
{
	return std::tie(a.m_inum, a.m_root, a.m_offset) < std::tie(b.m_inum, b.m_root, b.m_offset);
}

#endif // BEES_EXTENT_REF_ORDER_H
