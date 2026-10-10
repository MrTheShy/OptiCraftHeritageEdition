#include "platform/RenderAPI.h"
#include "pc/render/VitaTerrainBatch.h"
#include "pc/render/VitaModelBatch.h"


#include <glad/glad.h>
#include <SDL.h>
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef VITA_PLATFORM
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Per-frame work counters read (and reset) by the profiler backend.
extern "C" unsigned g_vitaPerfDraws = 0;
extern "C" unsigned g_vitaPerfTexUploads = 0;
extern "C" unsigned g_vitaPerfBufferBytes = 0;
#define VITA_PERF_COUNT(counter, n) (counter += (n))

namespace
{
std::vector<std::vector<std::function<void()>>> g_vitaLists(1);
int g_vitaRecording = 0;
// Lists being recorded around the current one. A list compiled lazily while
// another records (a model first drawn inside a cached HUD or chunk list)
// must hand recording back to the outer list when it ends, not stop it.
std::vector<int> g_vitaRecordingStack;

bool vitaRecord(std::function<void()> command)
{
    if (g_vitaRecording <= 0)
        return false;
    g_vitaLists[static_cast<std::size_t>(g_vitaRecording)].push_back(std::move(command));
    return true;
}
}

namespace PcOpenGlRenderBackend
{
bool renderCaptureInterleaved(const RenderInterleavedMesh& mesh, RenderCapturedMesh& out, bool append);
bool renderDrawCaptured(const RenderCapturedMesh& mesh);
}

// Test before building the closure: a std::function with a capture over 8
// bytes heap-allocates, and most calls happen outside any recording.
// Matrix calls leave pending model parts alone (each part took its modelview
// when submitted); every other call draws them first (VitaModelBatch).
#define VITA_RECORD_MATRIX(call) if (g_vitaRecording > 0) { vitaRecord([=] { call; }); return; } else (void)0
#define VITA_RECORD(call) VITA_MODEL_FLUSH(); VITA_RECORD_MATRIX(call)
#else
#define VITA_RECORD(call)
#define VITA_RECORD_MATRIX(call)
#define VITA_PERF_COUNT(counter, n) ((void)0)
#endif


#ifndef GL_NEAREST_MIPMAP_LINEAR
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#endif

#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL 0x813C
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

#ifndef GL_RESCALE_NORMAL
#define GL_RESCALE_NORMAL 0x803A
#endif

#ifndef GL_SAMPLES_PASSED_ARB
#define GL_SAMPLES_PASSED_ARB 0x8914
#endif
#ifndef GL_QUERY_RESULT_ARB
#define GL_QUERY_RESULT_ARB 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_ARB
#define GL_QUERY_RESULT_AVAILABLE_ARB 0x8867
#endif

namespace PcOpenGlRenderBackend
{

static bool desktopHasOpenGL12()
{
    const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    if (version == nullptr)
        return false;
    int major = 0;
    int minor = 0;
    if (std::sscanf(version, "%d.%d", &major, &minor) != 2)
        return false;
    return major > 1 || (major == 1 && minor >= 2);
}

// SDL_GL_GetProcAddress-based runtime loaders (desktop only).
static void *getProcEither(const char *arbName, const char *coreName)
{
	void *f = SDL_GL_GetProcAddress(arbName);
	if (!f) f = SDL_GL_GetProcAddress(coreName);
	return f;
}

static void activeTextureCompat(GLenum texture)
{
	using Function = void (APIENTRYP)(GLenum);
	static Function function = reinterpret_cast<Function>(getProcEither("glActiveTextureARB", "glActiveTexture"));
	if (function) function(texture);
}

static void clientActiveTextureCompat(GLenum texture)
{
	using Function = void (APIENTRYP)(GLenum);
	static Function function = reinterpret_cast<Function>(getProcEither("glClientActiveTextureARB", "glClientActiveTexture"));
	if (function) function(texture);
}

static void multiTexCoord2fCompat(GLenum texture, GLfloat u, GLfloat v)
{
	using Function = void (APIENTRYP)(GLenum, GLfloat, GLfloat);
	static Function function = reinterpret_cast<Function>(getProcEither("glMultiTexCoord2fARB", "glMultiTexCoord2f"));
	if (function) function(texture, u, v);
}

void glGenQueriesARB(GLsizei count, GLuint *queries)
{
	using Function = void (APIENTRYP)(GLsizei, GLuint *);
	static Function function = reinterpret_cast<Function>(getProcEither("glGenQueriesARB", "glGenQueries"));
	if (function) function(count, queries);
	else for (GLsizei i = 0; i < count; ++i) queries[i] = 0;
}

void glBeginQueryARB(GLenum target, GLuint query)
{
	using Function = void (APIENTRYP)(GLenum, GLuint);
	static Function function = reinterpret_cast<Function>(getProcEither("glBeginQueryARB", "glBeginQuery"));
	if (function) function(target, query);
}

void glEndQueryARB(GLenum target)
{
	using Function = void (APIENTRYP)(GLenum);
	static Function function = reinterpret_cast<Function>(getProcEither("glEndQueryARB", "glEndQuery"));
	if (function) function(target);
}

void glGetQueryObjectuivARB(GLuint query, GLenum parameter, GLuint *value)
{
	using Function = void (APIENTRYP)(GLuint, GLenum, GLuint *);
	static Function function = reinterpret_cast<Function>(getProcEither("glGetQueryObjectuivARB", "glGetQueryObjectuiv"));
	if (function) function(query, parameter, value);
	// Fail-OPEN: sin soporte de query -> disponible y "paso 1 sample" (visible),
	// nunca 0 (que marcaria todo como ocluido y borraria el mundo).
	else *value = 1u;
}


#ifdef VITA_PLATFORM
// Fixed-function state the terrain shader (VitaTerrainBatch) reproduces.
static VitaFixedState g_vitaFixed = {false, 0, 0.0f, 1.0f, 1.0f, {0.0f, 0.0f, 0.0f, 0.0f}, false, 0.0f};
const VitaFixedState &fixedState() { return g_vitaFixed; }

// vitaGL reads glMultiTexCoord only between glBegin/glEnd, and ignores texture
// unit 1 unless a coordinate array is attached. Entities are drawn from arrays
// without brightness, so they skipped the lightmap: always full bright, day or
// night, hand included. Such draws now get an array holding the current
// lightmap coordinate. Client arrays are copied at draw time, so one buffer
// serves every draw.
static bool g_vitaUnit1Active = false;
static bool g_vitaLightmapEnabled = false;
static GLshort g_vitaLightmapUV[2] = {240, 240};
static std::vector<GLshort> g_vitaLightmapArray;
static GLshort g_vitaLightmapFilledUV[2] = {-1, -1};
static std::size_t g_vitaLightmapFilled = 0;

static bool vitaBindConstantLightmap(int count)
{
    if (!g_vitaLightmapEnabled || count <= 0)
        return false;
    const std::size_t vertices = static_cast<std::size_t>(count);
    if (g_vitaLightmapFilledUV[0] != g_vitaLightmapUV[0] || g_vitaLightmapFilledUV[1] != g_vitaLightmapUV[1])
        g_vitaLightmapFilled = 0;
    if (g_vitaLightmapFilled < vertices)
    {
        g_vitaLightmapArray.resize(std::max(g_vitaLightmapArray.size(), vertices * 2));
        for (std::size_t i = g_vitaLightmapFilled * 2; i < vertices * 2; i += 2)
        {
            g_vitaLightmapArray[i] = g_vitaLightmapUV[0];
            g_vitaLightmapArray[i + 1] = g_vitaLightmapUV[1];
        }
        g_vitaLightmapFilled = vertices;
        g_vitaLightmapFilledUV[0] = g_vitaLightmapUV[0];
        g_vitaLightmapFilledUV[1] = g_vitaLightmapUV[1];
    }
    clientActiveTextureCompat(GL_TEXTURE1_ARB);
    glTexCoordPointer(2, GL_SHORT, 0, g_vitaLightmapArray.data());
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    clientActiveTextureCompat(GL_TEXTURE0_ARB);
    return true;
}

bool vitaDisplayListRecordingImpl() { return g_vitaRecording > 0; }
void vitaLightmapCoordImpl(float out[2]) { out[0] = g_vitaLightmapUV[0]; out[1] = g_vitaLightmapUV[1]; }

static void vitaUnbindConstantLightmap()
{
    clientActiveTextureCompat(GL_TEXTURE1_ARB);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_SHORT, 0, nullptr);
    clientActiveTextureCompat(GL_TEXTURE0_ARB);
}
#endif

