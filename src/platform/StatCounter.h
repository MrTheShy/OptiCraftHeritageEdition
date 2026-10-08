#pragma once

#include <atomic>

// 64-bit counter for timing statistics that only feed log lines.
// The Wii and the PS2 have no 64-bit atomic instructions and no libatomic,
// and neither runs worker threads, so there it is a plain integer.
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM)
class StatCounter
{
public:
	StatCounter(long long initial = 0) : value(initial) {}
	long long load() const { return value; }
	long long exchange(long long next) { const long long old = value; value = next; return old; }
	bool compare_exchange_strong(long long &expected, long long next)
	{
		if (value != expected)
		{
			expected = value;
			return false;
		}
		value = next;
		return true;
	}
	StatCounter &operator=(long long next) { value = next; return *this; }
	StatCounter &operator+=(long long delta) { value += delta; return *this; }
	StatCounter &operator++() { ++value; return *this; }
	long long operator++(int) { return value++; }
	operator long long() const { return value; }

private:
	long long value;
};
#else
using StatCounter = std::atomic<long long>;
#endif
