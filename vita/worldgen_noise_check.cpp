// Bit-identity check for NoiseGeneratorOctaves::generateNoiseOctavesMasked.
// From src/: clang++ -std=c++17 -O2 -w -I. -Inet/minecraft/src ../vita/worldgen_noise_check.cpp \
//   net/minecraft/src/NoiseGeneratorOctaves.cpp net/minecraft/src/NoiseGeneratorPerlin.cpp java/Random.cpp \
//   java/StrictMathCompat.cpp java/fdlibm/*.c* net/minecraft/src/MathHelper.cpp -o /tmp/noise && /tmp/noise
#include "net/minecraft/src/NoiseGeneratorOctaves.h"
#include "java/Random.h"
#include <cassert>
#include <cstdio>
#include <cstring>
// Masked 16-octave limit noise must equal the full one bit for bit wherever the mask is set.
int main()
{
    long long checked = 0, skipped = 0;
    for (long long seed : {0LL, 1LL, -4172144997902289642LL, 123456789LL, 8675309LL})
    {
        Random r(seed);
        NoiseGeneratorOctaves a(r, 16), b(r, 16), sel(r, 8);
        Random m(seed ^ 77);
        for (int c = 0; c < 400; ++c)
        {
            const int cx = m.nextInt(20000) - 10000, cz = m.nextInt(20000) - 10000;
            TerrainNoiseBuffer s, full, lazy;
            sel.generateNoiseOctaves(s, cx * 4, 0, cz * 4, 5, 17, 5, 684.412 / 80, 684.412 / 160, 684.412 / 80);
            std::vector<unsigned char> mask(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                const double blend = (s[i] / 10.0 + 1.0) / 2.0;
                mask[i] = c % 3 == 0 ? (i * 7 + c) % 3 == 0 : !(blend > 1.0); // random masks too
            }
            NoiseGeneratorOctaves &g = c & 1 ? a : b;
            g.generateNoiseOctaves(full, cx * 4, 0, cz * 4, 5, 17, 5, 684.412, 684.412, 684.412);
            g.generateNoiseOctavesMasked(lazy, cx * 4, 0, cz * 4, 5, 17, 5, 684.412, 684.412, 684.412, mask.data());
            for (size_t i = 0; i < full.size(); ++i)
            {
                if (!mask[i]) { ++skipped; continue; }
                assert(std::memcmp(&full[i], &lazy[i], sizeof(double)) == 0);
                ++checked;
            }
        }
    }
    std::printf("identical on %lld samples, skipped %lld (%.0f%%)\n", checked, skipped, 100.0 * skipped / (checked + skipped));
}
