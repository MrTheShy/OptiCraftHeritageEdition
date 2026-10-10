#include "platform/RenderModelBatch.h"

#if PLATFORM_MODEL_GPU_BATCH

#include <glad/glad.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config.h"
#include "ModelBox.h"
#include "ModelRenderer.h"
#include "PositionTextureVertex.h"
#include "TexturedQuad.h"
#include "pc/render/VitaModelBatch.h"
#include "pc/render/VitaTerrainBatch.h"
#include "platform/Log.h"

extern "C" unsigned g_vitaPerfDraws;
extern "C" unsigned g_vitaPerfBufferBytes;

bool g_vitaModelPending = false;

namespace
{
// Parts one renderer can hold (all its models: a player is 9 + 2 x 7 armour).
// Three float4 rows of modelview per part in the vertex uniforms.
constexpr int kMaxParts = 32;

struct Vertex
{
    float x, y, z, slot; // position already multiplied by the part's scale
    float u, v;
    std::int8_t nx, ny, nz, pad; // as Tessellator::setNormal quantises it
};
static_assert(sizeof(Vertex) == 28, "model vertex must stay 28 bytes");

// Every part one scope key (a renderer) has drawn, in one VBO.
struct Layout
{
    std::vector<ModelRenderer *> parts; // nullptr: part destroyed or rebuilt
    std::vector<float> scales;
    std::vector<int> first, count;
    GLuint buffer = 0;
    bool dirty = true;
};

std::unordered_map<const void *, Layout *> g_layouts;
// Per thread: models built on a worker (chunk meshes) never join a batch.
thread_local Layout *g_scope = nullptr;

Layout *g_pendingLayout = nullptr;
std::uint32_t g_pendingMask = 0;
float g_rows[kMaxParts * 3][4];

// Tracked fixed-function state, GL defaults until the game sets it.
float g_lightAmbient[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
float g_lightDiffuse[2][4] = {{1, 1, 1, 1}, {0, 0, 0, 1}};
float g_lightDir[2][4] = {{0, 0, 1, 0}, {0, 0, 1, 0}};
bool g_lightPoint[2] = {false, false};
float g_globalAmbient[4] = {0.2f, 0.2f, 0.2f, 1.0f};
unsigned g_alphaFunc = GL_ALWAYS;
bool g_colorMaterial = false;
// Alpha-test-dropped texels (alpha <= 0.1) of small textures (mob and skin
// sized), and per part+texture whether its faces sample any of them.
struct TextureAlpha
{
    int width = 0, height = 0;
    std::vector<std::uint8_t> dropped;
};
std::unordered_map<int, TextureAlpha> g_textureAlpha;
std::unordered_map<std::uint64_t, bool> g_partCutout;
GLint g_batchTexture = 0;
// A pending part samples texels the alpha test drops: draw with the discard
// program. Without one the SGX keeps its hidden-surface removal.
bool g_pendingCutout = false;

bool partSamplesDroppedTexels(ModelRenderer *part, int texture)
{
    const auto found = g_textureAlpha.find(texture);
    if (found == g_textureAlpha.end())
        return false;
    const std::uint64_t key = (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(part)) << 16) ^
        static_cast<std::uint32_t>(texture);
    const auto cached = g_partCutout.find(key);
    if (cached != g_partCutout.end())
        return cached->second;
    const TextureAlpha &alpha = found->second;
    bool dropped = false;
    for (ModelBox *box : part->boxes())
    {
        for (int q = 0; box != nullptr && q < 6 && !dropped; ++q)
        {
            TexturedQuad *quad = box->getQuad(q);
            if (quad == nullptr)
                continue;
            float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                u0 = std::min(u0, quad->vertexPositions[i]->texturePositionX);
                u1 = std::max(u1, quad->vertexPositions[i]->texturePositionX);
                v0 = std::min(v0, quad->vertexPositions[i]->texturePositionY);
                v1 = std::max(v1, quad->vertexPositions[i]->texturePositionY);
            }
            // The texels the face covers.
            const int x0 = std::max(0, static_cast<int>(std::floor(u0 * alpha.width)));
            const int x1 = std::min(alpha.width, static_cast<int>(std::ceil(u1 * alpha.width)));
            const int y0 = std::max(0, static_cast<int>(std::floor(v0 * alpha.height)));
            const int y1 = std::min(alpha.height, static_cast<int>(std::ceil(v1 * alpha.height)));
            for (int y = y0; y < y1 && !dropped; ++y)
                for (int x = x0; x < x1 && !dropped; ++x)
                    dropped = alpha.dropped[static_cast<std::size_t>(y * alpha.width + x)] != 0;
        }
    }
    g_partCutout.emplace(key, dropped);
    return dropped;
}

