#include "pc/render/VitaTerrainBatch.h"

#if PLATFORM_TERRAIN_VBO_BATCH

#include <glad/glad.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <cmath>

#include "net/minecraft/src/Config.h"
#include "platform/PlatformCompat.h"
#include "platform/Log.h"
#include "platform/Profiler.h"
#include "platform/RenderAPI.h"

extern "C" unsigned g_vitaPerfDraws;
extern "C" unsigned g_vitaPerfBufferBytes;

namespace
{
// Fixed-function layout, also the intermediate form of every captured vertex.
struct Vertex
{
    std::int16_t x, y, z, pad;
    float u, v;
    std::int32_t color;
    std::int32_t brightness; // two s16 lightmap coordinates: block, sky
};
static_assert(sizeof(Vertex) == 24, "terrain VBO vertex must stay 24 bytes");

// Shader layout.
struct ShaderVertex
{
    std::int16_t x, y, z;
    std::uint8_t lightBlock, lightSky;
    std::uint16_t u, v;
    std::int32_t color;
};
static_assert(sizeof(ShaderVertex) == 16, "terrain shader vertex must stay 16 bytes");

// Positions are section-local blocks stored as s16. 1/1024 is a power of two,
// so the scale folded into the matrix is exact, and it resolves the 1/16 (and
// 0.001) offsets blocks use. Covers -32..+32 blocks around the origin.
constexpr float kFixedScale = 1024.0f;
constexpr float kFixedInvScale = 1.0f / 1024.0f;

// vitaGL builds glDrawArrays indices in a MAX_IDX_NUMBER (0xC000) table; a
// GL_QUADS draw uses 6 of them per 4 vertices.
constexpr int kMaxQuadDrawVertices = (0xC000 / 6) * 4;

std::vector<Vertex> g_scratch;
std::vector<Vertex> g_solid;
std::vector<Vertex> g_cutout;
std::vector<ShaderVertex> g_shaderVertices;

// Bit i set: atlas tile i has a texel with alpha < 255. Until terrain.png is
// seen every tile counts as transparent, which only costs the split.
std::uint64_t g_tileAlpha[4] = {~0ull, ~0ull, ~0ull, ~0ull};

bool quantize(float value, std::int16_t &out)
{
    const float scaled = value * kFixedScale;
    if (!(scaled > -32767.0f && scaled < 32767.0f)) // also rejects NaN
        return false;
    out = static_cast<std::int16_t>(static_cast<int>(scaled + (scaled >= 0.0f ? 0.5f : -0.5f)));
    return true;
}

bool appendCompact(const RenderCapturedMesh &mesh)
{
    if (mesh.empty())
        return true;
    if (mesh.stride != 24 || mesh.primitive != RenderPrimitive::Triangles || !mesh.positionShort ||
        !mesh.hasTexture || mesh.texCoordOffset != 8 || !mesh.hasColor || mesh.colorOffset != 16 ||
        !mesh.hasBrightness || mesh.brightnessOffset != 20 || mesh.vertexCount % 3 != 0)
        return false;

    const unsigned char *src = reinterpret_cast<const unsigned char *>(mesh.raw.data());
    for (int i = 0; i < mesh.vertexCount; ++i, src += 24)
    {
        Vertex vertex;
        std::memcpy(&vertex, src, sizeof(vertex));
        const int x = vertex.x * 1024;
        const int y = vertex.y * 1024;
        const int z = vertex.z * 1024;
        if (x < -32767 || x > 32767 || y < -32767 || y > 32767 || z < -32767 || z > 32767)
            return false;
        vertex.x = static_cast<std::int16_t>(x);
        vertex.y = static_cast<std::int16_t>(y);
        vertex.z = static_cast<std::int16_t>(z);
        vertex.pad = 0;
        g_scratch.push_back(vertex);
    }
    return true;
}

bool appendBase(const RenderCapturedMesh &mesh)
{
    if (mesh.empty())
        return true;
    // Normals are dropped: terrain draws with lighting off, where they are unused.
    if (mesh.stride != 32 || mesh.primitive != RenderPrimitive::Triangles || mesh.positionShort ||
        !mesh.hasTexture || mesh.texCoordOffset != 12 || !mesh.hasColor || mesh.colorOffset != 20 ||
        !mesh.hasBrightness || mesh.brightnessOffset != 28 || mesh.vertexCount % 3 != 0)
        return false;

    const unsigned char *src = reinterpret_cast<const unsigned char *>(mesh.raw.data());
    for (int i = 0; i < mesh.vertexCount; ++i, src += 32)
    {
        float position[3];
        std::memcpy(position, src, sizeof(position));
        Vertex vertex;
        if (!quantize(position[0], vertex.x) || !quantize(position[1], vertex.y) ||
            !quantize(position[2], vertex.z))
            return false;
        vertex.pad = 0;
        std::memcpy(&vertex.u, src + 12, 8);
        std::memcpy(&vertex.color, src + 20, 4);
        std::memcpy(&vertex.brightness, src + 28, 4);
        g_scratch.push_back(vertex);
    }
    return true;
}

bool toShaderVertex(const Vertex &in, ShaderVertex &out)
{
    // The atlas is sampled with normalised u16; anything outside [0, 1] would
    // have relied on wrapping.
    if (!(in.u >= 0.0f && in.u <= 1.0f && in.v >= 0.0f && in.v <= 1.0f))
        return false;
    out.x = in.x;
    out.y = in.y;
    out.z = in.z;
    const int block = static_cast<std::int16_t>(in.brightness & 0xffff);
    const int sky = static_cast<std::int16_t>((in.brightness >> 16) & 0xffff);
    out.lightBlock = static_cast<std::uint8_t>(std::min(std::max(block, 0), 255));
    out.lightSky = static_cast<std::uint8_t>(std::min(std::max(sky, 0), 255));
    out.u = static_cast<std::uint16_t>(in.u * 65535.0f + 0.5f);
    out.v = static_cast<std::uint16_t>(in.v * 65535.0f + 0.5f);
    out.color = in.color;
    return true;
}

bool tileHasAlpha(float u, float v)
{
    const int column = std::min(15, std::max(0, static_cast<int>(u * 16.0f)));
    const int row = std::min(15, std::max(0, static_cast<int>(v * 16.0f)));
    const int tile = row * 16 + column;
    return (g_tileAlpha[tile >> 6] >> (tile & 63)) & 1u;
}

// The tessellator writes quad (a,b,c,d) as triangles (a,b,c)(a,c,d); undo that
// so the quad is 4 vertices. A lone triangle becomes the degenerate quad
// (a,b,c,c). Pass 0 quads go to g_solid or g_cutout by their atlas tile;
// pass 1 (translucent) is all g_cutout.
void buildQuads(int pass)
{
    g_solid.clear();
    g_cutout.clear();
    const std::size_t count = g_scratch.size();
    std::size_t i = 0;
    while (i + 3 <= count)
    {
        const Vertex *v = &g_scratch[i];
        const Vertex *quad[4];
        if (i + 6 <= count && std::memcmp(&v[3], &v[0], sizeof(Vertex)) == 0 &&
            std::memcmp(&v[4], &v[2], sizeof(Vertex)) == 0)
        {
            quad[0] = &v[0]; quad[1] = &v[1]; quad[2] = &v[2]; quad[3] = &v[5];
            i += 6;
        }
        else
        {
            quad[0] = &v[0]; quad[1] = &v[1]; quad[2] = &v[2]; quad[3] = &v[2];
            i += 3;
        }
        const float centreU = (quad[0]->u + quad[1]->u + quad[2]->u + quad[3]->u) * 0.25f;
        const float centreV = (quad[0]->v + quad[1]->v + quad[2]->v + quad[3]->v) * 0.25f;
        std::vector<Vertex> &target = (pass == 0 && !tileHasAlpha(centreU, centreV)) ? g_solid : g_cutout;
        for (const Vertex *corner : quad)
            target.push_back(*corner);
    }
}

// The shader path straight from the captured meshes: the quads buildQuads
// would make (solid, then cutout), converted by toShaderVertex, without the
// 24-byte intermediate copies. The convert was bound by memory traffic on Vita
// (~0.7 us a vertex, 21 ms for a 29k-vertex forest section). False when a
// vertex does not fit the shader layout: the caller takes the old path, which
// then fails the same way and uses fixed function.
// The output buffers only grow (no per-publish zero fill).
std::vector<ShaderVertex> g_shaderCutout;

bool compactFormatOk(const RenderCapturedMesh &mesh)
{
    return mesh.stride == 24 && mesh.primitive == RenderPrimitive::Triangles && mesh.positionShort &&
        mesh.hasTexture && mesh.texCoordOffset == 8 && mesh.hasColor && mesh.colorOffset == 16 &&
        mesh.hasBrightness && mesh.brightnessOffset == 20 && mesh.vertexCount % 3 == 0;
}

bool baseFormatOk(const RenderCapturedMesh &mesh)
{
    return mesh.stride == 32 && mesh.primitive == RenderPrimitive::Triangles && !mesh.positionShort &&
        mesh.hasTexture && mesh.texCoordOffset == 12 && mesh.hasColor && mesh.colorOffset == 20 &&
        mesh.hasBrightness && mesh.brightnessOffset == 28 && mesh.vertexCount % 3 == 0;
}

// Vertex i of a captured mesh as appendCompact / appendBase would store it.
// Inlined: a call per vertex plus out-of-line memcmp was a third of pubConvert.
__attribute__((always_inline)) inline bool readCompact(const unsigned char *raw, int i, Vertex &vertex)
{
    std::memcpy(&vertex, raw + i * 24, sizeof(vertex));
    const int x = vertex.x * 1024;
    const int y = vertex.y * 1024;
    const int z = vertex.z * 1024;
    if (x < -32767 || x > 32767 || y < -32767 || y > 32767 || z < -32767 || z > 32767)
        return false;
    vertex.x = static_cast<std::int16_t>(x);
    vertex.y = static_cast<std::int16_t>(y);
    vertex.z = static_cast<std::int16_t>(z);
    vertex.pad = 0;
    return true;
}

__attribute__((always_inline)) inline bool readBase(const unsigned char *raw, int i, Vertex &vertex)
{
    const unsigned char *src = raw + i * 32;
    float position[3];
    std::memcpy(position, src, sizeof(position));
    if (!quantize(position[0], vertex.x) || !quantize(position[1], vertex.y) || !quantize(position[2], vertex.z))
        return false;
    vertex.pad = 0;
    std::memcpy(&vertex.u, src + 12, 8);
    std::memcpy(&vertex.color, src + 20, 4);
    std::memcpy(&vertex.brightness, src + 28, 4);
    return true;
}

// Raw vertices a and b of a stride-byte mesh are byte-identical.
__attribute__((always_inline)) inline bool sameRaw(const unsigned char *raw, int a, int b, int stride)
{
    const std::uint32_t *pa = reinterpret_cast<const std::uint32_t *>(raw + a * stride);
    const std::uint32_t *pb = reinterpret_cast<const std::uint32_t *>(raw + b * stride);
    std::uint32_t diff = 0;
    for (int w = 0; w < stride / 4; ++w)
        diff |= pa[w] ^ pb[w];
    return diff == 0;
}

// The tessellator writes quad (a,b,c,d) as (a,b,c)(a,c,d) with the repeats
// as byte copies, so the raw words decide the quad before any conversion
// (the converted form is a function of the raw bytes: raw-equal gives
// converted-equal, the test buildQuads makes). Only the 4 corners are read.
template <int Stride, bool (*Read)(const unsigned char *, int, Vertex &)>
bool emitShaderQuads(const unsigned char *raw, int count, int pass,
    ShaderVertex *solid, std::size_t &solidN, ShaderVertex *cutout, std::size_t &cutoutN)
{
    Vertex v[4];
    int i = 0;
    while (i + 3 <= count)
    {
        if (!Read(raw, i, v[0]) || !Read(raw, i + 1, v[1]) || !Read(raw, i + 2, v[2]))
            return false;
        if (i + 6 <= count && sameRaw(raw, i + 3, i, Stride) && sameRaw(raw, i + 4, i + 2, Stride))
        {
            if (!Read(raw, i + 5, v[3]))
                return false;
            i += 6;
        }
        else
        {
            v[3] = v[2];
            i += 3;
        }
        const float centreU = (v[0].u + v[1].u + v[2].u + v[3].u) * 0.25f;
        const float centreV = (v[0].v + v[1].v + v[2].v + v[3].v) * 0.25f;
        const bool isSolid = pass == 0 && !tileHasAlpha(centreU, centreV);
        ShaderVertex *out = isSolid ? solid + solidN : cutout + cutoutN;
        for (int c = 0; c < 4; ++c)
            if (!toShaderVertex(v[c], out[c]))
                return false;
        (isSolid ? solidN : cutoutN) += 4;
    }
    return true;
}

// Ranges with at least this many vertices are grouped by face direction
// (a draw per run of kept groups: small sections are cheaper as one draw).
constexpr std::size_t kGroupMinVertices = 1536;
constexpr int kGroupStorageOrder[kVitaFaceGroups] = {1, 2, 5, 6, 4, 3, 0}; // +X -Y +Z other -Z +Y -X
// Direction code: 0 -X, 1 +X, 2 -Y, 3 +Y, 4 -Z, 5 +Z, 6 other.
std::vector<ShaderVertex> g_groupScratch;
std::vector<std::uint8_t> g_groupCodes;

int quadDirection(const ShaderVertex *q)
{
    const int bx = q[1].x - q[0].x, by = q[1].y - q[0].y, bz = q[1].z - q[0].z;
    const int cx = q[2].x - q[0].x, cy = q[2].y - q[0].y, cz = q[2].z - q[0].z;
    // Geometric normal (b-a)x(c-a); GL_CCW front faces are the ones it points
    // at, for a camera transform that does not mirror.
    if (q[0].x == q[1].x && q[1].x == q[2].x && q[2].x == q[3].x)
    {
        const int nx = by * cz - bz * cy;
        return nx > 0 ? 1 : nx < 0 ? 0 : 6;
    }
    if (q[0].y == q[1].y && q[1].y == q[2].y && q[2].y == q[3].y)
    {
        const int ny = bz * cx - bx * cz;
        return ny > 0 ? 3 : ny < 0 ? 2 : 6;
    }
    if (q[0].z == q[1].z && q[1].z == q[2].z && q[2].z == q[3].z)
    {
        const int nz = bx * cy - by * cx;
        return nz > 0 ? 5 : nz < 0 ? 4 : 6;
    }
    return 6;
}

// Reorders the quads of [vertices, vertices + count) by direction (stable:
// same-group quads keep their order), filling groups[range].
void groupRange(ShaderVertex *vertices, std::size_t count, VitaTerrainFaceGroups &groups, int range)
{
    const std::size_t quads = count / 4;
    if (g_groupCodes.size() < quads)
        g_groupCodes.resize(quads);
    if (g_groupScratch.size() < count)
        g_groupScratch.resize(count);
    std::size_t perCode[kVitaFaceGroups] = {};
    std::int16_t planeMin[kVitaFaceGroups], planeMax[kVitaFaceGroups];
    for (int c = 0; c < kVitaFaceGroups; ++c)
    {
        planeMin[c] = 32767;
        planeMax[c] = -32768;
    }
    for (std::size_t q = 0; q < quads; ++q)
    {
        const ShaderVertex *v = vertices + q * 4;
        const int code = quadDirection(v);
        g_groupCodes[q] = static_cast<std::uint8_t>(code);
        ++perCode[code];
        const std::int16_t plane = code < 2 ? v[0].x : code < 4 ? v[0].y : v[0].z;
        if (code < 6)
        {
            planeMin[code] = std::min(planeMin[code], plane);
            planeMax[code] = std::max(planeMax[code], plane);
        }
    }
    std::size_t writeAt[kVitaFaceGroups];
    std::size_t cursor = 0;
    for (int slot = 0; slot < kVitaFaceGroups; ++slot)
    {
        const int code = kGroupStorageOrder[slot];
        writeAt[code] = cursor;
        cursor += perCode[code] * 4;
        groups.end[range][slot] = static_cast<int>(cursor);
        groups.planeMin[range][slot] = planeMin[code];
        groups.planeMax[range][slot] = planeMax[code];
    }
    for (std::size_t q = 0; q < quads; ++q)
    {
        const int code = g_groupCodes[q];
        std::memcpy(g_groupScratch.data() + writeAt[code], vertices + q * 4, 4 * sizeof(ShaderVertex));
        writeAt[code] += 4;
    }
    std::memcpy(vertices, g_groupScratch.data(), count * sizeof(ShaderVertex));
}

bool buildShaderDirect(const RenderCapturedMesh &compact, const RenderCapturedMesh &base, int pass,
    std::size_t &solidCount, std::size_t &total)
{
    const bool hasCompact = !compact.empty();
    const bool hasBase = !base.empty();
    if ((hasCompact && !compactFormatOk(compact)) || (hasBase && !baseFormatOk(base)))
        return false;
    const int compactCount = hasCompact ? compact.vertexCount : 0;
    const int baseCount = hasBase ? base.vertexCount : 0;
    const std::size_t bound = static_cast<std::size_t>(compactCount / 3 + baseCount / 3) * 4 + 8;
    if (g_shaderVertices.size() < bound)
        g_shaderVertices.resize(bound);
    if (g_shaderCutout.size() < bound)
        g_shaderCutout.resize(bound);
    ShaderVertex *solid = g_shaderVertices.data();
    ShaderVertex *cutout = g_shaderCutout.data();
    std::size_t solidN = 0, cutoutN = 0;
    if (hasCompact && !emitShaderQuads<24, readCompact>(reinterpret_cast<const unsigned char *>(compact.raw.data()),
            compactCount, pass, solid, solidN, cutout, cutoutN))
        return false;
    if (hasBase && !emitShaderQuads<32, readBase>(reinterpret_cast<const unsigned char *>(base.raw.data()),
            baseCount, pass, solid, solidN, cutout, cutoutN))
        return false;
    std::memcpy(solid + solidN, cutout, cutoutN * sizeof(ShaderVertex));
    solidCount = solidN;
    total = solidN + cutoutN;
    return true;
}

const void *offsetPointer(int offset)
{
    return reinterpret_cast<const void *>(static_cast<std::uintptr_t>(offset));
}

// --- Cg programs ------------------------------------------------------------

const char *kVertexSource = R"(
void main(
	float3 aPosition,
	float2 aLight,
	float2 aTexCoord,
	float4 aColor,
	uniform float4x4 uMvp,
	uniform float3 uOrigin,
	uniform float4 uHandLight,
	uniform float4 uWave,
	uniform float3 uCameraMod,
	float4 out vPosition : POSITION,
	float2 out vTexCoord : TEXCOORD0,
	float2 out vLight : TEXCOORD1,
	float4 out vColor : COLOR)
{
	float3 world = aPosition * (1.0f / 1024.0f) + uOrigin;
	// Shader Effects: Enhanced (uWave.x). Foliage sways and water ripples by
	// world position (camera position modulo 16 in uCameraMod, so the phase is
	// continuous) and time (uWave.y, wraps at 2 pi): vertices shared by two
	// blocks move together, nothing opens up. The block is known by its tile.
	if (uWave.x > 0.5f)
	{
		// Faces end 0.01 texel short of the tile's far edge (15.99/16 of it);
		// a bias of 0.001 tile pushed those corners into the next tile, so a
		// grass side corner read as tall grass and waved. 0.0003 still covers
		// the u16 rounding (+-0.00012 tile) at the near edge.
		float2 tile = floor(aTexCoord * 16.0f + 0.0003f);
		float id = tile.y * 16.0f + tile.x;
		float3 w = world + uCameraMod;
		float phase = (w.x * 2.0f + w.z) * 0.3926991f + uWave.y;
		float amp = 0.0f;
		if (id == 52.0f || id == 53.0f || id == 132.0f || id == 133.0f || id == 196.0f || id == 197.0f)
			amp = 0.035f;
		else if (id == 39.0f || id == 56.0f || id == 55.0f || id == 12.0f || id == 13.0f || id == 15.0f ||
			id == 63.0f || id == 79.0f || id == 73.0f || (id >= 88.0f && id <= 95.0f))
			amp = 0.09f * (1.0f - frac(aTexCoord.y * 16.0f));
		// Whole multiples of phase only: time wraps at 2 pi and the camera
		// term at 16 blocks, and a fractional one (it was 0.8 and 1.3) made
		// every plant snap back each wrap, ~4 s.
		world.x += amp * sin(phase);
		world.z += amp * cos(phase * 2.0f);
		float surface = frac(w.y);
		if ((id == 205.0f || id == 206.0f || id == 207.0f || id == 222.0f || id == 223.0f) &&
			surface > 0.05f && surface < 0.98f)
			world.y += 0.04f * sin(phase * 2.0f + w.z * 0.7853982f);
	}
	vPosition = mul(uMvp, float4(world, 1.0f));
	vTexCoord = aTexCoord;
	// Held light source (uHandLight: position from the eye, level in w): block
	// light falls one level per block from it, as a placed one would, and the
	// brighter of the two wins. Per vertex, on the GPU; the world's light is
	// not touched.
	float2 light = aLight;
	light.x = max(light.x, max(uHandLight.w - length(world - uHandLight.xyz), 0.0f) * 16.0f);
	// The lightmap texture matrix vanilla sets: scale 1/256, translate 8.
	vLight = (light + 8.0f) * (1.0f / 256.0f);
	vColor = aColor;
}
)";

