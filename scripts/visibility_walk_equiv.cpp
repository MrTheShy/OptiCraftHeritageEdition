// Checks that the Vita section-visibility walk (RenderGlobalPcLegacyVisibility.cpp,
// VITA_PLATFORM branch) marks exactly the sections the generic walk marks.
// Both walks are transcribed over a synthetic ring grid with random per-section
// face connectivity, frustum rejections and camera position.
//   c++ -std=c++17 -O2 scripts/visibility_walk_equiv.cpp -o /tmp/vis && /tmp/vis
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
constexpr int kOpp[6] = {1, 0, 3, 2, 5, 4};
constexpr int kDX[6] = {0, 0, 0, 0, -1, 1};
constexpr int kDY[6] = {-1, 1, 0, 0, 0, 0};
constexpr int kDZ[6] = {0, 0, -1, 1, 0, 0};

struct Grid
{
    int W, T, D, vStart;
    std::vector<bool> present;
    std::vector<int> sx, sy, sz;
    std::vector<std::uint8_t> faces[6]; // visible exits per entry face
    std::vector<bool> inFrustum;
    int camX, camY, camZ;

    int pmod(int v, int m) const { int r = v % m; return r < 0 ? r + m : r; }
    int indexAt(int x, int y, int z) const
    {
        const int ys = y - vStart;
        if (ys < 0 || ys >= T) return -1;
        const int i = (pmod(z, D) * T + ys) * W + pmod(x, W);
        if (!present[i] || sx[i] != x || sy[i] != y || sz[i] != z) return -1;
        return i;
    }
};

std::vector<bool> generic(const Grid &g)
{
    const int total = g.W * g.T * g.D;
    std::vector<bool> vis(total, false);
    const int cam = g.indexAt(g.camX, g.camY, g.camZ);
    if (cam < 0) { for (int i = 0; i < total; ++i) vis[i] = g.present[i]; return vis; }
    std::vector<std::uint8_t> mask(total, 0);
    std::vector<int> q;
    vis[cam] = true;
    auto enq = [&](int r, int f)
    {
        if ((g.sx[r] - g.camX) * kDX[f] < 0 || (g.sy[r] - g.camY) * kDY[f] < 0 || (g.sz[r] - g.camZ) * kDZ[f] < 0) return;
        const int n = g.indexAt(g.sx[r] + kDX[f], g.sy[r] + kDY[f], g.sz[r] + kDZ[f]);
        if (n < 0) return;
        const int e = kOpp[f];
        std::uint8_t &m = mask[n];
        if (m & (1 << e)) return;
        if (m & 0x80) return;
        if (m == 0 && !g.inFrustum[n]) { m = 0x80; return; }
        m |= 1 << e;
        vis[n] = true;
        q.push_back(n * 6 + e);
    };
    for (int f = 0; f < 6; ++f) enq(cam, f);
    for (size_t r = 0; r < q.size(); ++r)
    {
        const int idx = q[r] / 6, e = q[r] % 6;
        const std::uint8_t ex = g.faces[e][idx];
        for (int f = 0; f < 6; ++f) if (ex & (1 << f)) enq(idx, f);
    }
    return vis;
}