// Profiler: printed every 10 s.
unsigned g_statDraws = 0, g_statParts = 0, g_statFallbackParts = 0;
std::chrono::steady_clock::time_point g_statSince = std::chrono::steady_clock::now();

// --- Cg programs ------------------------------------------------------------

// The fixed-function vertex stage vitaGL runs for a lit, colour-material model
// part (ffp_v.h): eye position from the modelview, normal through its inverse
// transpose, directional lights, colour clamped. Rows of the inverse transpose
// are the cross products of the modelview rows over the determinant.
const char *kVertexSource = R"(
void main(
	float4 aPosition,
	float2 aTexCoord,
	float3 aNormal,
	uniform float4x4 uProjection,
	uniform float4 uParts[96],
	uniform float4 uTexRow0,
	uniform float4 uTexRow1,
	uniform float4 uColor,
	uniform float4 uAmbient,
	uniform float4 uLightDir0,
	uniform float4 uLightDiffuse0,
	uniform float4 uLightDir1,
	uniform float4 uLightDiffuse1,
	uniform float uNormalize,
	uniform float4 uLightCoord,
	float4 out vPosition : POSITION,
	float2 out vTexCoord : TEXCOORD0,
	float2 out vLightCoord : TEXCOORD1,
	float4 out vColor : COLOR)
{
	vLightCoord = uLightCoord.xy;
	int base = (int)aPosition.w * 3;
	float4 r0 = uParts[base];
	float4 r1 = uParts[base + 1];
	float4 r2 = uParts[base + 2];
	float4 p = float4(aPosition.xyz, 1.0f);
	vPosition = mul(uProjection, float4(dot(r0, p), dot(r1, p), dot(r2, p), 1.0f));
	float4 uv = float4(aTexCoord, 0.0f, 1.0f);
	vTexCoord = float2(dot(uTexRow0, uv), dot(uTexRow1, uv));
	float3 c0 = cross(r1.xyz, r2.xyz);
	float3 c1 = cross(r2.xyz, r0.xyz);
	float3 c2 = cross(r0.xyz, r1.xyz);
	float3 n = float3(dot(c0, aNormal), dot(c1, aNormal), dot(c2, aNormal)) / dot(r0.xyz, c0);
	if (uNormalize > 0.5f)
		n = normalize(n);
	float3 light = uAmbient.rgb
		+ uLightDiffuse0.rgb * max(0.0f, dot(n, uLightDir0.xyz))
		+ uLightDiffuse1.rgb * max(0.0f, dot(n, uLightDir1.xyz));
	vColor = clamp(float4(uColor.rgb * light, uColor.a), 0.0f, 1.0f);
}
)";

// vitaGL's fixed-function fragment stage (ffp_f.h): MODULATE on both units,
// alpha test (GL_GREATER), then fog from WPOS. Same arithmetic as the terrain
// shader. Samplers only exist in the variants that read them, so a unit with
// texturing off is never sampled.
const char *kFragmentHead = R"(
float4 main(
	float2 vTexCoord : TEXCOORD0,
	float4 vColor : COLOR,
	float4 coords : WPOS,
)";
const char *kFragmentTextureParam = "\tuniform sampler2D uTexture,\n";
// The lightmap coordinate is constant per draw but arrives as a varying, as in
// vitaGL's fixed pipeline and the terrain shader: a tex2D on a uniform
// coordinate would be a dependent texture read on the SGX.
const char *kFragmentLightmapParam = "\tfloat2 vLightCoord : TEXCOORD1,\n\tuniform sampler2D uLightmap,\n";
const char *kFragmentBodyStart = R"(	uniform float uAlphaRef,
	uniform float4 uFogColor,
	uniform float4 uFog)
{
	float4 c = vColor;
)";
const char *kFragmentTexture = "\tc = tex2D(uTexture, vTexCoord) * c;\n";
const char *kFragmentLightmap = "\tc = tex2D(uLightmap, vLightCoord) * c;\n";
// The alpha test is appended by shaderReady only for draws that need it: with
// discard the SGX loses its hidden-surface removal.
const char *kFragmentBodyEnd = R"(	// uFog: x mode (0 linear, 1 exp, 2 exp2), y far, z 1/(far-near), w density
	float d = coords.z / coords.w;
	float k = uFog.w * d;
	float e = uFog.x < 1.5f ? k : k * k;
	float f = uFog.x < 0.5f ? (uFog.y - d) * uFog.z : exp(-e * 1.442695f);
	c.rgb = lerp(uFogColor.rgb, c.rgb, clamp(f, 0.0f, 1.0f));
	return c;
}
)";

