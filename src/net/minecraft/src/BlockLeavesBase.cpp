#include "BlockLeavesBase.h"
#include "Config.h"
#include "IBlockAccess.h"

bool BlockLeavesBase::meshAsFast = false;

BlockLeavesBase::BlockLeavesBase(int_t i, int_t j, Material *material, bool flag)
    : Block(i, j, material), graphicsLevel(flag)
{
}

bool BlockLeavesBase::isOpaqueCube()
{
    return false;
}

bool BlockLeavesBase::shouldSideBeRendered(IBlockAccess *iblockaccess, int_t i, int_t j, int_t k, int_t l)
{
    int_t i1 = iblockaccess->getBlockId(i, j, k);
    // Trees: Smart is Fancy (see-through leaves) without the faces between
    // two leaf blocks, most of a forest's vertices. The id test first: it
    // fails for five of six faces and the rest costs calls.
    if (i1 == blockID && (!graphicsLevel || meshAsFast || Config::isTreesSmart()))
        return false;
    return Block::shouldSideBeRendered(iblockaccess, i, j, k, l);
}