std::vector<bool> vita(const Grid &g)
{
    const int W = g.W, T = g.T, D = g.D, total = W * T * D, layer = W * T;
    struct Slot { int x, y, z; int slotX, slotY, slotZ; bool present; };
    std::vector<Slot> slots(total);
    std::vector<bool> vis(total, false);
    int i = 0;
    for (int zs = 0; zs < D; ++zs) for (int ys = 0; ys < T; ++ys) for (int xs = 0; xs < W; ++xs, ++i)
        slots[i] = {g.sx[i], g.sy[i], g.sz[i], xs, ys, zs, (bool)g.present[i]};
    const int cam = g.indexAt(g.camX, g.camY, g.camZ);
    if (cam < 0) { for (int k = 0; k < total; ++k) vis[k] = g.present[k]; return vis; }
    std::vector<std::uint8_t> mask(total, 0), processed(total, 0), exited(total, 0), queued(total, 0);
    std::vector<int> q;
    auto enq = [&](int from, int f)
    {
        const Slot &s = slots[from];
        if ((s.x - g.camX) * kDX[f] < 0 || (s.y - g.camY) * kDY[f] < 0 || (s.z - g.camZ) * kDZ[f] < 0) return;
        int n;
        switch (f)
        {
            case 0: if (s.slotY == 0) return; n = from - W; break;
            case 1: if (s.slotY + 1 >= T) return; n = from + W; break;
            case 2: n = s.slotZ == 0 ? from + (D - 1) * layer : from - layer; break;
            case 3: n = s.slotZ + 1 >= D ? from - (D - 1) * layer : from + layer; break;
            case 4: n = s.slotX == 0 ? from + (W - 1) : from - 1; break;
            default: n = s.slotX + 1 >= W ? from - (W - 1) : from + 1; break;
        }
        const Slot &t = slots[n];
        if (!t.present || t.x != s.x + kDX[f] || t.y != s.y + kDY[f] || t.z != s.z + kDZ[f]) return;
        const std::uint8_t bit = 1 << kOpp[f];
        std::uint8_t &m = mask[n];
        if (m & (bit | 0x80)) return;
        if (m == 0 && !g.inFrustum[n]) { m = 0x80; return; }
        m |= bit;
        vis[n] = true;
        if (!queued[n]) { queued[n] = 1; q.push_back(n); }
    };
    vis[cam] = true;
    for (int f = 0; f < 6; ++f) enq(cam, f);
    for (size_t r = 0; r < q.size(); ++r)
    {
        const int idx = q[r];
        queued[idx] = 0;
        const std::uint8_t ne = mask[idx] & 0x3f & ~processed[idx];
        processed[idx] |= ne;
        std::uint8_t ex = 0;
        for (int e = 0; e < 6; ++e) if (ne & (1 << e)) ex |= g.faces[e][idx];
        ex &= ~exited[idx];
        exited[idx] |= ex;
        for (int f = 0; f < 6; ++f) if (ex & (1 << f)) enq(idx, f);
    }
    return vis;
}
}

int main()
{
    std::mt19937 rng(12345);
    int checked = 0;
    for (int iter = 0; iter < 4000; ++iter)
    {
        Grid g;
        g.W = 3 + rng() % 15; g.T = 1 + rng() % 8; g.D = 3 + rng() % 15; g.vStart = (int)(rng() % 5) - 2;
        const int total = g.W * g.T * g.D;
        // Ring grid centred on a random camera, as markRenderersForNewPosition lays it out.
        g.camX = (int)(rng() % 200) - 100; g.camZ = (int)(rng() % 200) - 100;
        g.camY = g.vStart + (int)(rng() % (g.T + 2)) - 1;
        g.present.assign(total, true); g.sx.assign(total, 0); g.sy.assign(total, 0); g.sz.assign(total, 0);
        const int baseX = g.camX - g.W / 2, baseZ = g.camZ - g.D / 2;
        for (int x = baseX; x < baseX + g.W; ++x)
            for (int y = 0; y < g.T; ++y)
                for (int z = baseZ; z < baseZ + g.D; ++z)
                {
                    const int i = (g.pmod(z, g.D) * g.T + y) * g.W + g.pmod(x, g.W);
                    g.sx[i] = x; g.sy[i] = y + g.vStart; g.sz[i] = z;
                    g.present[i] = rng() % 50 != 0;
                }
        const int density = rng() % 4;
        for (int e = 0; e < 6; ++e)
        {
            g.faces[e].resize(total);
            for (int k = 0; k < total; ++k)
                g.faces[e][k] = density == 0 ? 0x3f : (std::uint8_t)(rng() & (density == 1 ? 0x3f : rng() & 0x3f));
        }
        g.inFrustum.resize(total);
        for (int k = 0; k < total; ++k) g.inFrustum[k] = rng() % (2 + iter % 5) != 0;
        if (generic(g) != vita(g))
        {
            std::printf("MISMATCH at iteration %d (W=%d T=%d D=%d)\n", iter, g.W, g.T, g.D);
            return 1;
        }
        ++checked;
    }
    std::printf("PASS: %d random grids, identical visible sets\n", checked);
    return 0;
}