void renderEnable(RenderCapability capability)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderEnable(capability));
    switch (capability)
    {
        case RenderCapability::Texture2D:
#ifdef VITA_PLATFORM
            if (g_vitaUnit1Active)
                g_vitaLightmapEnabled = true;
#endif
            glEnable(GL_TEXTURE_2D);
            break;
        case RenderCapability::ColorMaterial:
#if PLATFORM_MODEL_GPU_BATCH
            vitaModelTrackColorMaterial(true);
#endif
            glEnable(GL_COLOR_MATERIAL);
            break;
        case RenderCapability::CullFace: glEnable(GL_CULL_FACE); break;
        case RenderCapability::AlphaTest:
#ifdef VITA_PLATFORM
            g_vitaFixed.alphaTest = true;
#endif
            glEnable(GL_ALPHA_TEST);
            break;
        case RenderCapability::Blend: glEnable(GL_BLEND); break;
        case RenderCapability::DepthTest: glEnable(GL_DEPTH_TEST); break;
        case RenderCapability::Fog:
#ifdef VITA_PLATFORM
            g_vitaFixed.fog = true;
#endif
            glEnable(GL_FOG);
            break;
        case RenderCapability::Lighting: glEnable(GL_LIGHTING); break;
        case RenderCapability::Normalize: glEnable(GL_NORMALIZE); break;
#ifdef VITA_PLATFORM
        case RenderCapability::RescaleNormal: glEnable(GL_NORMALIZE); break;
#else
        case RenderCapability::RescaleNormal: glEnable(GL_RESCALE_NORMAL); break;
#endif
        case RenderCapability::Light0: glEnable(GL_LIGHT0); break;
        case RenderCapability::Light1: glEnable(GL_LIGHT1); break;
        case RenderCapability::PolygonOffsetFill: glEnable(GL_POLYGON_OFFSET_FILL); break;
    }
}

void renderDisable(RenderCapability capability)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderDisable(capability));
    switch (capability)
    {
        case RenderCapability::Texture2D:
#ifdef VITA_PLATFORM
            if (g_vitaUnit1Active)
                g_vitaLightmapEnabled = false;
#endif
            glDisable(GL_TEXTURE_2D);
            break;
        case RenderCapability::ColorMaterial:
#if PLATFORM_MODEL_GPU_BATCH
            vitaModelTrackColorMaterial(false);
#endif
            glDisable(GL_COLOR_MATERIAL);
            break;
        case RenderCapability::CullFace: glDisable(GL_CULL_FACE); break;
        case RenderCapability::AlphaTest:
#ifdef VITA_PLATFORM
            g_vitaFixed.alphaTest = false;
#endif
            glDisable(GL_ALPHA_TEST);
            break;
        case RenderCapability::Blend: glDisable(GL_BLEND); break;
        case RenderCapability::DepthTest: glDisable(GL_DEPTH_TEST); break;
        case RenderCapability::Fog:
#ifdef VITA_PLATFORM
            g_vitaFixed.fog = false;
#endif
            glDisable(GL_FOG);
            break;
        case RenderCapability::Lighting: glDisable(GL_LIGHTING); break;
        case RenderCapability::Normalize: glDisable(GL_NORMALIZE); break;
#ifdef VITA_PLATFORM
        case RenderCapability::RescaleNormal: glDisable(GL_NORMALIZE); break;
#else
        case RenderCapability::RescaleNormal: glDisable(GL_RESCALE_NORMAL); break;
#endif
        case RenderCapability::Light0: glDisable(GL_LIGHT0); break;
        case RenderCapability::Light1: glDisable(GL_LIGHT1); break;
        case RenderCapability::PolygonOffsetFill: glDisable(GL_POLYGON_OFFSET_FILL); break;
    }
}

