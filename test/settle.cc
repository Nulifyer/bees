#include "tests.h"

#include "../src/bees-settle.h"

#include <cassert>
#include <cstdlib>

using namespace std;

static
void
test_empty()
{
	BeesSettleHistory h;
	// Nothing recorded: nothing is settled and nothing will be
	assert(h.settled(1000, 60) == 0);
	assert(h.next_change(1000, 60) < 0);
	assert(h.pending() == 0);
}

static
void
test_one_transid()
{
	BeesSettleHistory h;
	h.update(100, 1000);
	// Not before the delay has passed
	assert(h.settled(1000, 60) == 0);
	assert(h.settled(1059.9, 60) == 0);
	assert(h.next_change(1000, 60) == 60);
	assert(h.next_change(1050, 60) == 10);
	// Exactly at the delay, and from then on
	assert(h.settled(1060, 60) == 100);
	assert(h.settled(5000, 60) == 100);
	// Nothing left to wait for: an idle filesystem needs no wakeup
	assert(h.next_change(5000, 60) < 0);
	assert(h.pending() == 0);
}

static
void
test_sequence()
{
	BeesSettleHistory h;
	h.update(100, 1000);
	h.update(101, 1030);
	h.update(105, 1060);
	assert(h.pending() == 3);
	assert(h.settled(1059, 60) == 0);
	assert(h.settled(1060, 60) == 100);
	assert(h.next_change(1060, 60) == 30);
	assert(h.settled(1089, 60) == 100);
	assert(h.settled(1090, 60) == 101);
	// A long sleep settles everything at once, newest wins
	assert(h.settled(9999, 60) == 105);
	assert(h.next_change(9999, 60) < 0);
}

static
void
test_never_decreases()
{
	BeesSettleHistory h;
	h.update(100, 1000);
	assert(h.settled(1060, 60) == 100);
	// Same or older transid again (polling an idle filesystem): ignored
	h.update(100, 1100);
	h.update(99, 1100);
	assert(h.pending() == 0);
	assert(h.settled(1100, 60) == 100);
	// A clock that reads earlier than before does not un-settle anything
	assert(h.settled(0, 60) == 100);
	// A new transid does not settle early because an old one already did
	h.update(101, 1100);
	assert(h.settled(1159, 60) == 100);
	assert(h.settled(1160, 60) == 101);
}

static
void
test_late_observation()
{
	// The transid is polled, so a commit can be seen late.
	// Then it settles late, never early.
	BeesSettleHistory h;
	h.update(100, 1000);
	h.update(101, 1500);
	assert(h.settled(1559, 60) == 100);
	assert(h.settled(1560, 60) == 101);
}

static
void
test_zero_wait()
{
	// An entry that is due but not collected yet: next_change is "now", not negative
	BeesSettleHistory h;
	h.update(100, 1000);
	assert(h.next_change(2000, 60) == 0);
	assert(h.settled(2000, 60) == 100);
}

static
void
test_bounded()
{
	// A transid per second for much longer than the list holds, with a long delay
	BeesSettleHistory h;
	const double delay = 1e6;
	const size_t count = BeesSettleHistory::c_max_pending * 4;
	for (size_t i = 1; i <= count; ++i) {
		h.update(i, i);
		assert(h.pending() <= BeesSettleHistory::c_max_pending);
	}
	// Nothing settled early
	assert(h.settled(count, delay) == 0);
	// The entries that fit keep their own times
	assert(h.settled(1 + delay, delay) == 1);
	assert(h.settled(100 + delay, delay) == 100);
	// The newest transid is in the list and settles a full delay after it was seen
	assert(h.settled(count - 1 + delay, delay) < count);
	assert(h.settled(count + delay, delay) == count);
	assert(h.pending() == 0);
}

int
main(int, char**)
{
	RUN_A_TEST(test_empty());
	RUN_A_TEST(test_one_transid());
	RUN_A_TEST(test_sequence());
	RUN_A_TEST(test_never_decreases());
	RUN_A_TEST(test_late_observation());
	RUN_A_TEST(test_zero_wait());
	RUN_A_TEST(test_bounded());

	exit(EXIT_SUCCESS);
}