struct Program
{
    GLuint id = 0;
    GLint position = -1, texCoord = -1, normal = -1;
    GLint projection = -1, parts = -1, texRow0 = -1, texRow1 = -1, color = -1, ambient = -1;
    GLint lightDir0 = -1, lightDiffuse0 = -1, lightDir1 = -1, lightDiffuse1 = -1, normalize = -1;
    GLint lightCoord = -1, alphaRef = -1, fogColor = -1, fog = -1;
};

// Index: bit 0 texture unit 0 on, bit 1 lightmap (unit 1) on, bit 2 alpha test.
Program g_programs[8];
int g_shaderState = 0; // 0 not tried, 1 ready, -1 failed

GLuint compileShader(GLenum type, const std::string &source)
{
    const GLuint shader = glCreateShader(type);
    const GLchar *text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        MC_LOG_ERROR("render", "entity shader compile failed: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool linkProgram(Program &program, GLuint vertex, GLuint fragment, bool texture, bool lightmap)
{
    program.id = glCreateProgram();
    glAttachShader(program.id, vertex);
    glAttachShader(program.id, fragment);
    glLinkProgram(program.id);
    GLint ok = GL_FALSE;
    glGetProgramiv(program.id, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        MC_LOG_ERROR("render", "entity shader link failed\n");
        return false;
    }
    const GLuint id = program.id;
    program.position = glGetAttribLocation(id, "aPosition");
    program.texCoord = glGetAttribLocation(id, "aTexCoord");
    program.normal = glGetAttribLocation(id, "aNormal");
    program.projection = glGetUniformLocation(id, "uProjection");
    program.parts = glGetUniformLocation(id, "uParts");
    program.texRow0 = glGetUniformLocation(id, "uTexRow0");
    program.texRow1 = glGetUniformLocation(id, "uTexRow1");
    program.color = glGetUniformLocation(id, "uColor");
    program.ambient = glGetUniformLocation(id, "uAmbient");
    program.lightDir0 = glGetUniformLocation(id, "uLightDir0");
    program.lightDiffuse0 = glGetUniformLocation(id, "uLightDiffuse0");
    program.lightDir1 = glGetUniformLocation(id, "uLightDir1");
    program.lightDiffuse1 = glGetUniformLocation(id, "uLightDiffuse1");
    program.normalize = glGetUniformLocation(id, "uNormalize");
    program.lightCoord = glGetUniformLocation(id, "uLightCoord");
    program.alphaRef = glGetUniformLocation(id, "uAlphaRef");
    program.fogColor = glGetUniformLocation(id, "uFogColor");
    program.fog = glGetUniformLocation(id, "uFog");
    glUseProgram(id);
    if (texture)
        glUniform1i(glGetUniformLocation(id, "uTexture"), 0);
    if (lightmap)
        glUniform1i(glGetUniformLocation(id, "uLightmap"), 1);
    glUseProgram(0);
    return program.position >= 0 && program.texCoord >= 0 && program.normal >= 0 && program.parts >= 0;
}

bool shaderReady()
{
    if (g_shaderState != 0)
        return g_shaderState > 0;
    g_shaderState = -1;
    const GLuint vertex = compileShader(GL_CG_VERTEX_SHADER_EXT, kVertexSource);
    if (vertex == 0)
        return false;
    for (int variant = 0; variant < 8; ++variant)
    {
        const bool texture = (variant & 1) != 0;
        const bool lightmap = (variant & 2) != 0;
        const bool cutout = (variant & 4) != 0;
        std::string source = kFragmentHead;
        if (texture)
            source += kFragmentTextureParam;
        if (lightmap)
            source += kFragmentLightmapParam;
        source += kFragmentBodyStart;
        if (texture)
            source += kFragmentTexture;
        if (lightmap)
            source += kFragmentLightmap;
        std::string end = kFragmentBodyEnd;
        // The alpha test is the last statement, after fog. Placed earlier (before
        // fog as in vitaGL's own program, or as clip()) the SGX drops pixels in a
        // 2x2 pattern and keeps alpha-0 texels.
        if (cutout)
            end.replace(end.rfind("\treturn c;"), 10, "\tif (c.a <= 0.1f)\n\t\tdiscard;\n\treturn c;");
        source += end;
        const GLuint fragment = compileShader(GL_CG_FRAGMENT_SHADER_EXT, source);
        if (fragment == 0 || !linkProgram(g_programs[variant], vertex, fragment, texture, lightmap))
        {
            MC_LOG_ERROR("render", "entity shader unavailable, fixed-function models\n");
            return false;
        }
    }
    g_shaderState = 1;
    MC_LOG_INFO("render", "entity shader ready\n");
    return true;
}

// --- Geometry ---------------------------------------------------------------

std::int8_t quantizeNormal(float value)
{
    return static_cast<std::int8_t>(static_cast<int>(value * 127.0f));
}

bool rebuild(Layout &layout)
{
    static std::vector<Vertex> vertices;
    vertices.clear();
    const std::size_t partCount = layout.parts.size();
    layout.first.assign(partCount, 0);
    layout.count.assign(partCount, 0);
    for (std::size_t slot = 0; slot < partCount; ++slot)
    {
        layout.first[slot] = static_cast<int>(vertices.size());
        ModelRenderer *part = layout.parts[slot];
        if (part == nullptr)
            continue;
        const float scale = layout.scales[slot];
        // Same vertices, order and normals as ModelRenderer's compiled list.
        for (ModelBox *box : part->boxes())
        {
            if (box == nullptr)
                continue;
            for (int q = 0; q < 6; ++q)
            {
                TexturedQuad *quad = box->getQuad(q);
                if (quad == nullptr)
                    continue;
                float nx, ny, nz;
                quad->faceNormal(nx, ny, nz);
                for (int i = 0; i < 4; ++i)
                {
                    const PositionTextureVertex *corner = quad->vertexPositions[i];
                    Vertex vertex;
                    vertex.x = corner->positionX * scale;
                    vertex.y = corner->positionY * scale;
                    vertex.z = corner->positionZ * scale;
                    vertex.slot = static_cast<float>(slot);
                    vertex.u = corner->texturePositionX;
                    vertex.v = corner->texturePositionY;
                    vertex.nx = quantizeNormal(nx);
                    vertex.ny = quantizeNormal(ny);
                    vertex.nz = quantizeNormal(nz);
                    vertex.pad = 0;
                    vertices.push_back(vertex);
                }
            }
        }
        layout.count[slot] = static_cast<int>(vertices.size()) - layout.first[slot];
    }

    if (layout.buffer != 0)
    {
        // vitaGL defers the free while the buffer is still in flight.
        glDeleteBuffers(1, &layout.buffer);
        layout.buffer = 0;
    }
    layout.dirty = false;
    if (vertices.empty())
        return true;
    glGenBuffers(1, &layout.buffer);
    if (layout.buffer == 0)
    {
        layout.dirty = true;
        return false;
    }
    const std::size_t bytes = vertices.size() * sizeof(Vertex);
    glBindBuffer(GL_ARRAY_BUFFER, layout.buffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(bytes), vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g_vitaPerfBufferBytes += static_cast<unsigned>(bytes);
    return true;
}

// --- State ------------------------------------------------------------------

// What the shader reproduces. Checked when a batch starts; it cannot change
// while parts are pending, since every state call flushes them first.
bool stateSupported()
{
    if (vitaDisplayListRecording())
        return false;
    if (vitaFixedState().alphaTest)
    {
        if (g_alphaFunc != GL_GREATER)
            return false;
    }
    // Texture binds flush the batch, so this holds for every part until then.
    g_batchTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &g_batchTexture);
    if (glIsEnabled(GL_LIGHTING) && (!g_colorMaterial || g_lightPoint[0] || g_lightPoint[1]))
        return false;
    return true;
}

void textureRows(const float *matrix, float rows[2][4])
{
    for (int row = 0; row < 2; ++row)
        for (int column = 0; column < 4; ++column)
            rows[row][column] = matrix[column * 4 + row];
}

void logStats()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - g_statSince < std::chrono::seconds(10))
        return;
    MC_LOG_INFO("render", "entity shader: %u draws for %u parts, %u parts on the fixed path (10 s)\n",
        g_statDraws, g_statParts, g_statFallbackParts);
    g_statDraws = g_statParts = g_statFallbackParts = 0;
    g_statSince = now;
}
}