void renderBlendFunc(RenderBlendFactor source, RenderBlendFactor destination)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderBlendFunc(source, destination));
    auto glFactor = [](RenderBlendFactor factor) -> GLenum
    {
        switch (factor)
        {
            case RenderBlendFactor::Zero: return GL_ZERO;
            case RenderBlendFactor::One: return GL_ONE;
            case RenderBlendFactor::SrcColor: return GL_SRC_COLOR;
            case RenderBlendFactor::OneMinusSrcColor: return GL_ONE_MINUS_SRC_COLOR;
            case RenderBlendFactor::SrcAlpha: return GL_SRC_ALPHA;
            case RenderBlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
            case RenderBlendFactor::DstAlpha: return GL_DST_ALPHA;
            case RenderBlendFactor::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
            case RenderBlendFactor::DstColor: return GL_DST_COLOR;
            case RenderBlendFactor::OneMinusDstColor: return GL_ONE_MINUS_DST_COLOR;
        }
        return GL_ONE;
    };
    glBlendFunc(glFactor(source), glFactor(destination));
}

void renderDepthMask(bool enabled)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderDepthMask(enabled));
    glDepthMask(enabled ? GL_TRUE : GL_FALSE);
}

void renderDepthFunc(RenderCompare function)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderDepthFunc(function));
    auto glCompare = [](RenderCompare compare) -> GLenum
    {
        switch (compare)
        {
            case RenderCompare::Never: return GL_NEVER;
            case RenderCompare::Less: return GL_LESS;
            case RenderCompare::Equal: return GL_EQUAL;
            case RenderCompare::LessEqual: return GL_LEQUAL;
            case RenderCompare::Greater: return GL_GREATER;
            case RenderCompare::NotEqual: return GL_NOTEQUAL;
            case RenderCompare::GreaterEqual: return GL_GEQUAL;
            case RenderCompare::Always: return GL_ALWAYS;
        }
        return GL_ALWAYS;
    };
    glDepthFunc(glCompare(function));
}

void renderAlphaFunc(RenderCompare function, float reference)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderAlphaFunc(function, reference));
    auto glCompare = [](RenderCompare compare) -> GLenum
    {
        switch (compare)
        {
            case RenderCompare::Never: return GL_NEVER;
            case RenderCompare::Less: return GL_LESS;
            case RenderCompare::Equal: return GL_EQUAL;
            case RenderCompare::LessEqual: return GL_LEQUAL;
            case RenderCompare::Greater: return GL_GREATER;
            case RenderCompare::NotEqual: return GL_NOTEQUAL;
            case RenderCompare::GreaterEqual: return GL_GEQUAL;
            case RenderCompare::Always: return GL_ALWAYS;
        }
        return GL_ALWAYS;
    };
#ifdef VITA_PLATFORM
    g_vitaFixed.alphaRef = reference;
#endif
#if PLATFORM_MODEL_GPU_BATCH
    vitaModelTrackAlphaFunc(glCompare(function));
#endif
    glAlphaFunc(glCompare(function), reference);
}

void renderCullFace(RenderFace face)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderCullFace(face));
    switch (face)
    {
        case RenderFace::Front: glCullFace(GL_FRONT); break;
        case RenderFace::Back: glCullFace(GL_BACK); break;
        case RenderFace::FrontAndBack: glCullFace(GL_FRONT_AND_BACK); break;
    }
}

void renderColorMask(bool red, bool green, bool blue, bool alpha)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderColorMask(red, green, blue, alpha));
    glColorMask(red ? GL_TRUE : GL_FALSE, green ? GL_TRUE : GL_FALSE, blue ? GL_TRUE : GL_FALSE, alpha ? GL_TRUE : GL_FALSE);
}

void renderBindTexture(int texture)
{
#ifdef VITA_PLATFORM
    VITA_MODEL_FLUSH();
    if (g_vitaRecording > 0)
        vitaRecord([=] { PcOpenGlRenderBackend::renderBindTexture(texture); });
#endif
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
}

void renderSetActiveTextureUnit(int textureUnit)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderSetActiveTextureUnit(textureUnit));
#ifdef VITA_PLATFORM
    g_vitaUnit1Active = textureUnit == GL_TEXTURE1_ARB;
#endif
    activeTextureCompat(static_cast<GLenum>(textureUnit));
}

void renderSetClientActiveTextureUnit(int textureUnit)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderSetClientActiveTextureUnit(textureUnit));
    clientActiveTextureCompat(static_cast<GLenum>(textureUnit));
}

void renderSetMultiTextureCoord(int textureUnit, float u, float v)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderSetMultiTextureCoord(textureUnit, u, v));
#ifdef VITA_PLATFORM
    if (textureUnit == GL_TEXTURE1_ARB)
    {
        g_vitaLightmapUV[0] = static_cast<GLshort>(u);
        g_vitaLightmapUV[1] = static_cast<GLshort>(v);
    }
    // Outside glBegin/glEnd vitaGL rejects the call (GL_INVALID_OPERATION).
#else
    multiTexCoord2fCompat(static_cast<GLenum>(textureUnit), u, v);
#endif
}

void renderSetLightmapColors(const std::uint32_t* colors, int count)
{
    (void)colors;
    (void)count;
}

void renderColor4f(float r, float g, float b, float a)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderColor4f(r, g, b, a));
    glColor4f(r, g, b, a);
}

void renderColor3f(float r, float g, float b)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderColor3f(r, g, b));
    glColor3f(r, g, b);
}

void renderNormal3f(float x, float y, float z)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderNormal3f(x, y, z));
    glNormal3f(x, y, z);
}