// Same arithmetic as vitaGL's fixed-function fragment program (MODULATE on both
// units, fog distance from WPOS; the alpha test comes last, see below), so
// nothing changes on screen. The solid variant has no discard: that is what keeps the SGX's
// hidden-surface removal working for the bulk of the terrain.
const char *kFragmentSource = R"(
half4 main(
	float2 vTexCoord : TEXCOORD0,
	float2 vLight : TEXCOORD1,
	half4 vColor : COLOR,
	float4 coords : WPOS,
	uniform sampler2D uAtlas,
	uniform sampler2D uLightmap,
	uniform float4 uFogColor,
	uniform float4 uFog)
{
	// Colour work in half precision: the SGX runs it on its faster
	// medium-precision path (8-bit texels and colours lose nothing). Texture
	// coordinates and the fog distance stay float.
	half4 c = (half4)tex2D(uAtlas, vTexCoord) * vColor * (half4)tex2D(uLightmap, vLight);
	// uFog: x mode (0 linear, 1 exp, 2 exp2), y far, z 1/(far-near), w density
	float d = coords.z / coords.w;
	float k = uFog.w * d;
	float e = uFog.x < 1.5f ? k : k * k;
	float f = uFog.x < 0.5f ? (uFog.y - d) * uFog.z : exp(-e * 1.442695f);
	c.rgb = lerp((half3)uFogColor.rgb, c.rgb, (half)clamp(f, 0.0f, 1.0f));
	// The alpha test is the last statement, after fog: placed before it (as in
	// vitaGL's own program) the SGX drops pixels in a 2x2 pattern and keeps
	// alpha-0 texels.
	ALPHA_TEST
	return c;
}
)";

