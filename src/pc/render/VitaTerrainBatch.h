#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/PlatformConfig.h"

#ifdef VITA_PLATFORM
// Fixed-function state that the terrain shader reproduces, tracked by the GL
// backend as the game sets it (vitaGL cannot report fog parameters back).
struct VitaFixedState
{
    bool fog;
    int fogMode; // 0 linear, 1 exp, 2 exp2
    float fogStart, fogEnd, fogDensity;
    float fogColor[4];
    bool alphaTest;
    float alphaRef;
};
const VitaFixedState &vitaFixedState();
#endif

#if PLATFORM_TERRAIN_VBO_BATCH

struct RenderCapturedMesh;

// One terrain section pass as a single VBO of GL_QUADS. Quads with no
// transparent texel come first and skip the alpha test. Vertices are 24 bytes
// (fixed function) or 16 (Terrain Shader). With the shader, big sections store
// their quads by face direction (+X, -Y, +Z, other, -Z, +Y, -X) so a draw can
// skip the groups the eye is behind.
constexpr int kVitaFaceGroups = 7;
struct VitaTerrainFaceGroups
{
    int end[2][kVitaFaceGroups];           // cumulative vertex end per group, from the range start
    std::int16_t planeMin[2][kVitaFaceGroups]; // plane coordinate range of the axis groups
    std::int16_t planeMax[2][kVitaFaceGroups];
};

struct VitaTerrainMesh
{
    unsigned buffer = 0;
    int vertexCount = 0;
    int solidCount = 0; // leading vertices that need no alpha test
    bool shader = false;
    bool grouped = false;
    VitaTerrainFaceGroups groups;
};

enum class VitaTerrainPublish
{
    Unsupported, // mesh does not fit the format: caller keeps using a display list
    Empty,       // nothing to draw
    Uploaded     // `out` now owns a VBO
};

// Merges the compact and tessellator meshes of one pass (compact first, as the
// display list drew them) into `out`, replacing and freeing its previous VBO.
VitaTerrainPublish vitaTerrainPublish(VitaTerrainMesh &out, const RenderCapturedMesh &compact,
    const RenderCapturedMesh &base, int pass, bool useShader);
void vitaTerrainRelease(VitaTerrainMesh &mesh);

// Which 16x16 atlas tiles hold a texel with alpha below 255; read from
// terrain.png when it is loaded.
void vitaTerrainSetAtlas(const unsigned char *rgba, int width, int height);

struct VitaTerrainDraw
{
    unsigned buffer;
    int vertexCount;
    int solidCount;
    bool shader;
    float x, y, z; // section origin relative to the eye, in blocks
    const VitaTerrainMesh *mesh; // for the face groups; lives as long as its renderer
};

// Draws the sections of one pass. Leaves GL state as the display-list draw did.
void vitaTerrainDrawBatch(const VitaTerrainDraw *draws, std::size_t count, int pass);

// Position-only GL_QUADS in a static buffer (clouds), drawn with one colour
// as the current colour: the geometry is uploaded once and the colour is not
// part of it, so a sky colour change does not rebuild anything.
unsigned vitaStaticQuadsUpload(const float *xyz, std::size_t vertexCount);
void vitaStaticQuadsRelease(unsigned buffer);
void vitaStaticQuadsDraw(unsigned buffer, int first, int count, const float rgba[4]);
// The viewer position the draws' origins are relative to (Shader Effects).
void vitaTerrainSetViewer(double x, double y, double z);

#endif