void renderGenerateTextures(int count, int *textures)
{
    if (count <= 0 || textures == nullptr)
        return;
    glGenTextures(static_cast<GLsizei>(count), reinterpret_cast<GLuint *>(textures));
}

void renderDeleteTextures(int count, const int *textures)
{
    if (count <= 0 || textures == nullptr)
        return;
    glDeleteTextures(static_cast<GLsizei>(count), reinterpret_cast<const GLuint *>(textures));
}

void renderTextureSubImageRgba(int level, int x, int y, int width, int height, const void *pixels)
{
    glTexSubImage2D(GL_TEXTURE_2D, level, x, y, width, height,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

// Specifies one mip level of the bound texture from tightly packed RGBA8.
//
// The desktop internal format stays GL_RGBA -- the third glTexImage2D argument
// is what decides the uploaded cost, and a desktop driver stores that as 32 bits
// per texel. The console backends pick their own storage (PS2: CT16, or PSMT8
// plus a CT16 CLUT when PS2_ENABLE_PSMT8 is on), which is why they take the
// pixels rather than a format enum.
void renderTextureImageRgba(int level, int width, int height, const void *pixels)
{
    VITA_PERF_COUNT(g_vitaPerfTexUploads, 1);
    glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

// Only the desktop backend maps these one-to-one onto texture state. The Wii
// takes all three in wii_native_texture_begin_upload, before any level exists,
// so there is nothing left to program by the time this is called; the PS2
// resolves wrap per primitive from the UV bounds instead of per texture. See
// ps2_texture_set_parameters for why that is the better answer there.
void renderTextureParameters(bool blur, bool mipmaps, bool clamp)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    blur ? GL_LINEAR : (mipmaps ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, blur ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp ? GL_CLAMP : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp ? GL_CLAMP : GL_REPEAT);
}

void renderApplyTextureQuality(bool blur, int mipmapLevel, bool mipmapLinear, int anisotropy)
{
    GLenum minFilter = blur ? GL_LINEAR : GL_NEAREST;
    if (mipmapLevel > 0)
    {
        if (blur)
            minFilter = GL_LINEAR_MIPMAP_LINEAR;
        else
            minFilter = mipmapLinear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, blur ? GL_LINEAR : GL_NEAREST);
    // Only the selected chain is uploaded. Clamp sampling to that chain so a
    // texture does not become incomplete by implicitly requiring lower levels.
    if (desktopHasOpenGL12())
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipmapLevel > 0 ? mipmapLevel : 0);
    }

    if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") == SDL_TRUE)
    {
        GLfloat maximum = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum);
        const GLfloat requested = static_cast<GLfloat>(anisotropy < 1 ? 1 : anisotropy);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                        requested < maximum ? requested : maximum);
    }
}

int renderGetMaxAnisotropy()
{
    if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") != SDL_TRUE)
        return 1;
    GLfloat maximum = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum);
    return maximum > 1.0f ? static_cast<int>(maximum) : 1;
}

int renderGetMaxSamples()
{
    int buffers = 0;
    int samples = 0;
    SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &buffers);
    SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &samples);
    return buffers > 0 && samples > 0 ? samples : 0;
}

bool renderTextureBeginUpload(int, int, int, int, bool, bool, bool, bool) { return true; }
bool renderTextureIsValid(int texture) { return texture > 0; }
void renderResetResources() {}


void renderFogf(RenderFogParameter parameter, float value)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderFogf(parameter, value));
    GLenum glParameter = GL_FOG_DENSITY;
    switch (parameter)
    {
        case RenderFogParameter::Density: glParameter = GL_FOG_DENSITY; break;
        case RenderFogParameter::Start: glParameter = GL_FOG_START; break;
        case RenderFogParameter::End: glParameter = GL_FOG_END; break;
        case RenderFogParameter::Mode: glParameter = GL_FOG_MODE; break;
        case RenderFogParameter::Color: glParameter = GL_FOG_COLOR; break;
        case RenderFogParameter::DistanceMode:
#ifdef GL_FOG_DISTANCE_MODE_NV
            glParameter = GL_FOG_DISTANCE_MODE_NV;
#else
            return;
#endif
            break;
    }
#ifdef VITA_PLATFORM
    switch (parameter)
    {
        case RenderFogParameter::Density: g_vitaFixed.fogDensity = value; break;
        case RenderFogParameter::Start: g_vitaFixed.fogStart = value; break;
        case RenderFogParameter::End: g_vitaFixed.fogEnd = value; break;
        case RenderFogParameter::Mode:
            g_vitaFixed.fogMode = value == GL_EXP ? 1 : value == GL_EXP2 ? 2 : 0;
            break;
        default: break;
    }
#endif
    glFogf(glParameter, value);
}

void renderFogi(RenderFogParameter parameter, RenderFogMode value)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderFogi(parameter, value));
    const GLenum glParameter = parameter == RenderFogParameter::DistanceMode ? GL_FOG_DISTANCE_MODE_NV : GL_FOG_MODE;
    GLint glMode = GL_LINEAR;
    switch (value)
    {
        case RenderFogMode::Exp: glMode = GL_EXP; break;
        case RenderFogMode::Exp2: glMode = GL_EXP2; break;
        case RenderFogMode::Linear: glMode = GL_LINEAR; break;
        case RenderFogMode::EyeRadial: glMode = GL_EYE_RADIAL_NV; break;
    }
#ifdef VITA_PLATFORM
    if (glParameter == GL_FOG_MODE)
        g_vitaFixed.fogMode = glMode == GL_EXP ? 1 : glMode == GL_EXP2 ? 2 : 0;
#endif
    glFogi(glParameter, glMode);
}

void renderFogColor(const float* values)
{
#ifdef VITA_PLATFORM
    VITA_MODEL_FLUSH();
    if (g_vitaRecording > 0)
    {
        const float v0 = values[0], v1 = values[1], v2 = values[2], v3 = values[3];
        vitaRecord([=] { const float copy[4] = {v0, v1, v2, v3}; PcOpenGlRenderBackend::renderFogColor(copy); });
        return;
    }
#endif
#ifdef VITA_PLATFORM
    std::memcpy(g_vitaFixed.fogColor, values, sizeof(g_vitaFixed.fogColor));
#endif
    glFogfv(GL_FOG_COLOR, values);
}