struct Program
{
    GLuint id = 0;
    GLint mvp = -1, origin = -1, fogColor = -1, fog = -1, handLight = -1, wave = -1, cameraMod = -1;
    GLint position = -1, light = -1, texCoord = -1, color = -1;
};

// Program variants, built on first use. Vertex: bit 0 wave (Enhanced
// effects), bit 1 held light; without the bit the code is absent, not
// skipped (a uniform branch can still cost every vertex on the SGX, and the
// held-light term is ~6 of ~16 vertex instructions). Fragment: alpha test,
// and the fog formula of the mode in use (0 linear, 1 exp, 2 exp2; 3 the
// generic one with the selects, the fallback). All give the same image.
constexpr int kFogKinds = 4;
GLuint g_vertexShaders[4] = {};
GLuint g_fragmentShaders[2 * kFogKinds] = {};
Program g_programs[4 * 2 * kFogKinds];
bool g_programTried[4 * 2 * kFogKinds] = {};
int g_shaderState = 0; // 0 not tried, 1 ready, -1 failed

GLuint compileShader(GLenum type, const char *prefix, const char *source)
{
    const GLuint shader = glCreateShader(type);
    const GLchar *parts[2] = {prefix, source};
    glShaderSource(shader, 2, parts, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        MC_LOG_ERROR("render", "terrain shader compile failed: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool linkProgram(Program &program, GLuint vertex, GLuint fragment)
{
    program.id = glCreateProgram();
    glAttachShader(program.id, vertex);
    glAttachShader(program.id, fragment);
    glLinkProgram(program.id);
    GLint ok = GL_FALSE;
    glGetProgramiv(program.id, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        MC_LOG_ERROR("render", "terrain shader link failed\n");
        return false;
    }
    program.mvp = glGetUniformLocation(program.id, "uMvp");
    program.origin = glGetUniformLocation(program.id, "uOrigin");
    program.handLight = glGetUniformLocation(program.id, "uHandLight");
    program.wave = glGetUniformLocation(program.id, "uWave");
    program.cameraMod = glGetUniformLocation(program.id, "uCameraMod");
    program.fogColor = glGetUniformLocation(program.id, "uFogColor");
    program.fog = glGetUniformLocation(program.id, "uFog");
    program.position = glGetAttribLocation(program.id, "aPosition");
    program.light = glGetAttribLocation(program.id, "aLight");
    program.texCoord = glGetAttribLocation(program.id, "aTexCoord");
    program.color = glGetAttribLocation(program.id, "aColor");
    glUseProgram(program.id);
    glUniform1i(glGetUniformLocation(program.id, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(program.id, "uLightmap"), 1);
    glUseProgram(0);
    return program.position >= 0 && program.light >= 0 && program.texCoord >= 0 && program.color >= 0;
}

GLuint vertexShaderFor(int variant)
{
    GLuint &shader = g_vertexShaders[variant];
    if (shader != 0)
        return shader;
    std::string source = kVertexSource;
    if ((variant & 1) == 0)
    {
        const std::size_t begin = source.find("\tif (uWave.x > 0.5f)");
        const std::size_t end = source.find("\tvPosition = ");
        if (begin != std::string::npos && end != std::string::npos && end > begin)
            source.erase(begin, end - begin);
    }
    if ((variant & 2) == 0)
    {
        const std::string handLine = "\tlight.x = max(light.x, max(uHandLight.w - length(world - uHandLight.xyz), 0.0f) * 16.0f);\n";
        const std::size_t at = source.find(handLine);
        if (at != std::string::npos)
            source.erase(at, handLine.size());
    }
    shader = compileShader(GL_CG_VERTEX_SHADER_EXT, "", source.c_str());
    return shader;
}

GLuint fragmentShaderFor(bool cutout, int fogKind)
{
    GLuint &shader = g_fragmentShaders[(cutout ? kFogKinds : 0) + fogKind];
    if (shader != 0)
        return shader;
    std::string source = kFragmentSource;
    // Plain text edits rather than #if, so the variant cannot depend on the
    // compiler's preprocessor. Vanilla terrain: glAlphaFunc(GL_GREATER, 0.1).
    const std::string marker = "ALPHA_TEST";
    source.replace(source.find(marker), marker.size(), cutout ? "if (c.a <= 0.1f) discard;" : "");
    const std::string genericFog =
        "\tfloat k = uFog.w * d;\n"
        "\tfloat e = uFog.x < 1.5f ? k : k * k;\n"
        "\tfloat f = uFog.x < 0.5f ? (uFog.y - d) * uFog.z : exp(-e * 1.442695f);\n";
    const char *fogByKind[kFogKinds] = {
        "\tfloat f = (uFog.y - d) * uFog.z;\n",
        "\tfloat f = exp(-uFog.w * d * 1.442695f);\n",
        "\tfloat k = uFog.w * d;\n\tfloat f = exp(-k * k * 1.442695f);\n",
        nullptr,
    };
    if (fogByKind[fogKind] != nullptr)
    {
        const std::size_t at = source.find(genericFog);
        if (at != std::string::npos)
            source.replace(at, genericFog.size(), fogByKind[fogKind]);
    }
    shader = compileShader(GL_CG_FRAGMENT_SHADER_EXT, "", source.c_str());
    return shader;
}

// The generic program (all vertex code, generic fog) is the one shaderReady
// requires; a variant that fails to build falls back to it.
const Program &programFor(bool wave, bool hand, bool cutout, int fogKind)
{
    const int vertexVariant = (wave ? 1 : 0) | (hand ? 2 : 0);
    const int index = (vertexVariant * 2 + (cutout ? 1 : 0)) * kFogKinds + fogKind;
    Program &program = g_programs[index];
    if (program.id != 0)
        return program;
    if (!g_programTried[index])
    {
        g_programTried[index] = true;
        const GLuint vertex = vertexShaderFor(vertexVariant);
        const GLuint fragment = fragmentShaderFor(cutout, fogKind);
        if (vertex != 0 && fragment != 0 && linkProgram(program, vertex, fragment))
            return program;
        program.id = 0;
        MC_LOG_ERROR("render", "terrain shader variant %d unavailable, generic one used\n", index);
    }
    return g_programs[(3 * 2 + (cutout ? 1 : 0)) * kFogKinds + 3];
}

bool shaderReady()
{
    if (g_shaderState != 0)
        return g_shaderState > 0;
    g_shaderState = -1;
    const bool ok = programFor(true, true, false, 3).id != 0 && programFor(true, true, true, 3).id != 0;
    if (ok)
    {
        g_shaderState = 1;
        MC_LOG_INFO("render", "terrain shader ready\n");
    }
    else
    {
        MC_LOG_ERROR("render", "terrain shader unavailable, fixed-function terrain\n");
    }
    return g_shaderState > 0;
}

// Column-major product, as OpenGL stores matrices.
void multiply(const float *a, const float *b, float *out)
{
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[column * 4 + k];
            out[column * 4 + row] = sum;
        }
}

double g_viewer[3] = {0.0, 0.0, 0.0};

float g_wavePhase = 0.0f;

void setFrameUniforms(const Program &program, const float *mvpRowMajor)
{
    const VitaFixedState &fixed = vitaFixedState();
    glUniformMatrix4fv(program.mvp, 1, GL_FALSE, mvpRowMajor);
    // The sections are placed relative to the client player's posY, which in
    // 1.2.5 is eye level; the hand is half a block lower.
    if (program.handLight >= 0)
        glUniform4f(program.handLight, 0.0f, -0.5f, 0.0f, static_cast<float>(Config::getHeldLightLevel()));
    float waveOn = 0.0f, phase = 0.0f, camera[3] = {0.0f, 0.0f, 0.0f};
    if (Config::isShaderEnhanced())
    {
        waveOn = 1.0f;
        // Set when the opaque pass draws (drawShaderBatch), reused by pass 1:
        // Fancy water draws pass 1 twice (depth only, then colour with a depth
        // test), and a phase read per draw moved the ripple between them, so
        // the colour failed the depth test where the surface rose: water in
        // patches showing the bed.
        phase = g_wavePhase;
        for (int a = 0; a < 3; ++a)
        {
            const double m = std::fmod(g_viewer[a], 16.0);
            camera[a] = static_cast<float>(m < 0.0 ? m + 16.0 : m);
        }
    }
    if (program.wave >= 0)
        glUniform4f(program.wave, waveOn, phase, 0.0f, 0.0f);
    if (program.cameraMod >= 0)
        glUniform3f(program.cameraMod, camera[0], camera[1], camera[2]);
    glUniform4fv(program.fogColor, 1, fixed.fogColor);
    if (fixed.fog)
    {
        const float range = fixed.fogEnd - fixed.fogStart;
        glUniform4f(program.fog, static_cast<float>(fixed.fogMode), fixed.fogEnd,
            range != 0.0f ? 1.0f / range : 0.0f, fixed.fogDensity);
    }
    else
    {
        glUniform4f(program.fog, 0.0f, 1.0e30f, 1.0f, 0.0f); // fog factor clamps to 1
    }
}

void drawShaderRange(const Program &program, const VitaTerrainDraw *draws, std::size_t count, bool solidPart)
{
    glUseProgram(program.id);
    glEnableVertexAttribArray(program.position);
    glEnableVertexAttribArray(program.light);
    glEnableVertexAttribArray(program.texCoord);
    glEnableVertexAttribArray(program.color);
    for (std::size_t i = 0; i < count; ++i)
    {
        const VitaTerrainDraw &draw = draws[i];
        if (!draw.shader)
            continue;
        const int first = solidPart ? 0 : draw.solidCount;
        const int end = solidPart ? draw.solidCount : draw.vertexCount;
        if (end <= first)
            continue;
        // Runs of groups to draw: all of the range, or (grouped section) the
        // groups not entirely behind the eye. A +X face at plane px is
        // back-facing when the eye's x <= px; a group is skipped when that
        // holds for all its planes, with a margin for the wave displacement.
        int runFirst[kVitaFaceGroups], runEnd[kVitaFaceGroups];
        int runs = 0;
        const VitaTerrainMesh *mesh = draw.mesh;
        if (mesh != nullptr && mesh->grouped)
        {
            constexpr int kMargin = 256; // 0.25 block in 1/1024 units
            const int range = solidPart ? 0 : 1;
            const float eye[3] = {-draw.x * kFixedScale, -draw.y * kFixedScale, -draw.z * kFixedScale};
            int groupStart = 0;
            for (int slot = 0; slot < kVitaFaceGroups; ++slot)
            {
                const int groupEnd = mesh->groups.end[range][slot];
                const int code = kGroupStorageOrder[slot];
                bool keep = groupEnd > groupStart;
                if (keep && code < 6)
                {
                    const float e = eye[code >> 1];
                    keep = (code & 1) != 0
                        ? e > static_cast<float>(mesh->groups.planeMin[range][slot]) - kMargin
                        : e < static_cast<float>(mesh->groups.planeMax[range][slot]) + kMargin;
                }
                if (keep)
                {
                    if (runs > 0 && runEnd[runs - 1] == first + groupStart)
                        runEnd[runs - 1] = first + groupEnd;
                    else
                    {
                        runFirst[runs] = first + groupStart;
                        runEnd[runs] = first + groupEnd;
                        ++runs;
                    }
                }
                groupStart = groupEnd;
            }
            if (runs == 0)
                continue;
        }
        else
        {
            runFirst[0] = first;
            runEnd[0] = end;
            runs = 1;
        }
        glUniform3f(program.origin, draw.x, draw.y, draw.z);
        // Pointers capture the bound buffer, so they are set per section.
        glBindBuffer(GL_ARRAY_BUFFER, draw.buffer);
        glVertexAttribPointer(program.position, 3, GL_SHORT, GL_FALSE, sizeof(ShaderVertex), offsetPointer(0));
        glVertexAttribPointer(program.light, 2, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(ShaderVertex), offsetPointer(6));
        glVertexAttribPointer(program.texCoord, 2, GL_UNSIGNED_SHORT, GL_TRUE, sizeof(ShaderVertex), offsetPointer(8));
        glVertexAttribPointer(program.color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ShaderVertex), offsetPointer(12));
        for (int run = 0; run < runs; ++run)
        {
            for (int start = runFirst[run]; start < runEnd[run]; start += kMaxQuadDrawVertices)
            {
                g_vitaPerfDraws += 1;
                glDrawArrays(GL_QUADS, start, std::min(kMaxQuadDrawVertices, runEnd[run] - start));
            }
        }
    }
    glDisableVertexAttribArray(program.position);
    glDisableVertexAttribArray(program.light);
    glDisableVertexAttribArray(program.texCoord);
    glDisableVertexAttribArray(program.color);
}

void drawShaderBatch(const VitaTerrainDraw *draws, std::size_t count, int pass)
{
    float modelview[16];
    float projection[16];
    float mvp[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, modelview);
    glGetFloatv(GL_PROJECTION_MATRIX, projection);
    multiply(projection, modelview, mvp);
    // Cg reads the uniform row-major and vitaGL copies it as given.
    float mvpRowMajor[16];
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            mvpRowMajor[row * 4 + column] = mvp[column * 4 + row];

    const bool wave = Config::isShaderEnhanced();
    if (pass == 0)
        g_wavePhase = static_cast<float>(std::fmod(
            static_cast<double>(PlatformCompat::getMonotonicMicros()) * 1.5e-6, 6.283185307179586));
    const bool hand = Config::getHeldLightLevel() > 0;
    const VitaFixedState &fixed = vitaFixedState();
    const int fogKind = !fixed.fog ? 0 : std::min(std::max(fixed.fogMode, 0), 2);
    const Program &solidProgram = programFor(wave, hand, false, fogKind);
    const Program &cutoutProgram = programFor(wave, hand, true, fogKind);
    // Vertices drawn per frame: solid (hidden-surface removal works) and the
    // rest (alpha-tested in pass 0: every layer is shaded).
    long long solidVertices = 0, restVertices = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!draws[i].shader)
            continue;
        if (pass == 0)
            solidVertices += draws[i].solidCount;
        restVertices += draws[i].vertexCount - (pass == 0 ? draws[i].solidCount : 0);
    }
    platformProfileTickPhase(pass == 0 ? "#tVert0" : "#tVert1", solidVertices);
    if (pass == 0)
    {
        // Vertices the face groups keep off the GPU this frame.
        long long skipped = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const VitaTerrainMesh *mesh = draws[i].mesh;
            if (!draws[i].shader || mesh == nullptr || !mesh->grouped)
                continue;
            const float eye[3] = {-draws[i].x * kFixedScale, -draws[i].y * kFixedScale, -draws[i].z * kFixedScale};
            for (int range = 0; range < 2; ++range)
            {
                int groupStart = 0;
                for (int slot = 0; slot < kVitaFaceGroups; ++slot)
                {
                    const int groupEnd = mesh->groups.end[range][slot];
                    const int code = kGroupStorageOrder[slot];
                    if (groupEnd > groupStart && code < 6)
                    {
                        const float e = eye[code >> 1];
                        const bool keep = (code & 1) != 0
                            ? e > static_cast<float>(mesh->groups.planeMin[range][slot]) - 256.0f
                            : e < static_cast<float>(mesh->groups.planeMax[range][slot]) + 256.0f;
                        if (!keep)
                            skipped += groupEnd - groupStart;
                    }
                    groupStart = groupEnd;
                }
            }
        }
        platformProfileTickPhase("#tVertSkip", skipped);
    }
    platformProfileTickPhase(pass == 0 ? "#tVertCut" : "#tVert1b", restVertices);
    if (pass == 0)
    {
        setFrameUniforms(solidProgram, mvpRowMajor);
        drawShaderRange(solidProgram, draws, count, true);
    }
    // Past solidCount: tiles with transparent texels (pass 0) take the
    // alpha-tested program. Pass 1 (water, ice) does not: Fancy water draws it
    // first depth-only (colour mask off), and a discard in that pass makes the
    // SGX drop pixels in a 2x2 pattern, so the colour pass fills only
    // every other pixel. Water and ice have no texel the 0.1 test would drop.
    const Program &rest = pass == 0 ? cutoutProgram : solidProgram;
    setFrameUniforms(rest, mvpRowMajor);
    drawShaderRange(rest, draws, count, false);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

