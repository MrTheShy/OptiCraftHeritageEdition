#pragma once

// Backend side of platform/RenderModelBatch.h: the hooks the GL backend calls.
#include "platform/RenderAPI.h"

#if PLATFORM_MODEL_GPU_BATCH
// Parts are waiting to be drawn. Every backend call that changes state or
// draws must flush them first, so they see the state they were submitted with.
extern bool g_vitaModelPending;
void vitaModelBatchFlush();
#define VITA_MODEL_FLUSH() (g_vitaModelPending ? vitaModelBatchFlush() : (void)0)

// Fixed-function state vitaGL cannot report back (no glGetLight, no alpha func).
void vitaModelTrackLight(int light, RenderLightParameter parameter, const float *values);
void vitaModelTrackLightModelAmbient(const float *values);
void vitaModelTrackAlphaFunc(unsigned glFunction);
void vitaModelTrackColorMaterial(bool enabled);
// RGBA8 contents of an uploaded texture (RenderEngine::setupTexture), to find the
// model parts whose texels the alpha test would drop.
void vitaModelTrackTextureAlpha(int texture, int width, int height, const unsigned char *rgba);

// Provided by the backend.
bool vitaDisplayListRecording();
void vitaLightmapCoord(float out[2]);
#else
#define VITA_MODEL_FLUSH() ((void)0)
#endif
