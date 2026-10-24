#include "PCH.h"
#include "FlashlightSync/FlashlightSync.h"

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>
#include <REL/Relocation.h>

#include <Windows.h>
#include <intrin.h>   // _InterlockedIncrement / _InterlockedExchangeAdd (refcount)
#include <cstdint>

using namespace std::string_view_literals;

namespace Pulsar::FlashlightSync {
namespace {

    // Detection: Actor+0xEB0 = BSShadowLight* on the PLAYER only (null = off).
    constexpr std::uintptr_t kFieldOff = 0xEB0;

    // --- engine pieces (RE'd from the headlamp setup FUN_141a68a10) ---
    // Light factory: creates a BSShadowLight attached to a node. Real call form
    // is (mgr, null, attachNode, 1, 1, 0) -> BSShadowLight*.
    using FactoryFn   = void* (__fastcall*)(void* a_mgr, void* a_p2, void* a_node,
                                            char a_c4, char a_c5, char a_c6);
    // Actor -> attach target. (actor, &out) -> &out, *out = node (*(actor+0xDE8)
    // or the 3D-root fallback). Holds a ref on *out.
    using AttachFn    = void** (__fastcall*)(void* a_actor, void** a_out);
    // FUN_1404f0f20() -> ptr; +0xAF0 = the fallback light manager.
    using MgrProvFn   = void* (__fastcall*)();
    // FUN_1424a79c0(x,on) - register/activate the light with the renderer (1=on).
    using EnableFn    = void  (__fastcall*)(void* a_x, char a_on);
    // FUN_14036bdb0(light) - DETACH a light from the scene graph: it does
    // parent->RemoveChild(light) via light+0x38, then runs PostAttachActions. The
    // engine headlamp OFF calls this BEFORE dropping its owning ref. NOTE: it MUST
    // be passed the light* (earlier code called it arg-less → no-op → OFF failed).
    // RemoveChild decrefs the light, so the caller must hold an extra ref or the
    // light frees mid-call → UAF.
    using DetachFn    = void  (__fastcall*)(void* a_light);

    static REL::Relocation<FactoryFn> g_factory { REL::Offset(0xA6C510) };
    static REL::Relocation<AttachFn>  g_attach  { REL::Offset(0x1A3EB60) };
    static REL::Relocation<MgrProvFn> g_mgrProv { REL::Offset(0x4F07A0) };
    static REL::Relocation<EnableFn>  g_enable  { REL::Offset(0x24A5740) };
    static REL::Relocation<DetachFn>  g_detach  { REL::Offset(0x36B7E0) };
    constexpr std::uintptr_t kRVA_MgrGlobal = 0x5E8BD00;  // DAT_145e93ac0 (read raw)

    // NiRefObject refcount is a 32-bit int at +0x8 (engine: incref `*(int*)(o+8)+1`,
    // decref then `if old==1 -> vtable[1](o)` = DeleteThis). The clone is a base
    // Actor (sizeof 0x578) NOT a PlayerCharacter (0x1128), so the engine's +0xEB0
    // headlamp slot is OUT OF BOUNDS on it. We therefore can't drive the engine
    // toggle FUN_141a68a10 on the clone (it writes actor+0xEB0). Instead we OWN the
    // light: g_litLight is our +0xEB0-equivalent owner ref. We mirror the engine's
    // create (factory + INCREF + activate) and destroy (detach + DECREF) exactly.
    constexpr std::uintptr_t kRefCountOff = 0x08;

    // Single remote clone per machine (Alpha = 2 players). Track the clone we lit +
    // our OWNED BSShadowLight* (we hold one ref, like the engine's +0xEB0 slot).
    static RE::Actor* g_litClone = nullptr;
    static void*      g_litLight = nullptr;   // OWNED ptr (we hold +1 ref)
    static void*      g_lit3D    = nullptr;   // clone data3D the light was made against