// first/end select the vertices: solid part, alpha-tested part, or both.
enum class FixedRange { Solid, Cutout, All };

void drawFixedFunctionRange(const VitaTerrainDraw *draws, std::size_t count, FixedRange range)
{
    // One glLoadMatrixf per section instead of push/translate/scale/pop: each
    // is a vitaGL call that dirties the matrices. Built from the same base
    // every time, so float error cannot drift across sections.
    float base[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, base);
    float section[16];
    for (int i = 0; i < 12; ++i)
        section[i] = base[i] * kFixedInvScale;
    bool moved = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        const VitaTerrainDraw &draw = draws[i];
        if (draw.shader)
            continue;
        const int first = range == FixedRange::Cutout ? draw.solidCount : 0;
        const int end = range == FixedRange::Solid ? draw.solidCount : draw.vertexCount;
        if (end <= first)
            continue;

        for (int row = 0; row < 4; ++row)
            section[12 + row] = base[row] * draw.x + base[4 + row] * draw.y + base[8 + row] * draw.z + base[12 + row];
        glLoadMatrixf(section);
        moved = true;

        glBindBuffer(GL_ARRAY_BUFFER, draw.buffer);
        glVertexPointer(3, GL_SHORT, sizeof(Vertex), offsetPointer(0));
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), offsetPointer(8));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), offsetPointer(16));
        glClientActiveTexture(GL_TEXTURE1);
        glTexCoordPointer(2, GL_SHORT, sizeof(Vertex), offsetPointer(20));
        glClientActiveTexture(GL_TEXTURE0);

        for (int start = first; start < end; start += kMaxQuadDrawVertices)
        {
            g_vitaPerfDraws += 1;
            glDrawArrays(GL_QUADS, start, std::min(kMaxQuadDrawVertices, end - start));
        }
    }
    if (moved)
        glLoadMatrixf(base);
}