void vitaModelTrackLight(int light, RenderLightParameter parameter, const float *values)
{
    if (light < 0 || light > 1 || values == nullptr)
        return;
    switch (parameter)
    {
        case RenderLightParameter::Ambient: std::memcpy(g_lightAmbient[light], values, sizeof(float) * 4); break;
        case RenderLightParameter::Diffuse: std::memcpy(g_lightDiffuse[light], values, sizeof(float) * 4); break;
        case RenderLightParameter::Specular: break; // always 0 in the game; the shader has no specular term
        case RenderLightParameter::Position:
        {
            // GL stores the position in eye space, through the current modelview.
            float m[16];
            glGetFloatv(GL_MODELVIEW_MATRIX, m);
            float eye[3];
            for (int row = 0; row < 3; ++row)
                eye[row] = m[row] * values[0] + m[4 + row] * values[1] + m[8 + row] * values[2] + m[12 + row] * values[3];
            const float length = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
            const float inverse = length > 0.0f ? 1.0f / length : 0.0f;
            g_lightDir[light][0] = eye[0] * inverse;
            g_lightDir[light][1] = eye[1] * inverse;
            g_lightDir[light][2] = eye[2] * inverse;
            g_lightDir[light][3] = 0.0f;
            g_lightPoint[light] = values[3] != 0.0f;
            break;
        }
    }
}