    // ---- POD SEH helpers (no C++ unwinding objects in these frames) ----
    static bool ReadPtr(void* a_base, std::uintptr_t a_off, void** a_out) noexcept
    {
        __try { *a_out = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(a_base) + a_off); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Clone's rendered 3D root: actor+0xB8 (loadedData) -> +0x08 (data3D). 0 when
    // the clone's 3D is detached (proc-cell / not loaded). SEH-guarded.
    static void* CloneData3D(void* a_clone) noexcept
    {
        __try {
            void* ld = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(a_clone) + 0xB8);
            if (!ld) return nullptr;
            return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(ld) + 0x08);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static bool MgrValid(void* m) noexcept   // +0x2e == 0x27 gate (as in headlamp setup)
    { return m && *(reinterpret_cast<std::uint8_t*>(m) + 0x2e) == 0x27; }
    static bool MgrIsShadow(void* m) noexcept  // factory: +0x1d0==1 -> creates BSShadowLight
    { return m && *(reinterpret_cast<std::uint8_t*>(m) + 0x1d0) == 1; }

    // Resolve the light manager. The factory makes a real BSShadowLight (visible
    // headlamp, like the player) only when *(mgr+0x1d0)==1; the other manager
    // yields a non-shadow light that doesn't render the same → invisible (the
    // dump showed CLONE got the wrong vtable). So PREFER the shadow manager.
    static void* ResolveManager() noexcept
    {
        __try {
            void* m1 = *reinterpret_cast<void**>(REL::Offset(kRVA_MgrGlobal).address());
            void* m2 = nullptr;
            if (void* prov = g_mgrProv())
                m2 = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(prov) + 0xAF0);
            // 1) prefer a valid SHADOW manager
            if (MgrValid(m1) && MgrIsShadow(m1)) return m1;
            if (MgrValid(m2) && MgrIsShadow(m2)) return m2;
            // 2) fallback: any valid manager (non-shadow - last resort)
            if (MgrValid(m1)) return m1;
            if (MgrValid(m2)) return m2;
            return nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static long RefCount(void* a_obj) noexcept   // 32-bit refcount at +0x8
    { return *reinterpret_cast<long*>(reinterpret_cast<std::uint8_t*>(a_obj) + kRefCountOff); }

    // g_attach (FUN_141a41570) reads *(actor+0xDE8) and traverses the 3D - it FAULTS
    // on some clones (client log: 7954/7954 host-clone creates died here, node never
    // set). Isolate it so a fault falls back to the clone's 3D root instead of
    // aborting the whole create.
    static void* SafeAttachNode(void* a_clone, bool* a_faulted) noexcept
    {
        *a_faulted = false;
        void* nodeOut = nullptr;
        __try { g_attach(a_clone, &nodeOut); }
        __except (EXCEPTION_EXECUTE_HANDLER) { *a_faulted = true; nodeOut = nullptr; }
        return nodeOut;
    }

    // DIAG - per-create snapshot (POD so it can be filled inside __try; the
    // caller logs it outside, since REX::INFO uses C++ objects).
    struct CreateDiag { void* mgr; bool mgrShadow; void* node; void* light; void* lightVt; long rc; bool attachFaulted; };

    // Create + attach the light + take ONE owning ref. SEH split in two so an
    // activate fault doesn't lose an already-owned light (else: leak + re-create
    // every tick). a_d3d = the already-validated 3D root from ApplyToClone, used as
    // the attach node when the fragile g_attach resolver faults. Returns the owned
    // light (we hold +1 ref) or null.
    static void* CreateCloneLight(void* a_clone, void* a_d3d, CreateDiag* a_d) noexcept
    {
        void* light = nullptr;
        // g_attach is fragile - resolve it in its OWN SEH so a fault falls back to
        // the 3D root rather than aborting (this is what stranded the client).
        bool attachFaulted = false;
        void* attachNode = SafeAttachNode(a_clone, &attachFaulted);
        // --- phase 1: resolve mgr + create + OWN. A fault here means nothing was
        // owned yet, so returning null is leak-safe. ---
        __try {
            void* mgr = ResolveManager();
            a_d->mgr = mgr; a_d->mgrShadow = MgrIsShadow(mgr);
            a_d->attachFaulted = attachFaulted;
            if (!mgr) return nullptr;
            void* nodeOut = attachNode ? attachNode : a_d3d;   // fallback: validated 3D root
            a_d->node = nodeOut;
            if (!nodeOut) return nullptr;          // clone 3D not loaded → retry next tick
            light = g_factory(mgr, nullptr, nodeOut, 1, 1, 0);
            a_d->light = light;
            if (light) a_d->lightVt = *reinterpret_cast<void**>(light);
            if (!light) return nullptr;
            // OWN it: the factory attaches the light to the node (node holds 1 ref).
            // The engine ON path then INCREFs before storing at +0xEB0, so a later
            // detach (RemoveChild → decref the node's ref) doesn't free it mid-call.
            // We do the same: this +1 is OUR owning ref (released in DestroyCloneLight).
            _InterlockedIncrement(reinterpret_cast<long*>(
                reinterpret_cast<std::uint8_t*>(light) + kRefCountOff));
            a_d->rc = RefCount(light);   // refcount after our owning incref
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        // --- phase 2: activate. We already OWN the light; a fault here must NOT
        // lose it (we still return it so it's tracked + destroyable). ---
        __try {
            void** vt = *reinterpret_cast<void***>(light);
            using GetActivatableFn = void* (__fastcall*)(void*);
            void* x = reinterpret_cast<GetActivatableFn>(vt[0x100 / 8])(light);
            if (x) g_enable(x, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return light;
    }

    // POD result so DestroyCloneLight (a __try frame, no C++ objects) can hand back
    // diagnostics for the caller to log outside the SEH frame.
    struct DestroyDiag { long rcBefore; long rcAfterDetach; long old; bool destroyed; bool faulted; };

    // Real OFF: detach the light from the scene graph, then drop our owning ref so
    // it's destroyed. Exactly the engine headlamp OFF sequence (FUN_141a68a10 OFF
    // branch), operating on OUR owned light instead of player+0xEB0:
    //   FUN_14036bdb0(light)  -> parent->RemoveChild(light) + PostAttachActions
    //                            (drops the node's ref AND should un-register the
    //                            light from the renderer/shadow lists; our extra ref
    //                            keeps it alive through the call)
    //   decref(light)         -> if that was the last ref -> vtable[1] = DeleteThis
    // The refcount snapshots tell us whether detach really drops to our single ref
    // (old==1 → destroy). If detach leaves rc>1 the light won't free → OFF fails and
    // the log will show it (then we'd deactivate before detach).
    // SEH-guarded: if the clone's 3D was already torn down the parent ptr may be
    // stale; a fault is swallowed (minor leak, never a crash).
    static void DestroyCloneLight(void* a_light, DestroyDiag* a_d) noexcept
    {
        a_d->rcBefore = a_d->rcAfterDetach = a_d->old = 0;
        a_d->destroyed = a_d->faulted = false;
        if (!a_light) return;
        __try {
            a_d->rcBefore = RefCount(a_light);
            g_detach(a_light);                 // detach from scene graph (needs light* arg!)
            a_d->rcAfterDetach = RefCount(a_light);
            // drop our owning ref; if it was the last, call the deleting destructor.
            a_d->old = _InterlockedExchangeAdd(
                reinterpret_cast<long*>(reinterpret_cast<std::uint8_t*>(a_light) + kRefCountOff), -1);
            if (a_d->old == 1) {
                void** vt = *reinterpret_cast<void***>(a_light);
                using DeleteThisFn = void (__fastcall*)(void*);
                reinterpret_cast<DeleteThisFn>(vt[1])(a_light);
                a_d->destroyed = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { a_d->faulted = true; }
    }

    // Destroy + log (logging lives OUTSIDE the __try frame - REX::INFO uses C++
    // objects, forbidden in a __try body by C2712).
    static void DestroyAndLog(void* a_light, const char* a_tag) noexcept
    {
        DestroyDiag d{};
        DestroyCloneLight(a_light, &d);
        PLOG_INFO("[PULSAR] [FL] OFF {} light=0x{:X} rc:before={} afterDetach={} old={} "
                  "destroyed={} faulted={}"sv,
                  a_tag, reinterpret_cast<std::uintptr_t>(a_light),
                  d.rcBefore, d.rcAfterDetach, d.old, d.destroyed, d.faulted);
    }

    // DIAG - dump first 0x100 bytes of a light object as qword rows (so we
    // can eyeball pointers + interpret dwords as floats). POD SEH frame.
    static void DumpLightObj(const char* a_tag, void* a_obj) noexcept
    {
        if (!a_obj) { return; }
        __try {
            auto* b = reinterpret_cast<std::uint8_t*>(a_obj);
            for (std::uintptr_t off = 0; off < 0x100; off += 0x10) {
                std::uint64_t q0 = *reinterpret_cast<std::uint64_t*>(b + off);
                std::uint64_t q1 = *reinterpret_cast<std::uint64_t*>(b + off + 8);
                float f0 = *reinterpret_cast<float*>(b + off);
                float f1 = *reinterpret_cast<float*>(b + off + 4);
                float f2 = *reinterpret_cast<float*>(b + off + 8);
                float f3 = *reinterpret_cast<float*>(b + off + 12);
                PLOG_INFO("[PULSAR] [FLDUMP] {} +0x{:03X}: {:016X} {:016X} | f {:.3f} {:.3f} {:.3f} {:.3f}"sv,
                          a_tag, static_cast<unsigned>(off), q0, q1, f0, f1, f2, f3);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            PLOG_INFO("[PULSAR] [FLDUMP] {} read faulted"sv, a_tag);
        }
    }

}  // namespace

void DumpLights()
{
    // Player's working headlamp (the reference).
    void* pl = nullptr;
    if (auto* pc = RE::PlayerCharacter::GetSingleton())
        ReadPtr(pc, kFieldOff, &pl);
    PLOG_INFO("[PULSAR] [FLDUMP] === player light=0x{:X}  clone light=0x{:X} ==="sv,
              reinterpret_cast<std::uintptr_t>(pl),
              reinterpret_cast<std::uintptr_t>(g_litLight));
    DumpLightObj("PLAYER", pl);
    DumpLightObj("CLONE ", g_litLight);
}

bool IsLocalOn()
{
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc) return false;
    void* light = nullptr;
    if (!ReadPtr(pc, kFieldOff, &light)) return false;
    const bool on = (light != nullptr);
    static int s_last = -1;
    if (static_cast<int>(on) != s_last) {
        s_last = static_cast<int>(on);
        PLOG_INFO("[PULSAR] [FL] local flashlight {}"sv, on ? "ON" : "OFF");
    }
    return on;
}

void ApplyToClone(RE::Actor* a_clone, bool a_wantOn)
{
    if (!a_clone) return;

    void* d3d = CloneData3D(a_clone);

    // Clone changed (respawn) OR the clone's 3D re-attached (proc-cell
    // detach/reattach gives a new data3D) → our old light is bound to the OLD node.
    // Tear it down properly (detach + decref) so it doesn't leak/dangle, then start
    // fresh against the new 3D. DestroyCloneLight is SEH-guarded against a node that
    // was already freed by the engine's 3D teardown.
    if (a_clone != g_litClone || d3d != g_lit3D) {
        if (g_litLight) {
            DestroyAndLog(g_litLight, "3d-change");
        }
        g_litLight = nullptr;
        g_litClone = a_clone;
        g_lit3D    = d3d;
    }

    if (a_wantOn) {
        if (!g_litLight && d3d) {   // create once, only after the clone 3D is loaded
            CreateDiag d{};
            g_litLight = CreateCloneLight(a_clone, d3d, &d);
            void* shadowVt = reinterpret_cast<void*>(REL::Offset(0x4D73A70).address());
            PLOG_INFO("[PULSAR] [FL] create clone=0x{:X} mgr=0x{:X} shadowMgr={} node=0x{:X} "
                      "attachFaulted={} light=0x{:X} vt=0x{:X} isBSShadowLight={} rc={}"sv,
                      reinterpret_cast<std::uintptr_t>(a_clone),
                      reinterpret_cast<std::uintptr_t>(d.mgr), d.mgrShadow,
                      reinterpret_cast<std::uintptr_t>(d.node), d.attachFaulted,
                      reinterpret_cast<std::uintptr_t>(d.light),
                      reinterpret_cast<std::uintptr_t>(d.lightVt),
                      d.lightVt == shadowVt, d.rc);
        }
    } else {
        if (g_litLight) {   // real OFF - destroy the light (engine OFF sequence)
            DestroyAndLog(g_litLight, "toggle-off");
            g_litLight = nullptr;
        }
    }
}

void ReleaseForClone(RE::Actor* a_clone)
{
    // Controlled despawn while the clone (and its 3D) is still valid → tear the
    // light down cleanly so we drop our owning ref before the node goes away.
    if (a_clone == g_litClone && g_litLight) {
        DestroyAndLog(g_litLight, "despawn");
    }
    if (a_clone == g_litClone) {
        g_litLight = nullptr;
        g_litClone = nullptr;
        g_lit3D    = nullptr;
    }
}

void ForgetAll()
{
    // Save-load full reset already freed the underlying objects (including our
    // light, since its node was destroyed) - do NOT touch the freed light; just
    // forget the tracking so we never deref it.
    g_litLight = nullptr;
    g_litClone = nullptr;
    g_lit3D    = nullptr;
}

}  // namespace Pulsar::FlashlightSync
