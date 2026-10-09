#pragma once

#include <cstdint>
#include <stdexcept>

#include "java/Arithmetic.h"
#include "java/Type.h"
#include "platform/PlatformConfig.h"

class Random
{
private:
	long_t seed;

public:
	Random();
	Random(long_t set_seed);

	void setSeed(long_t set_seed);
	// The 48-bit LCG state as is (no scrambling), to hand a generator's
	// exact position to another Random.
	long_t rawState() const { return seed; }
	void setRawState(long_t state) { seed = state; haveNextNextGaussian = false; }

	// The hot draws are inline (the Vita build has no LTO): a constant bound
	// then compiles to a multiply instead of a software divide and 3 calls.
	int_t next(int_t bits)
	{
		const ulong_t seedBits = static_cast<ulong_t>(seed);
#if PLATFORM_RANDOM_SPLIT_MULTIPLY
		// (seed * MUL + ADD) mod 2^48 without a 64x64 multiply. With
		// seed = sh*2^32 + sl (sh 16 bits) and MUL = 5*2^32 + ml, the product's
		// low 64 bits are sl*ml + (sh*ml + sl*5) * 2^32; only the low 16 bits of
		// that upper term survive the 48-bit mask, so it can wrap in 32 bits. The
		// one widening 32x32 multiply is a single instruction on these cores.
		const std::uint32_t sl = static_cast<std::uint32_t>(seedBits);
		const std::uint32_t sh = static_cast<std::uint32_t>(seedBits >> 32);
		const std::uint32_t ml = static_cast<std::uint32_t>(kMul);
		const std::uint32_t mh = static_cast<std::uint32_t>(kMul >> 32);
		const std::uint64_t low = static_cast<std::uint64_t>(sl) * ml;
		std::uint32_t hi = static_cast<std::uint32_t>(low >> 32) + sh * ml + sl * mh;
		const std::uint32_t lo = static_cast<std::uint32_t>(low) + static_cast<std::uint32_t>(kAdd);
		if (lo < static_cast<std::uint32_t>(low))
			++hi;
		seed = static_cast<long_t>((static_cast<ulong_t>(hi & 0xffffu) << 32) | lo);
#else
		seed = static_cast<long_t>((seedBits * kMul + kAdd) & kMask);
#endif
		return JavaArithmetic::intFromBits(static_cast<uint_t>(static_cast<ulong_t>(seed) >> (48 - bits)));
	}

	bool nextBoolean() { return next(1) == 1; }

	int_t nextInt() { return next(32); }
	int_t nextInt(int_t bound)
	{
		if (bound <= 0)
			throw std::invalid_argument("bound must be positive");

		const int_t m = bound - 1;
		int_t r = next(31);
		if ((bound & m) == 0)
		{
			// next(31) is non-negative, so this multiplication/shift is defined and
			// matches java.util.Random exactly. Both operands fit 32 bits, so the
			// widening form lets the compiler use one 32x32->64 multiply.
			return static_cast<int_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(bound)) *
			                           static_cast<std::uint32_t>(r)) >> 31);
		}

		for (;;)
		{
			const int_t u = r;
			r = u % bound;

			// Java int arithmetic wraps modulo 2^32.  The source expression
			// `u - r + m < 0` relies on that overflow for the rejection test; signed
			// overflow would be undefined in C++.  Reproduce the Java sign bit
			// explicitly in uint32_t.
			const std::uint32_t wrapped = static_cast<std::uint32_t>(u)
			                            - static_cast<std::uint32_t>(r)
			                            + static_cast<std::uint32_t>(m);
			if ((wrapped & 0x80000000u) == 0)
				return r;

			r = next(31);
		}
	}

	long_t nextLong();

	float nextFloat() { return next(24) / static_cast<float>(1LL << 24); }
	double nextDouble();
	float nextDoubleFloat();
	int_t nextInt5();
	// Advances exactly as nextInt5() does, without the division: a draw is
	// rejected (and redrawn) iff it is >= 2147483645.
	void skipInt5() { while (next(31) >= 2147483645) {} }

	// C++ does not preserve Java's left-to-right evaluation order for every
	// expression. These helpers sequence common multi-draw Java expressions.
	int_t nextIntDifference(int_t bound);
	int_t nextIntOffset(int_t base, int_t bound);
	int_t nextIntSum(int_t bound);
	float nextFloatDifference();
	float nextFloatProduct();
	float nextFloatProduct3();
	float nextFloatDifferenceTimesNextFloat();
	double nextDoubleDifference();

	// java.util.Random.nextGaussian() — polar Box-Muller, state-bearing.
	double nextGaussian();

private:
	static constexpr ulong_t kMul = 0x5DEECE66DULL;
	static constexpr ulong_t kAdd = 0xBULL;
	static constexpr ulong_t kMask = (1ULL << 48) - 1ULL;
	bool   haveNextNextGaussian = false;
	double nextNextGaussian = 0.0;
};