void drawFixedFunctionBatch(const VitaTerrainDraw *draws, std::size_t count, int pass)
{
    // Client state once for the whole batch.
    glClientActiveTexture(GL_TEXTURE0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glClientActiveTexture(GL_TEXTURE1);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTexture(GL_TEXTURE0);

    (void)pass;
    // One draw per section with the game's alpha test on. Splitting the solid
    // part off (alpha test off, for the SGX's hidden-surface removal) doubled
    // the draw calls, and the CPU is the limit here while the GPU idles.
    drawFixedFunctionRange(draws, count, FixedRange::All);

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glClientActiveTexture(GL_TEXTURE1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Not redundant: with lighting on, vitaGL feeds the material from the
    // colour/normal pointers it last saw, so a stale VBO pointer recolours the
    // next lit draw (entities turned red/blue). Same reset as the list draw.
    glVertexPointer(3, GL_FLOAT, 0, nullptr);
    glTexCoordPointer(2, GL_FLOAT, 0, nullptr);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
    glClientActiveTexture(GL_TEXTURE1);
    glTexCoordPointer(2, GL_SHORT, 0, nullptr);
    glClientActiveTexture(GL_TEXTURE0);
}

GLuint upload(const void *data, std::size_t bytes)
{
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    if (buffer == 0)
        return 0;
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(bytes), data, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g_vitaPerfBufferBytes += static_cast<unsigned>(bytes);
    return buffer;
}
}

void vitaTerrainSetAtlas(const unsigned char *rgba, int width, int height)
{
    if (rgba == nullptr || width < 16 || height < 16)
        return;
    const int tileWidth = width / 16;
    const int tileHeight = height / 16;
    std::uint64_t alpha[4] = {0, 0, 0, 0};
    for (int y = 0; y < tileHeight * 16; ++y)
        for (int x = 0; x < tileWidth * 16; ++x)
            if (rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3] != 255)
            {
                const int tile = (y / tileHeight) * 16 + x / tileWidth;
                alpha[tile >> 6] |= 1ull << (tile & 63);
            }
    // Fire (31, 47) is drawn by TextureFX at runtime, whatever the file holds.
    alpha[0] |= (1ull << 31) | (1ull << 47);
    std::memcpy(g_tileAlpha, alpha, sizeof(alpha));
    int transparentTiles = 0;
    for (std::uint64_t bits : alpha)
        transparentTiles += __builtin_popcountll(bits);
    MC_LOG_INFO("render", "terrain atlas %dx%d: %d/256 tiles with transparent texels\n", width, height, transparentTiles);
}

