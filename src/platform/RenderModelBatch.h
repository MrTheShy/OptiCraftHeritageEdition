#pragma once

#include "platform/PlatformConfig.h"

class ModelRenderer;

// Model parts drawn inside a scope are collected and drawn together: one VBO
// per scope key (an entity or tile-entity renderer) holding every part it has
// drawn, one draw per pass with a matrix per part. Anything else that reaches
// the backend (a state change, another draw) draws the collected parts first,
// so the result is the same as drawing each part where it was submitted.
#if PLATFORM_MODEL_GPU_BATCH
// Returns the scope it replaces, to hand back to renderModelBatchEnd.
const void *renderModelBatchBegin(const void *key);
void renderModelBatchEnd(const void *previous);
// The part's transform is the current modelview. False: draw it as usual.
bool renderModelBatchPart(ModelRenderer *part, float scale);
// The part's boxes changed, or it is being destroyed.
void renderModelBatchForget(ModelRenderer *part);
#else
inline const void *renderModelBatchBegin(const void *) { return nullptr; }
inline void renderModelBatchEnd(const void *) {}
inline bool renderModelBatchPart(ModelRenderer *, float) { return false; }
inline void renderModelBatchForget(ModelRenderer *) {}
#endif

struct RenderModelBatchScope
{
    explicit RenderModelBatchScope(const void *key) : previous(renderModelBatchBegin(key)) {}
    ~RenderModelBatchScope() { renderModelBatchEnd(previous); }
    RenderModelBatchScope(const RenderModelBatchScope &) = delete;
    RenderModelBatchScope &operator=(const RenderModelBatchScope &) = delete;

private:
    const void *previous;
};
