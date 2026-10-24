#include "PCH.h"
#include "NPCSync/PluginLookup.h"

#include "NASLog.h"
#include <REL/Relocation.h>
#include <Windows.h>

#include <atomic>
#include <cstring>

namespace Pulsar::NPCSync::PluginLookup
{
    using namespace std::string_view_literals;

    // Starfield.exe base, via the same mechanism REL::ID and REL::Offset use
    // under the hood - REX::FModule::GetExecutingModule(). Using
    // GetModuleHandleW(L"Starfield.exe") is unreliable under some
    // SFSE loader configurations; REL::Offset.address() is the canonical way.
    static uintptr_t DataHandlerSlotAddress()
    {
        static uintptr_t s_addr = []() -> uintptr_t {
            // RVA 0x5FD9B28 in Starfield 1.16.236 (T34 Ghidra-verified).
            REL::Offset slot{ 0x5FD9B28 };
            return slot.address();
        }();
        return s_addr;
    }

    // Offsets inside TESDataHandler
    static constexpr ptrdiff_t kDH_LoadedModsHead = 0x1570;

    // Offsets inside TESFile (T34 Ghidra-verified)
    static constexpr ptrdiff_t kFile_Name         = 0x38;
    static constexpr ptrdiff_t kFile_Flags        = 0x1B8;
    static constexpr ptrdiff_t kFile_OverlaySrc   = 0x1D8;
    static constexpr ptrdiff_t kFile_CompileIdx   = 0x218;
    static constexpr ptrdiff_t kFile_MediumSlot   = 0x219;
    static constexpr ptrdiff_t kFile_LightSlot    = 0x21A;

    // Flag bits at TESFile+0x1B8 (bit layout from FixupFormID @ FUN_1405B8560)
    static constexpr uint32_t  kFlag_Light    = 1u << 8;
    static constexpr uint32_t  kFlag_Overlay  = 1u << 9;
    static constexpr uint32_t  kFlag_Medium   = 1u << 10;

    // Match NPCStateData.sourcePlugin size - avoids a chicken-and-egg include.
    static constexpr size_t NPC_SOURCE_PLUGIN_MAX_LOCAL = 64;

    // Everything that dereferences engine memory runs in plain-C __try wrappers
    // (no C++ unwinding). Pointers in/out only.