void renderLightfv(int lightIndex, RenderLightParameter parameter, const float* values)
{
#ifdef VITA_PLATFORM
    VITA_MODEL_FLUSH();
    if (g_vitaRecording > 0)
    {
        const float v0 = values[0], v1 = values[1], v2 = values[2], v3 = values[3];
        vitaRecord([=] { const float copy[4] = {v0, v1, v2, v3}; PcOpenGlRenderBackend::renderLightfv(lightIndex, parameter, copy); });
        return;
    }
#endif
    GLenum glParameter = GL_POSITION;
    switch (parameter)
    {
        case RenderLightParameter::Ambient: glParameter = GL_AMBIENT; break;
        case RenderLightParameter::Diffuse: glParameter = GL_DIFFUSE; break;
        case RenderLightParameter::Specular: glParameter = GL_SPECULAR; break;
        case RenderLightParameter::Position: glParameter = GL_POSITION; break;
    }
#if PLATFORM_MODEL_GPU_BATCH
    vitaModelTrackLight(lightIndex, parameter, values);
#endif
    glLightfv(GL_LIGHT0 + lightIndex, glParameter, values);
}

void renderLightModelAmbient(const float* values)
{
#ifdef VITA_PLATFORM
    VITA_MODEL_FLUSH();
    if (g_vitaRecording > 0)
    {
        const float v0 = values[0], v1 = values[1], v2 = values[2], v3 = values[3];
        vitaRecord([=] { const float copy[4] = {v0, v1, v2, v3}; PcOpenGlRenderBackend::renderLightModelAmbient(copy); });
        return;
    }
#endif
#if PLATFORM_MODEL_GPU_BATCH
    vitaModelTrackLightModelAmbient(values);
#endif
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, values);
}

void renderColorMaterial(RenderFace face, RenderColorMaterialMode mode)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderColorMaterial(face, mode));
    GLenum glFace = face == RenderFace::Front ? GL_FRONT : face == RenderFace::Back ? GL_BACK : GL_FRONT_AND_BACK;
    GLenum glMode = mode == RenderColorMaterialMode::Ambient ? GL_AMBIENT : GL_AMBIENT_AND_DIFFUSE;
    glColorMaterial(glFace, glMode);
}

void renderShadeModel(RenderShadeModel model)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderShadeModel(model));
    glShadeModel(model == RenderShadeModel::Smooth ? GL_SMOOTH : GL_FLAT);
}

void renderClear(unsigned int mask)
{
    VITA_MODEL_FLUSH();
    glClear(static_cast<GLbitfield>(mask));
}

void renderFinishGpu()
{
    glFinish();
}

// The GL backend has no separate submit step: SDL_GL_SwapWindow is the frame
// boundary and the driver already pipelines behind it.
void renderSubmitFrame()
{
}

void renderClearColor(float r, float g, float b, float a)
{
    glClearColor(r, g, b, a);
}

void renderClearDepth(double depth)
{
    glClearDepth(depth);
}

void renderPolygonOffset(float factor, float units)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderPolygonOffset(factor, units));
    glPolygonOffset(factor, units);
}

void renderLineWidth(float width)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderLineWidth(width));
    glLineWidth(width);
}

void renderViewport(int x, int y, int width, int height)
{
    glViewport(x, y, width, height);
}

void renderGetViewport(int* values)
{
    if (values != nullptr)
        glGetIntegerv(GL_VIEWPORT, values);
}

void renderGetMatrix(RenderMatrixQuery query, float* values)
{
    GLenum glQuery = query == RenderMatrixQuery::Projection ? GL_PROJECTION_MATRIX : query == RenderMatrixQuery::Texture ? GL_TEXTURE_MATRIX : GL_MODELVIEW_MATRIX;
    glGetFloatv(glQuery, values);
}

const unsigned char* renderGetString(RenderStringQuery query)
{
    GLenum glQuery = query == RenderStringQuery::Vendor ? GL_VENDOR : query == RenderStringQuery::Renderer ? GL_RENDERER : query == RenderStringQuery::Version ? GL_VERSION : GL_EXTENSIONS;
    return glGetString(glQuery);
}

bool renderSupportsFeature(RenderFeature feature)
{
    switch (feature)
    {
        case RenderFeature::FancyFogDistance:
            return SDL_GL_ExtensionSupported("GL_NV_fog_distance") == SDL_TRUE;
        case RenderFeature::OcclusionQuery:
            return SDL_GL_ExtensionSupported("GL_ARB_occlusion_query") == SDL_TRUE;
        case RenderFeature::Mipmaps:
            return desktopHasOpenGL12();
        case RenderFeature::AnisotropicFiltering:
            return SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") == SDL_TRUE;
        case RenderFeature::MultisampleAntialiasing:
            return renderGetMaxSamples() > 0;
    }
    return false;
}

unsigned int renderGetError()
{
    return glGetError();
}

void renderFogHint(RenderHintMode mode)
{
    glHint(GL_FOG_HINT, mode == RenderHintMode::Nicest ? GL_NICEST : GL_FASTEST);
}


void renderMatrixMode(RenderMatrixMode mode)
{
    VITA_RECORD(PcOpenGlRenderBackend::renderMatrixMode(mode));
    switch (mode)
    {
        case RenderMatrixMode::ModelView: glMatrixMode(GL_MODELVIEW); break;
        case RenderMatrixMode::Projection: glMatrixMode(GL_PROJECTION); break;
        case RenderMatrixMode::Texture: glMatrixMode(GL_TEXTURE); break;
    }
}

void renderLoadIdentity()
{
    VITA_RECORD_MATRIX(PcOpenGlRenderBackend::renderLoadIdentity());
    glLoadIdentity();
}