void vitaModelTrackLightModelAmbient(const float *values)
{
    if (values != nullptr)
        std::memcpy(g_globalAmbient, values, sizeof(g_globalAmbient));
}

void vitaModelTrackAlphaFunc(unsigned glFunction)
{
    g_alphaFunc = glFunction;
}

void vitaModelTrackTextureAlpha(int texture, int width, int height, const unsigned char *rgba)
{
    g_textureAlpha.erase(texture);
    // Ids are reused after a texture is freed; cached answers name the old one.
    g_partCutout.clear();
    if (rgba == nullptr || width <= 0 || height <= 0 || width * height > 128 * 128)
        return; // bigger textures (atlases) are never model skins
    TextureAlpha alpha;
    alpha.width = width;
    alpha.height = height;
    alpha.dropped.resize(static_cast<std::size_t>(width * height));
    bool any = false;
    for (std::size_t i = 0; i < alpha.dropped.size(); ++i)
    {
        alpha.dropped[i] = rgba[i * 4 + 3] <= 25 ? 1 : 0;
        any |= alpha.dropped[i] != 0;
    }
    if (any)
        g_textureAlpha.emplace(texture, std::move(alpha));
}

void vitaModelTrackColorMaterial(bool enabled)
{
    g_colorMaterial = enabled;
}

const void *renderModelBatchBegin(const void *key)
{
    VITA_MODEL_FLUSH();
    Layout *previous = g_scope;
    g_scope = nullptr;
    if (key != nullptr && Config::isEntityShader() && shaderReady())
    {
        Layout *&layout = g_layouts[key];
        if (layout == nullptr)
            layout = new Layout();
        g_scope = layout;
    }
    return previous;
}

void renderModelBatchEnd(const void *previous)
{
    VITA_MODEL_FLUSH();
    g_scope = static_cast<Layout *>(const_cast<void *>(previous));
    logStats();
}

