#pragma once

namespace Pulsar::Debug
{
    void InstallDropDebugHook();      // Call at kPostLoad
    void InstallDeferredHooks();      // Call from game thread (permanent task)
}
