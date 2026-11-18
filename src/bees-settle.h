#ifndef BEES_SETTLE_H
#define BEES_SETTLE_H

#include <cstddef>
#include <cstdint>
#include <deque>

// Settle delay (--settle-delay): don't scan data until it has been on disk,
// unchanged, for some time.
//
// Scanners work in transid windows: each crawl covers extents with
// min_transid <= generation <= max_transid, and when it finishes, the next
// window starts at the old max_transid and ends at "the current transid".
// With a settle delay, "the current transid" is replaced by the settled
// transid: the newest transid that bees saw committed at least the delay ago.
// Every extent of that generation or older was written before bees saw the
// commit, so it is at least that old.
//
// This class is the memory behind the settled transid: a short list of
// (transid, when bees first saw it committed).  It needs no btrfs, so
// test/settle.cc can test it.
//
// What follows from this design:
//  - An extent that is deleted or overwritten before it settles is gone from
//    the extent tree by the time a window reaches its generation, so it is
//    never mapped, opened or read.
//  - Nothing is skipped.  Windows still follow each other without a gap, and
//    the saved crawl state (beescrawl.dat) only ever holds windows that end at
//    a settled transid, so a restart or crash resumes behind the unsettled
//    data, not past it.
//  - The list is not saved.  After a restart bees waits one delay before it
//    opens a new window: it has no way to know how old the newest data is.
//    Windows loaded from the saved crawl state continue once the wait is over.
//  - Times are whatever monotonic clock the caller uses.  Seeing a commit late
//    (the transid is polled) makes the data wait longer, never shorter.
//  - Nothing here needs a wakeup while the filesystem is idle: once every
//    recorded transid has settled, next_change() says so.
class BeesSettleHistory {
	struct Entry {
		uint64_t	m_transid;
		double		m_seen;
	};
	/// Transids that have not settled yet, oldest first
	std::deque<Entry>	m_pending;
	/// Newest transid that has settled, or 0 if there is none yet
	uint64_t		m_settled = 0;
	/// Newest transid ever recorded
	uint64_t		m_latest = 0;

public:
	/// Upper bound on the list.  When it is full, the newest entry is
	/// replaced, which only makes its transid settle later.
	static const size_t c_max_pending = 1024;

	/// Record that transid was committed when the clock read 'now' seconds.
	/// Transids that are not newer than the newest recorded one are ignored.
	void
	update(const uint64_t transid, const double now)
	{
		if (transid <= m_latest) {
			return;
		}
		m_latest = transid;
		const Entry entry = { .m_transid = transid, .m_seen = now };
		if (m_pending.size() >= c_max_pending) {
			m_pending.back() = entry;
		} else {
			m_pending.push_back(entry);
		}
	}

	/// Newest transid that was recorded at least 'delay' seconds before 'now',
	/// or 0 if no transid has waited that long.  Never decreases.
	uint64_t
	settled(const double now, const double delay)
	{
		while (!m_pending.empty() && m_pending.front().m_seen + delay <= now) {
			m_settled = m_pending.front().m_transid;
			m_pending.pop_front();
		}
		return m_settled;
	}

	/// Seconds from 'now' until settled() returns a newer transid, or a
	/// negative number if it won't until update() records one.
	double
	next_change(const double now, const double delay) const
	{
		if (m_pending.empty()) {
			return -1;
		}
		const double wait = m_pending.front().m_seen + delay - now;
		return wait > 0 ? wait : 0;
	}

	/// Number of transids still waiting to settle
	size_t
	pending() const
	{
		return m_pending.size();
	}
};

#endif // BEES_SETTLE_H