void renderPushMatrix()
{
    VITA_RECORD_MATRIX(PcOpenGlRenderBackend::renderPushMatrix());
    glPushMatrix();
}

void renderPopMatrix()
{
    VITA_RECORD_MATRIX(PcOpenGlRenderBackend::renderPopMatrix());
    glPopMatrix();
}

void renderTranslate(float x, float y, float z)
{
    VITA_RECORD_MATRIX(PcOpenGlRenderBackend::renderTranslate(x, y, z));
    glTranslatef(x, y, z);
}

void renderRotate(float angle, float x, float y, float z)
{
    VITA_RECORD_MATRIX(PcOpenGlRenderBackend::renderRotate(angle, x, y, z));
    glRotatef(angle, x, y, z);
}

void renderScale(float x, float y, float z)
{
    VITA_RECORD_MATRIX(PcOpenGlRenderBackend::renderScale(x, y, z));
    glScalef(x, y, z);
}

void renderScaleDouble(double x, double y, double z)
{
    VITA_RECORD_MATRIX(PcOpenGlRenderBackend::renderScaleDouble(x, y, z));
    glScaled(x, y, z);
}

void renderFrustum(double left, double right, double bottom, double top, double nearValue, double farValue)
{
    VITA_RECORD_MATRIX(PcOpenGlRenderBackend::renderFrustum(left, right, bottom, top, nearValue, farValue));
    glFrustum(left, right, bottom, top, nearValue, farValue);
}

void renderOrtho(double left, double right, double bottom, double top, double nearValue, double farValue)
{
    VITA_RECORD_MATRIX(PcOpenGlRenderBackend::renderOrtho(left, right, bottom, top, nearValue, farValue));
    glOrtho(left, right, bottom, top, nearValue, farValue);
}



#ifdef VITA_PLATFORM
int renderGenerateDisplayLists(int count)
{
    if (count <= 0)
        return 0;
    const int first = static_cast<int>(g_vitaLists.size());
    g_vitaLists.resize(g_vitaLists.size() + static_cast<std::size_t>(count));
    return first;
}

void renderDeleteDisplayLists(int first, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const int list = first + i;
        if (list > 0 && list < static_cast<int>(g_vitaLists.size()))
            std::vector<std::function<void()>>().swap(g_vitaLists[static_cast<std::size_t>(list)]);
    }
}

void renderBeginDisplayList(int list)
{
    if (list <= 0 || list >= static_cast<int>(g_vitaLists.size()))
        return;
    g_vitaLists[static_cast<std::size_t>(list)].clear();
    g_vitaRecordingStack.push_back(g_vitaRecording);
    g_vitaRecording = list;
}

void renderEndDisplayList()
{
    if (g_vitaRecordingStack.empty())
    {
        g_vitaRecording = 0;
        return;
    }
    g_vitaRecording = g_vitaRecordingStack.back();
    g_vitaRecordingStack.pop_back();
}

void renderCallDisplayList(int list)
{
    if (list <= 0 || list >= static_cast<int>(g_vitaLists.size()))
        return;
    VITA_RECORD(PcOpenGlRenderBackend::renderCallDisplayList(list));
    for (const std::function<void()>& command : g_vitaLists[static_cast<std::size_t>(list)])
        command();
}

void renderCallDisplayLists(int count, const int* lists)
{
    for (int i = 0; i < count; ++i)
        PcOpenGlRenderBackend::renderCallDisplayList(lists[i]);
}
#else
int renderGenerateDisplayLists(int count)
{
    return static_cast<int>(glGenLists(static_cast<GLsizei>(count)));
}

void renderDeleteDisplayLists(int first, int count)
{
    glDeleteLists(static_cast<GLuint>(first), static_cast<GLsizei>(count));
}

void renderBeginDisplayList(int list)
{
    glNewList(static_cast<GLuint>(list), GL_COMPILE);
}

void renderEndDisplayList()
{
    glEndList();
}

void renderCallDisplayList(int list)
{
    glCallList(static_cast<GLuint>(list));
}

void renderCallDisplayLists(int count, const int* lists)
{
    glCallLists(static_cast<GLsizei>(count), GL_INT, lists);
}
#endif

void renderGenerateOcclusionQueries(int count, int* queries)
{
    glGenQueriesARB(static_cast<GLsizei>(count), reinterpret_cast<GLuint*>(queries));
}

void renderBeginOcclusionQuery(int query)
{
    VITA_MODEL_FLUSH();
    glBeginQueryARB(GL_SAMPLES_PASSED_ARB, static_cast<GLuint>(query));
}

void renderEndOcclusionQuery()
{
    glEndQueryARB(GL_SAMPLES_PASSED_ARB);
}

bool renderOcclusionQueryResultAvailable(int query)
{
    GLuint value = 0;
    glGetQueryObjectuivARB(static_cast<GLuint>(query), GL_QUERY_RESULT_AVAILABLE_ARB, &value);
    return value != 0;
}

unsigned int renderOcclusionQueryResult(int query)
{
    GLuint value = 0;
    glGetQueryObjectuivARB(static_cast<GLuint>(query), GL_QUERY_RESULT_ARB, &value);
    return static_cast<unsigned int>(value);
}

bool renderReadPixelsRgb(int x, int y, int width, int height, void* pixels)
{
    if (pixels == nullptr || width <= 0 || height <= 0) return false;
    VITA_MODEL_FLUSH();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    return true;
}

void renderSetLegacyPresentationGamma(bool)
{
    // Desktop uses the real GLSL framebuffer gamma pass.
}

bool renderCopyFramebufferToBoundTexture(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    VITA_MODEL_FLUSH();
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, y, width, height);
    return glGetError() == GL_NO_ERROR;
}



#ifdef VITA_PLATFORM
struct VitaBufferedMesh
{
    GLuint buffer = 0;
    RenderInterleavedMesh layout;

    ~VitaBufferedMesh()
    {
        if (buffer != 0)
            glDeleteBuffers(1, &buffer);
    }
};

