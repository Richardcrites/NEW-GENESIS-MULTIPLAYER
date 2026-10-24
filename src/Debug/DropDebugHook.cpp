#include "PCH.h"
#include "Debug/DropDebugHook.h"

using namespace std::string_view_literals;

namespace Pulsar::Debug
{
    // Debug hooks disabled - PlaceAtMe found at RVA 0x2079E10
    // It's a Papyrus latent function, can't hook safely with MinHook.
    // Solution: use Papyrus PlaceAtMe via GLOBs for 3D item drops.

    void InstallDeferredHooks() {}
    void InstallDropDebugHook()
    {
        PLOG_INFO("[PULSAR] DropDebugHook disabled (PlaceAtMe found, using Papyrus path)"sv);
    }
}