bool renderModelBatchPart(ModelRenderer *part, float scale)
{
    Layout *layout = g_scope;
    if (layout == nullptr || part == nullptr)
        return false;
    if (part->boxes().empty())
        return true;
    if (g_pendingMask == 0 && !stateSupported())
    {
        ++g_statFallbackParts;
        return false;
    }

    // Faces that sample alpha-test-dropped texels (a skin's hat layer, a
    // skeleton's ribs) make the draw use the discard program.
    const bool cutout = vitaFixedState().alphaTest && glIsEnabled(GL_TEXTURE_2D) != GL_FALSE &&
        partSamplesDroppedTexels(part, g_batchTexture);

    int slot = -1;
    if (part->gpuLayout == layout)
    {
        slot = part->gpuSlot;
        if (layout->scales[static_cast<std::size_t>(slot)] != scale)
        {
            ++g_statFallbackParts;
            return false;
        }
    }
    else if (part->gpuLayout == nullptr && layout->parts.size() < static_cast<std::size_t>(kMaxParts))
    {
        slot = static_cast<int>(layout->parts.size());
        layout->parts.push_back(part);
        layout->scales.push_back(scale);
        layout->dirty = true;
        part->gpuLayout = layout;
        part->gpuSlot = slot;
    }
    else
    {
        // Full, or the part belongs to another renderer's VBO.
        ++g_statFallbackParts;
        return false;
    }

    const std::uint32_t bit = 1u << slot;
    if ((g_pendingMask & bit) != 0 || (g_pendingMask != 0 && g_pendingLayout != layout))
        vitaModelBatchFlush();

    float m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    float(*rows)[4] = &g_rows[slot * 3];
    for (int row = 0; row < 3; ++row)
    {
        rows[row][0] = m[row];
        rows[row][1] = m[4 + row];
        rows[row][2] = m[8 + row];
        rows[row][3] = m[12 + row];
    }
    g_pendingLayout = layout;
    g_pendingMask |= bit;
    g_pendingCutout |= cutout;
    g_vitaModelPending = true;
    ++g_statParts;
    return true;
}

void renderModelBatchForget(ModelRenderer *part)
{
    if (part == nullptr || part->gpuLayout == nullptr)
        return;
    Layout *layout = static_cast<Layout *>(part->gpuLayout);
    if (g_pendingLayout == layout)
        VITA_MODEL_FLUSH();
    const std::size_t slot = static_cast<std::size_t>(part->gpuSlot);
    if (slot < layout->parts.size() && layout->parts[slot] == part)
    {
        layout->parts[slot] = nullptr;
        layout->dirty = true;
    }
    part->gpuLayout = nullptr;
    part->gpuSlot = -1;
}