static std::shared_ptr<VitaBufferedMesh> vitaUploadMesh(const RenderCapturedMesh& mesh)
{
    if (mesh.empty() || mesh.stride <= 0)
        return nullptr;
    std::shared_ptr<VitaBufferedMesh> out = std::make_shared<VitaBufferedMesh>();
    glGenBuffers(1, &out->buffer);
    if (out->buffer == 0)
        return nullptr;
    glBindBuffer(GL_ARRAY_BUFFER, out->buffer);
    VITA_PERF_COUNT(g_vitaPerfBufferBytes, static_cast<unsigned>(mesh.vertexCount * mesh.stride));
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(static_cast<std::size_t>(mesh.vertexCount) * static_cast<std::size_t>(mesh.stride)),
        mesh.raw.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    out->layout.stride = mesh.stride;
    out->layout.count = mesh.vertexCount;
    out->layout.primitive = mesh.primitive;
    out->layout.positionShort = mesh.positionShort;
    out->layout.hasTexture = mesh.hasTexture;
    out->layout.texCoordOffset = mesh.texCoordOffset;
    out->layout.hasColor = mesh.hasColor;
    out->layout.colorOffset = mesh.colorOffset;
    out->layout.hasNormals = mesh.hasNormals;
    out->layout.normalOffset = mesh.normalOffset;
    out->layout.hasBrightness = mesh.hasBrightness;
    out->layout.brightnessOffset = mesh.brightnessOffset;
    return out;
}

static void vitaDrawBufferedMesh(const VitaBufferedMesh& buffered)
{
    const RenderInterleavedMesh& mesh = buffered.layout;
    auto pointerForOffset = [](int offset) -> const void*
    {
        return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset));
    };

    // Before the VBO bind: vitaGL takes the buffer bound when the pointer is set.
    const bool constantLightmap = !mesh.hasBrightness && vitaBindConstantLightmap(mesh.count);
    glBindBuffer(GL_ARRAY_BUFFER, buffered.buffer);
    clientActiveTextureCompat(GL_TEXTURE0_ARB);
    if (mesh.hasTexture)
    {
        glTexCoordPointer(2, GL_FLOAT, mesh.stride, pointerForOffset(mesh.texCoordOffset));
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    if (mesh.hasBrightness)
    {
        clientActiveTextureCompat(GL_TEXTURE1_ARB);
        glTexCoordPointer(2, GL_SHORT, mesh.stride, pointerForOffset(mesh.brightnessOffset));
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        clientActiveTextureCompat(GL_TEXTURE0_ARB);
    }
    if (mesh.hasColor)
    {
        glColorPointer(4, GL_UNSIGNED_BYTE, mesh.stride, pointerForOffset(mesh.colorOffset));
        glEnableClientState(GL_COLOR_ARRAY);
    }
    if (mesh.hasNormals)
    {
        glNormalPointer(GL_BYTE, mesh.stride, pointerForOffset(mesh.normalOffset));
        glEnableClientState(GL_NORMAL_ARRAY);
    }

    glVertexPointer(3, mesh.positionShort ? GL_SHORT : GL_FLOAT, mesh.stride, pointerForOffset(0));
    glEnableClientState(GL_VERTEX_ARRAY);
    VITA_PERF_COUNT(g_vitaPerfDraws, 1);
    glDrawArrays(static_cast<GLenum>(renderPrimitiveValue(mesh.primitive)), 0, mesh.count);
    // Unbind first: every pointer reset below must capture no buffer, or it
    // keeps naming this VBO (the TEX1 reset in vitaUnbindConstantLightmap did).
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableClientState(GL_VERTEX_ARRAY);
    if (constantLightmap)
        vitaUnbindConstantLightmap();

    if (mesh.hasTexture)
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (mesh.hasBrightness)
    {
        clientActiveTextureCompat(GL_TEXTURE1_ARB);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        clientActiveTextureCompat(GL_TEXTURE0_ARB);
    }
    if (mesh.hasColor)
        glDisableClientState(GL_COLOR_ARRAY);
    if (mesh.hasNormals)
        glDisableClientState(GL_NORMAL_ARRAY);

    // Not redundant: with lighting on, vitaGL feeds the material from the
    // colour/normal pointers it last saw, so a stale VBO pointer recolours
    // the next lit draw (entities turned red/blue without these).
    glVertexPointer(3, GL_FLOAT, 0, nullptr);
    if (mesh.hasTexture)
        glTexCoordPointer(2, GL_FLOAT, 0, nullptr);
    if (mesh.hasBrightness)
    {
        clientActiveTextureCompat(GL_TEXTURE1_ARB);
        glTexCoordPointer(2, GL_SHORT, 0, nullptr);
        clientActiveTextureCompat(GL_TEXTURE0_ARB);
    }
    if (mesh.hasColor)
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, nullptr);
    if (mesh.hasNormals)
        glNormalPointer(GL_BYTE, 0, nullptr);
}
#endif