    static uint8_t* SafeGetDataHandler(uintptr_t slotAddr) noexcept
    {
        if (!slotAddr) return nullptr;
        __try {
            return *reinterpret_cast<uint8_t**>(slotAddr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // The Ghidra reference code in FUN_140598FF0 treats (DataHandler+0x1570)
    // itself as the first list node: `plVar2 = (longlong*)(dh+0x1570); lVar4 = *plVar2`
    // reads the first file pointer DIRECTLY from that offset. No extra deref.
    // (We previously did one; it silently read garbage for the first node's
    // `next` field and the walk always misses.)
    static uint8_t** SafeGetListHead(uint8_t* dh) noexcept
    {
        return reinterpret_cast<uint8_t**>(dh + kDH_LoadedModsHead);
    }

    // Returns 0 on fault.
    static uint32_t SafeReadFileFlags(uint8_t* file) noexcept
    {
        __try {
            return *reinterpret_cast<uint32_t*>(file + kFile_Flags);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // NOTE: TESFile+0x38 is an INLINE `char[]`, not a `char*`. The Ghidra
    // reference code does `_stricmp(name, (char *)(file + 0x38))` - a cast,
    // not a deref. Previously we dereferenced once, which interpreted the
    // first 8 bytes of the filename ("Starfie" etc.) as a pointer and
    // faulted on that "address"; our SEH swallowed the fault and gave us an
    // empty string (see `name=''` diagnostic from 22:00:59).
    static const char* SafeReadFileName(uint8_t* file) noexcept
    {
        if (!file) return nullptr;
        return reinterpret_cast<const char*>(file + kFile_Name);
    }

    // Bounded, SEH-guarded C-string copy. outBuf must be non-null and
    // outSize >= 1. Returns true iff the copy completed (null-terminator
    // written). On false, outBuf[0] is guaranteed '\0'.
    static bool SafeCopyCString(const char* src, char* outBuf, size_t outSize) noexcept
    {
        if (!outBuf || outSize == 0) return false;
        outBuf[0] = '\0';
        if (!src) return false;
        __try {
            size_t i = 0;
            for (; i + 1 < outSize; ++i) {
                char c = src[i];
                outBuf[i] = c;
                if (c == '\0') return true;
            }
            outBuf[i] = '\0';
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            outBuf[0] = '\0';
            return false;
        }
    }

    // Follows overlay indirection: when bit 9 is set, the real file metadata
    // lives at **(+0x1D8). Returns the original file on fault.
    static uint8_t* SafeResolveOverlayBase(uint8_t* file, uint32_t flags) noexcept
    {
        if ((flags & kFlag_Overlay) == 0) return file;
        __try {
            auto* src = *reinterpret_cast<uint8_t***>(file + kFile_OverlaySrc);
            return (src && *src) ? *src : file;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return file;
        }
    }

    // Output slot-fields. Returns true on success; out-values are set on
    // success only. Packaged as one call so we only enter/exit __try once.
    struct FileSlots {
        uint8_t  compileIdx;
        uint8_t  mediumSlot;
        uint16_t lightSlot;
    };
    static bool SafeReadFileSlots(uint8_t* file, FileSlots& out) noexcept
    {
        __try {
            out.compileIdx = *reinterpret_cast<uint8_t*>(file + kFile_CompileIdx);
            out.mediumSlot = *reinterpret_cast<uint8_t*>(file + kFile_MediumSlot);
            out.lightSlot  = *reinterpret_cast<uint16_t*>(file + kFile_LightSlot);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Read the next-link from a list node. Returns nullptr on fault / end.
    static uint8_t** SafeReadNextNode(uint8_t** node) noexcept
    {
        __try {
            return reinterpret_cast<uint8_t**>(node[1]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    static uint8_t* SafeReadNodeFile(uint8_t** node) noexcept
    {
        __try {
            return reinterpret_cast<uint8_t*>(node[0]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    bool FormIDToPluginName(uint32_t formID, char* outBuf, size_t outSize) noexcept
    {
        if (outBuf && outSize > 0) outBuf[0] = '\0';
        if (formID == 0) return false;
        uint8_t topByte = static_cast<uint8_t>((formID >> 24) & 0xFFu);
        if (topByte == 0xFF) return false;   // dynamic

        uintptr_t slot = DataHandlerSlotAddress();
        uint8_t*  dh   = SafeGetDataHandler(slot);

        // One-shot diagnostic on first call - confirms our slot address is
        // sensible and the DataHandler isn't null. Without this, a silent
        // "lookup returned nothing" is indistinguishable from a crash.
        static std::atomic<bool> s_logged{ false };
        if (!s_logged.exchange(true)) {
            PLOG_INFO("[PULSAR] PluginLookup: slot=0x{:x} dh=0x{:x}"sv,
                      slot, reinterpret_cast<uintptr_t>(dh));
        }
        if (!dh) return false;

        uint8_t** node = SafeGetListHead(dh);

        // Per-walk diagnostic - log the first few nodes' file pointers + compile-idx
        // on the first matching-formID lookup, to confirm offsets are right.
        static std::atomic<int> s_diagCount{ 0 };
        const bool diag = s_diagCount.fetch_add(1) < 3;
        // Hard iteration cap - engine's mod list can't exceed ~255 entries.
        // Prevents runaway walks if the list tail gets corrupted mid-load.
        for (int guard = 0; node && guard < 512; ++guard) {
            uint8_t* file = SafeReadNodeFile(node);
            if (!file) break;

            uint32_t flags  = SafeReadFileFlags(file);
            uint8_t* base   = SafeResolveOverlayBase(file, flags);
            if (!base) { node = SafeReadNextNode(node); continue; }

            FileSlots fileSlots{};
            if (!SafeReadFileSlots(base, fileSlots)) { node = SafeReadNextNode(node); continue; }

            bool isLight  = (flags & kFlag_Light)  != 0;
            bool isMedium = (flags & kFlag_Medium) != 0;

            if (diag && guard < 4) {
                char tmpName[NPC_SOURCE_PLUGIN_MAX_LOCAL];
                SafeCopyCString(SafeReadFileName(file), tmpName, sizeof(tmpName));
                PLOG_INFO("[PULSAR] PluginLookup: [{}] file=0x{:x} flags=0x{:x} idx={:02X} medium={:02X} light={:04X} name='{}'"sv,
                          guard,
                          reinterpret_cast<uintptr_t>(file),
                          flags,
                          fileSlots.compileIdx,
                          fileSlots.mediumSlot,
                          fileSlots.lightSlot,
                          tmpName);
            }

            bool match = false;
            if (isLight) {
                if (topByte == 0xFE) {
                    uint16_t slotBits = static_cast<uint16_t>((formID >> 12) & 0xFFFu);
                    match = (slotBits == fileSlots.lightSlot);
                }
            } else if (isMedium) {
                if (topByte == 0xFD) {
                    uint8_t slotByte = static_cast<uint8_t>((formID >> 16) & 0xFFu);
                    match = (slotByte == fileSlots.mediumSlot);
                }
            } else {
                match = (fileSlots.compileIdx != 0xFF) && (fileSlots.compileIdx == topByte);
            }

            if (match) {
                const char* name = SafeReadFileName(file);
                return SafeCopyCString(name, outBuf, outSize);
            }
            node = SafeReadNextNode(node);
        }
        return false;
    }

    uint32_t FormIDToLocalID(uint32_t formID) noexcept
    {
        uint8_t topByte = static_cast<uint8_t>((formID >> 24) & 0xFFu);
        if (topByte == 0xFE) return formID & 0xFFFu;        // light / ESL
        if (topByte == 0xFD) return formID & 0xFFFFu;       // medium
        return formID & 0xFFFFFFu;                          // regular
    }

    uint32_t ResolveCrossPlugin(const char* pluginName, uint32_t localID) noexcept
    {
        if (!pluginName || !pluginName[0]) return 0;

        uint8_t* dh = SafeGetDataHandler(DataHandlerSlotAddress());
        if (!dh) return 0;

        uint8_t** node = SafeGetListHead(dh);
        for (int guard = 0; node && guard < 512; ++guard) {
            uint8_t* file = SafeReadNodeFile(node);
            if (!file) break;

            // SEH-copy the filename into a local buffer before stricmp -
            // _stricmp walks until it finds a null byte and would fault on
            // an invalid engine-side char*.
            char nameBuf[NPC_SOURCE_PLUGIN_MAX_LOCAL];
            const char* rawName = SafeReadFileName(file);
            if (!SafeCopyCString(rawName, nameBuf, sizeof(nameBuf)) ||
                _stricmp(nameBuf, pluginName) != 0)
            {
                node = SafeReadNextNode(node);
                continue;
            }

            // Found our plugin. Read its encoding and build a client-local FormID.
            uint32_t flags  = SafeReadFileFlags(file);
            uint8_t* base   = SafeResolveOverlayBase(file, flags);
            if (!base) return 0;

            FileSlots slots{};
            if (!SafeReadFileSlots(base, slots)) return 0;

            bool isLight  = (flags & kFlag_Light)  != 0;
            bool isMedium = (flags & kFlag_Medium) != 0;
            if (isLight) {
                return 0xFE000000u
                     | (static_cast<uint32_t>(slots.lightSlot) << 12)
                     | (localID & 0xFFFu);
            }
            if (isMedium) {
                return 0xFD000000u
                     | (static_cast<uint32_t>(slots.mediumSlot) << 16)
                     | (localID & 0xFFFFu);
            }
            if (slots.compileIdx == 0xFF) return 0;
            return (static_cast<uint32_t>(slots.compileIdx) << 24)
                 | (localID & 0xFFFFFFu);
        }
        return 0;
    }
}
