#pragma once
#include "RE/I/IMenu.h"

namespace Pulsar::Lobby
{
    // Native Scaleform lobby menu (Teil 3). Concrete IMenu so it can be
    // instantiated, but at creation time its vtable pointers are overwritten to a
    // patched copy of the ENGINE's real IMenu vtable (CLibSF's IMenu thunks are
    // stale {0} and would crash). The C++ overrides below only make the class
    // concrete - they are bypassed by the vtable overwrite.
    class LobbyMenu : public RE::IMenu
    {
    public:
        SF_MENU_NAME("LobbyMenu");

        LobbyMenu() = default;

        const char*   GetName() const override     { return "LobbyMenu"; }
        const char*   GetRootPath() const override { return "Interface/pulsar_lobby.swf"; }
        std::uint64_t GetUnk05() override          { return 0; }

        // Required to make the class concrete (pure in BSTEventSink). Bypassed by
        // the vtable overwrite at creation, so the body is never actually called.
        RE::BSEventNotifyControl ProcessEvent(const RE::UpdateSceneRectEvent&,
                                              RE::BSTEventSource<RE::UpdateSceneRectEvent>*) override
        {
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // Game-thread, self-guarded: registers LobbyMenu into UI::menuMap (at the
    // confirmed offset UI+0x470, with an engine-matching hash) once the UI exists.
    void TryRegisterMenuOnce();

    // Hotkey action: open the menu if closed, close it if open (UIMessageQueue).
    void ToggleLobbyMenu();
}
