#pragma once

#ifdef PS2_PLATFORM

#include <cstdint>

namespace Ps2Network
{
bool initialize();
bool isReady();
bool localAddressNetworkOrder(std::uint32_t &address);
// Lets the connecting screen stop a connect() retry loop.
void cancelPendingConnect();
void clearConnectCancel();
bool connectCancelled();
}

#endif
