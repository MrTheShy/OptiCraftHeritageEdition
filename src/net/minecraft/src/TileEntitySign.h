#pragma once

#include "TileEntity.h"
#include "java/String.h"
#include "platform/PlatformConfig.h"
#if PLATFORM_SIGN_TEXT_CACHE
#include "platform/RenderAPI.h"
#endif

class TileEntitySign : public TileEntity {
public:
    TileEntitySign();
#if PLATFORM_SIGN_TEXT_CACHE
    // Text glyphs as last laid out (sign-local coordinates), and what they
    // were laid out from. Plain CPU memory: no GL object to free.
    RenderCapturedMesh textMesh;
    jstring textMeshText[4];
    int textMeshEditLine = -2;
    unsigned int textMeshFont = 0;
    bool textMeshStale(unsigned int fontRevision) const;
    void markTextMeshDrawn(unsigned int fontRevision);
#endif

    void writeToNBT(NBTTagCompound* nbttagcompound) override;
    void readFromNBT(NBTTagCompound* nbttagcompound) override;

    bool isEditable() const;
    void setEditable(bool editable);

    jstring signText[4];
    int lineBeingEdited;

private:
    bool editable;
};