unsigned vitaStaticQuadsUpload(const float *xyz, std::size_t vertexCount)
{
    if (xyz == nullptr || vertexCount == 0)
        return 0;
    return upload(xyz, vertexCount * 3 * sizeof(float));
}

void vitaStaticQuadsRelease(unsigned buffer)
{
    if (buffer != 0)
    {
        const GLuint id = buffer;
        glDeleteBuffers(1, &id);
    }
}

void vitaStaticQuadsDraw(unsigned buffer, int first, int count, const float rgba[4])
{
    if (buffer == 0 || count <= 0)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 3 * sizeof(float), offsetPointer(0));
    glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
    for (int start = first; start < first + count; start += kMaxQuadDrawVertices)
    {
        g_vitaPerfDraws += 1;
        glDrawArrays(GL_QUADS, start, std::min(kMaxQuadDrawVertices, first + count - start));
    }
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(3, GL_FLOAT, 0, nullptr);
}

void vitaTerrainRelease(VitaTerrainMesh &mesh)
{
    if (mesh.buffer != 0)
    {
        // vitaGL defers the free while the buffer is still in flight.
        const GLuint buffer = mesh.buffer;
        glDeleteBuffers(1, &buffer);
    }
    mesh.buffer = 0;
    mesh.vertexCount = 0;
    mesh.solidCount = 0;
}