void vitaModelBatchFlush()
{
    g_vitaModelPending = false;
    const std::uint32_t mask = g_pendingMask;
    const bool cutout = g_pendingCutout;
    g_pendingCutout = false;
    Layout *layout = g_pendingLayout;
    g_pendingMask = 0;
    if (mask == 0 || layout == nullptr)
        return;
    if (layout->dirty && !rebuild(*layout))
    {
        MC_LOG_ERROR("render", "entity shader: model VBO allocation failed\n");
        return;
    }

    int first = 0x7fffffff, end = 0, top = 0;
    for (int slot = 0; slot < kMaxParts; ++slot)
    {
        if ((mask & (1u << slot)) == 0)
            continue;
        const int partFirst = layout->first[static_cast<std::size_t>(slot)];
        first = std::min(first, partFirst);
        end = std::max(end, partFirst + layout->count[static_cast<std::size_t>(slot)]);
        top = slot;
    }
    if (end <= first || layout->buffer == 0)
        return;
    // Parts in the drawn range that were not submitted collapse to one point
    // behind the eye (eye z = +1), so they are clipped whatever the projection.
    for (int slot = 0; slot <= top; ++slot)
        if ((mask & (1u << slot)) == 0)
        {
            std::memset(g_rows[slot * 3], 0, sizeof(float) * 12);
            g_rows[slot * 3 + 2][3] = 1.0f;
        }

    // Texture units: enable state and matrices, as the fixed pipeline reads them.
    GLint active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    float matrix[16];
    float texRows[2][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}};
    float lightRows[2][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}};
    glActiveTexture(GL_TEXTURE0);
    // Not "== GL_TRUE": vitaGL returns the unit's state bit (state & 2) for
    // GL_TEXTURE_2D, i.e. 2 when enabled (get_info.c, glIsEnabled).
    const bool texture = glIsEnabled(GL_TEXTURE_2D) != GL_FALSE;
    if (texture)
    {
        glGetFloatv(GL_TEXTURE_MATRIX, matrix);
        textureRows(matrix, texRows);
    }
    glActiveTexture(GL_TEXTURE1);
    const bool lightmap = glIsEnabled(GL_TEXTURE_2D) != GL_FALSE;
    if (lightmap)
    {
        glGetFloatv(GL_TEXTURE_MATRIX, matrix);
        textureRows(matrix, lightRows);
    }
    glActiveTexture(static_cast<GLenum>(active));

    const Program &program = g_programs[(texture ? 1 : 0) | (lightmap ? 2 : 0) | (cutout ? 4 : 0)];
    glUseProgram(program.id);

    float projection[16];
    float projectionRows[16];
    glGetFloatv(GL_PROJECTION_MATRIX, projection);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            projectionRows[row * 4 + column] = projection[column * 4 + row];
    glUniformMatrix4fv(program.projection, 1, GL_FALSE, projectionRows);
    glUniform4fv(program.parts, (top + 1) * 3, &g_rows[0][0]);
    glUniform4fv(program.texRow0, 1, texRows[0]);
    glUniform4fv(program.texRow1, 1, texRows[1]);

    float color[4];
    glGetFloatv(GL_CURRENT_COLOR, color);
    glUniform4fv(program.color, 1, color);
    const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (glIsEnabled(GL_LIGHTING))
    {
        float ambient[4] = {g_globalAmbient[0], g_globalAmbient[1], g_globalAmbient[2], 1.0f};
        const bool enabled[2] = {glIsEnabled(GL_LIGHT0) != GL_FALSE, glIsEnabled(GL_LIGHT1) != GL_FALSE};
        for (int light = 0; light < 2; ++light)
            if (enabled[light])
                for (int k = 0; k < 3; ++k)
                    ambient[k] += g_lightAmbient[light][k];
        glUniform4fv(program.ambient, 1, ambient);
        glUniform4fv(program.lightDir0, 1, g_lightDir[0]);
        glUniform4fv(program.lightDiffuse0, 1, enabled[0] ? g_lightDiffuse[0] : none);
        glUniform4fv(program.lightDir1, 1, g_lightDir[1]);
        glUniform4fv(program.lightDiffuse1, 1, enabled[1] ? g_lightDiffuse[1] : none);
    }
    else
    {
        const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        glUniform4fv(program.ambient, 1, one);
        glUniform4fv(program.lightDiffuse0, 1, none);
        glUniform4fv(program.lightDiffuse1, 1, none);
    }
    glUniform1f(program.normalize, glIsEnabled(GL_NORMALIZE) ? 1.0f : 0.0f);

    if (lightmap)
    {
        // The constant lightmap coordinate the fixed path feeds unit 1
        // (vitaBindConstantLightmap), through the unit 1 texture matrix.
        float uv[2];
        vitaLightmapCoord(uv);
        const float coord[4] = {
            lightRows[0][0] * uv[0] + lightRows[0][1] * uv[1] + lightRows[0][3],
            lightRows[1][0] * uv[0] + lightRows[1][1] * uv[1] + lightRows[1][3], 0.0f, 0.0f};
        glUniform4fv(program.lightCoord, 1, coord);
    }

    const VitaFixedState &fixed = vitaFixedState();
    glUniform1f(program.alphaRef, fixed.alphaTest ? fixed.alphaRef : -1.0f);
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

    const auto offset = [](int bytes) { return reinterpret_cast<const void *>(static_cast<std::uintptr_t>(bytes)); };
    glBindBuffer(GL_ARRAY_BUFFER, layout->buffer);
    glVertexAttribPointer(program.position, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), offset(0));
    glVertexAttribPointer(program.texCoord, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), offset(16));
    glVertexAttribPointer(program.normal, 3, GL_BYTE, GL_TRUE, sizeof(Vertex), offset(24));
    glEnableVertexAttribArray(program.position);
    glEnableVertexAttribArray(program.texCoord);
    glEnableVertexAttribArray(program.normal);
    g_vitaPerfDraws += 1;
    ++g_statDraws;
    glDrawArrays(GL_QUADS, first, end - first);
    glDisableVertexAttribArray(program.position);
    glDisableVertexAttribArray(program.texCoord);
    glDisableVertexAttribArray(program.normal);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

#endif
