#pragma once

#include "Block.h"

class IBlockAccess;

// net.minecraft.src.BlockLeavesBase
class BlockLeavesBase : public Block
{
public:
	BlockLeavesBase(int_t i, int_t j, Material *material, bool flag);

	bool isOpaqueCube() override;
	bool shouldSideBeRendered(IBlockAccess *iblockaccess, int_t i, int_t j, int_t k, int_t l) override;
	bool usesDefaultFaceCulling() const override { return false; }

	// Trees: Smart. Set on the game thread while a section farther than
	// PC_LEGACY_FAR_LEAVES_DISTANCE is meshed: its leaves are built as Fast
	// (opaque, the fast tile, no inner faces), so the GPU keeps hidden-surface
	// removal for most of a forest; near sections stay Fancy without inner
	// faces. A plain static: thread_local is emulated on vitasdk (a chain of
	// nine calls per read, ~0.3 us, read up to 19 times per leaf). It is only
	// true inside one renderer's mesh step on the game thread; the worker
	// threads read block tables, not Block methods.
	static bool meshAsFast;

protected:
	bool graphicsLevel;
};