bool renderDrawInterleaved(const RenderInterleavedMesh& mesh)
{
    if (mesh.data == nullptr || mesh.stride <= 0 || mesh.count <= 0)
        return false;
    VITA_MODEL_FLUSH();
#ifdef VITA_PLATFORM
    if (g_vitaRecording > 0)
    {
        std::shared_ptr<RenderCapturedMesh> captured = std::make_shared<RenderCapturedMesh>();
        if (!PcOpenGlRenderBackend::renderCaptureInterleaved(mesh, *captured, false))
            return false;
        std::shared_ptr<VitaBufferedMesh> buffered = vitaUploadMesh(*captured);
        if (!buffered)
            return false;
        vitaRecord([buffered] { vitaDrawBufferedMesh(*buffered); });
        return true;
    }
#endif

    auto pointerForOffset = [&](int offset) -> const void*
    {
        return reinterpret_cast<const unsigned char*>(mesh.data) + offset;
    };

#ifdef VITA_PLATFORM
    const bool constantLightmap = !mesh.hasBrightness && vitaBindConstantLightmap(mesh.first + mesh.count);
#endif
    clientActiveTextureCompat(GL_TEXTURE0_ARB);
    if (mesh.hasTexture)
    {
        glTexCoordPointer(2, GL_FLOAT, mesh.stride, pointerForOffset(mesh.texCoordOffset));
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    if (mesh.hasBrightness)
    {
        clientActiveTextureCompat(GL_TEXTURE1_ARB);
        glTexCoordPointer(2, GL_SHORT, mesh.stride, pointerForOffset(mesh.brightnessOffset));
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        clientActiveTextureCompat(GL_TEXTURE0_ARB);
    }
    if (mesh.hasColor)
    {
        glColorPointer(4, GL_UNSIGNED_BYTE, mesh.stride, pointerForOffset(mesh.colorOffset));
        glEnableClientState(GL_COLOR_ARRAY);
    }
    if (mesh.hasNormals)
    {
        glNormalPointer(GL_BYTE, mesh.stride, pointerForOffset(mesh.normalOffset));
        glEnableClientState(GL_NORMAL_ARRAY);
    }

    glVertexPointer(3, mesh.positionShort ? GL_SHORT : GL_FLOAT, mesh.stride, pointerForOffset(0));
    glEnableClientState(GL_VERTEX_ARRAY);
    VITA_PERF_COUNT(g_vitaPerfDraws, 1);
    glDrawArrays(static_cast<GLenum>(renderPrimitiveValue(mesh.primitive)), mesh.first, mesh.count);
    glDisableClientState(GL_VERTEX_ARRAY);
#ifdef VITA_PLATFORM
    if (constantLightmap)
        vitaUnbindConstantLightmap();
#endif

    if (mesh.hasTexture)
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (mesh.hasBrightness)
    {
        clientActiveTextureCompat(GL_TEXTURE1_ARB);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        clientActiveTextureCompat(GL_TEXTURE0_ARB);
    }
    if (mesh.hasColor)
        glDisableClientState(GL_COLOR_ARRAY);
    if (mesh.hasNormals)
        glDisableClientState(GL_NORMAL_ARRAY);

    return true;
}


bool renderCaptureInterleaved(const RenderInterleavedMesh& mesh, RenderCapturedMesh& out, bool append)
{
    if (mesh.data == nullptr || mesh.stride <= 0 || mesh.count <= 0)
        return false;
    if (!append)
        out.clear();
    if (!out.empty() && (out.stride != mesh.stride || out.primitive != mesh.primitive ||
        out.positionShort != mesh.positionShort ||
        out.hasTexture != mesh.hasTexture || (mesh.hasTexture && out.texCoordOffset != mesh.texCoordOffset) ||
        out.hasColor != mesh.hasColor || (mesh.hasColor && out.colorOffset != mesh.colorOffset) ||
        out.hasNormals != mesh.hasNormals || (mesh.hasNormals && out.normalOffset != mesh.normalOffset) ||
        out.hasBrightness != mesh.hasBrightness || (mesh.hasBrightness && out.brightnessOffset != mesh.brightnessOffset)))
        return false;
    if (out.empty()) {
        out.stride = mesh.stride; out.primitive = mesh.primitive; out.positionShort = mesh.positionShort;
        out.hasTexture = mesh.hasTexture; out.texCoordOffset = mesh.texCoordOffset;
        out.hasColor = mesh.hasColor; out.colorOffset = mesh.colorOffset;
        out.hasNormals = mesh.hasNormals; out.normalOffset = mesh.normalOffset;
        out.hasBrightness = mesh.hasBrightness; out.brightnessOffset = mesh.brightnessOffset;
    }
    const unsigned char* src = static_cast<const unsigned char*>(mesh.data) + (size_t)mesh.first * mesh.stride;
    const size_t bytes = (size_t)mesh.count * mesh.stride;
    // insert, not resize + memcpy: resize zero-fills the new ints first (a
    // whole extra write pass over every captured byte).
    const std::int32_t* srcInts = reinterpret_cast<const std::int32_t*>(src);
    const size_t wholeInts = bytes / 4u;
    out.raw.insert(out.raw.end(), srcInts, srcInts + wholeInts);
    if (bytes & 3u)
    {
        std::int32_t tail = 0;
        std::memcpy(&tail, src + wholeInts * 4u, bytes & 3u);
        out.raw.push_back(tail);
    }
    out.vertexCount += mesh.count;
    return true;
}

bool renderDrawCaptured(const RenderCapturedMesh& mesh)
{
    if (mesh.empty()) return false;
#ifdef VITA_PLATFORM
    if (g_vitaRecording > 0)
    {
        std::shared_ptr<VitaBufferedMesh> buffered = vitaUploadMesh(mesh);
        if (!buffered)
            return false;
        vitaRecord([buffered] { vitaDrawBufferedMesh(*buffered); });
        return true;
    }
#endif
    RenderInterleavedMesh view;
    view.data = mesh.raw.data(); view.stride = mesh.stride; view.count = mesh.vertexCount;
    view.primitive = mesh.primitive; view.positionShort = mesh.positionShort;
    view.hasTexture = mesh.hasTexture; view.texCoordOffset = mesh.texCoordOffset;
    view.hasColor = mesh.hasColor; view.colorOffset = mesh.colorOffset;
    view.hasNormals = mesh.hasNormals; view.normalOffset = mesh.normalOffset;
    view.hasBrightness = mesh.hasBrightness; view.brightnessOffset = mesh.brightnessOffset;
    return PcOpenGlRenderBackend::renderDrawInterleaved(view);
}


}

#ifdef VITA_PLATFORM
const VitaFixedState &vitaFixedState() { return PcOpenGlRenderBackend::fixedState(); }
#endif
#if PLATFORM_MODEL_GPU_BATCH
bool vitaDisplayListRecording() { return PcOpenGlRenderBackend::vitaDisplayListRecordingImpl(); }
void vitaLightmapCoord(float out[2]) { PcOpenGlRenderBackend::vitaLightmapCoordImpl(out); }
#endif