VitaTerrainPublish vitaTerrainPublish(VitaTerrainMesh &out, const RenderCapturedMesh &compact,
    const RenderCapturedMesh &base, int pass, bool useShader)
{
    VitaTerrainMesh old = out;
    out = VitaTerrainMesh();

    PlatformTickPhaseScope convertPhase("pubConvert");
    VitaTerrainPublish result = VitaTerrainPublish::Unsupported;
    std::size_t shaderSolid = 0, shaderTotal = 0;
    const int capturedVertices = (compact.empty() ? 0 : compact.vertexCount) + (base.empty() ? 0 : base.vertexCount);
    bool shader = useShader && capturedVertices > 0 && shaderReady() &&
        buildShaderDirect(compact, base, pass, shaderSolid, shaderTotal);
    if (shader)
    {
        result = shaderTotal > 0 ? VitaTerrainPublish::Uploaded : VitaTerrainPublish::Empty;
    }
    else
    {
        g_scratch.clear();
        if (appendCompact(compact) && appendBase(base))
            result = g_scratch.empty() ? VitaTerrainPublish::Empty : VitaTerrainPublish::Uploaded;
    }
    // Size of what is converted ("pubVerts": vertices per frame, largest
    // section as the max) and how often a shader build falls back.
    platformProfileTickPhase("pubVerts", static_cast<long long>(capturedVertices) * 1000000LL);
    if (useShader && !shader && result == VitaTerrainPublish::Uploaded)
        platformProfileTickPhase("#pubFixed", 1);

    if (result == VitaTerrainPublish::Uploaded)
    {
        if (shader)
        {
            out.solidCount = static_cast<int>(shaderSolid);
            out.vertexCount = static_cast<int>(shaderTotal);
            // Pass 1 draws with cull face off: nothing to skip there.
            out.grouped = pass == 0 && (shaderSolid >= kGroupMinVertices || shaderTotal - shaderSolid >= kGroupMinVertices);
            if (out.grouped)
            {
                PlatformTickPhaseScope groupPhase("pubGroup");
                groupRange(g_shaderVertices.data(), shaderSolid, out.groups, 0);
                groupRange(g_shaderVertices.data() + shaderSolid, shaderTotal - shaderSolid, out.groups, 1);
            }
        }
        else
        {
            buildQuads(pass);
            const int solid = static_cast<int>(g_solid.size());
            g_solid.insert(g_solid.end(), g_cutout.begin(), g_cutout.end());
            out.solidCount = solid;
            out.vertexCount = static_cast<int>(g_solid.size());
        }
        out.shader = shader;
        PlatformTickPhaseScope uploadPhase("pubUpload");
        out.buffer = shader
            ? upload(g_shaderVertices.data(), shaderTotal * sizeof(ShaderVertex))
            : upload(g_solid.data(), g_solid.size() * sizeof(Vertex));
        if (out.buffer == 0)
        {
            out = VitaTerrainMesh();
            result = VitaTerrainPublish::Unsupported;
        }
    }

    PlatformTickPhaseScope releasePhase("pubRelease");
    vitaTerrainRelease(old);
    return result;
}

void vitaTerrainSetViewer(double x, double y, double z)
{
    g_viewer[0] = x;
    g_viewer[1] = y;
    g_viewer[2] = z;
}

void vitaTerrainDrawBatch(const VitaTerrainDraw *draws, std::size_t count, int pass)
{
    if (count == 0)
        return;
    bool anyShader = false;
    bool anyFixed = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        anyShader |= draws[i].shader;
        anyFixed |= !draws[i].shader;
    }
    if (anyFixed)
        drawFixedFunctionBatch(draws, count, pass);
    if (anyShader)
        drawShaderBatch(draws, count, pass);
}

#endif
