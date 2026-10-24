#include "PCH.h"
#include "Lobby/LobbyHandoff.h"
#include "Profile/ClientProfile.h"
#include "InventorySync/DirectRestore.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::Lobby
{
    // ─── tiny JSON value extraction (same minimal style as ClientProfile) ───

    static std::string ExtractStr(const std::string& json, const std::string& key)
    {
        std::string search = "\"" + key + "\"";
        auto pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos);
        if (pos == std::string::npos) return "";
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        auto end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos + 1, end - pos - 1);
    }

    static uint32_t ExtractUInt(const std::string& json, const std::string& key)
    {
        std::string search = "\"" + key + "\"";
        auto pos = json.find(search);
        if (pos == std::string::npos) return 0;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return 0;
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
        return static_cast<uint32_t>(std::strtoul(json.c_str() + pos, nullptr, 10));
    }

    std::string LobbyDir()
    {
        std::string base = ".";
        char*  buf = nullptr;
        size_t sz  = 0;
        if (_dupenv_s(&buf, &sz, "LOCALAPPDATA") == 0 && buf) {
            base = buf;
            free(buf);
        }
        std::string dir = base + "\\PULSAR\\";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    Handoff ReadHandoff()
    {
        Handoff h;
        std::string path = LobbyDir() + "pulsar_handoff.json";
        std::ifstream f(path);
        if (!f.is_open()) return h;   // found == false → caller leaves config as-is

        std::stringstream ss;
        ss << f.rdbuf();
        std::string j = ss.str();
        f.close();

        // Consume-once: delete the handoff so the NEXT (normal/solo) launch isn't
        // hijacked into client/connect mode by a stale file.
        { std::error_code ec; std::filesystem::remove(path, ec); }

        h.found   = true;
        h.role    = ExtractStr(j, "role");
        h.version = ExtractUInt(j, "version");

        std::string addr = ExtractStr(j, "host_addr");
        auto colon = addr.rfind(':');
        if (colon != std::string::npos) {
            h.hostIP = addr.substr(0, colon);
            h.port   = static_cast<uint16_t>(std::strtoul(addr.c_str() + colon + 1, nullptr, 10));
        } else {
            h.hostIP = addr;
        }

        h.relaySession = ExtractStr(j, "relay_session");
        h.cryptoSecret = ExtractStr(j, "crypto_secret");   // E2E auth secret (anti-MITM); empty = none
        std::string relay = ExtractStr(j, "relay_addr");
        if (!relay.empty()) h.relayAddr = relay;
        return h;
    }

    void TickApplyProfile()
    {
        static bool s_applied = false;
        if (s_applied) return;

        // Only act when a companion actually staged a profile to apply.
        std::string path = LobbyDir() + "pulsar_profile_in.json";
        if (!std::filesystem::exists(path)) { s_applied = true; return; }

        // Wait until the player is fully spawned (reuses the V3 connect-gate).
        if (!Pulsar::InventorySync::DirectRestore::IsPlayerReadyForConnect()) return;

        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) { s_applied = true; return; }
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());

        f.close();
        if (!blob.empty()) {
            bool ok = Pulsar::Profile::ApplyProfile(blob);
            PLOG_INFO("[PULSAR] [LobbyHandoff] applied profile-in ({} bytes) ok={}"sv,
                      blob.size(), ok);
        }
        // Consume-once: remove so a later normal launch doesn't re-apply a stale profile.
        { std::error_code ec; std::filesystem::remove(path, ec); }
        s_applied = true;
    }

    // Shared writer - write an already-serialized profile blob to profile-out.
    static void WriteProfileBlob(const std::vector<uint8_t>& blob)
    {
        std::string path = LobbyDir() + "pulsar_profile_out.json";
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            PLOG_WARN("[PULSAR] [LobbyHandoff] cannot write {}"sv, path);
            return;
        }
        f.write(reinterpret_cast<const char*>(blob.data()),
                static_cast<std::streamsize>(blob.size()));
        PLOG_INFO("[PULSAR] [LobbyHandoff] wrote profile-out ({} bytes) -> {}"sv,
                  blob.size(), path);
    }

    // FNV-1a over the blob - cheap change-detection so the periodic clean
    // capture doesn't re-write (and re-upload) an identical profile.
    static uint64_t BlobHash(const std::vector<uint8_t>& blob)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t b : blob) { h ^= b; h *= 1099511628211ull; }
        return h;
    }

    void WriteProfileOut()
    {
        std::vector<uint8_t> blob = Pulsar::Profile::SerializeProfile();
        if (blob.empty()) return;
        WriteProfileBlob(blob);
    }

    static std::atomic<bool> g_autoSaveRequested{ false };
    static uint64_t          s_lastAutoSaveMs = 0;

    void RequestAutoSave()
    {
        g_autoSaveRequested.store(true, std::memory_order_relaxed);
    }

    void TickAutoSave()
    {
        if (!g_autoSaveRequested.exchange(false, std::memory_order_relaxed)) return;

        // Phase A - capture only when the live inventory is the CLIENT's own:
        // own save pre-bridge, OR post-restore (host items wiped → live is the
        // client's items + co-op loot). A disconnect AFTER the restore now
        // captures the session's gains (not host poison - host items are gone).
        // A foreign/host save before its restore is never captured.
        // Phase C - when DB-authoritative, NEVER upload pre-bridge: the loaded save
        // may be stale and would overwrite the DB's newer gains. Only the
        // post-restore upload (live = DB + gains) runs in that mode. With an empty
        // DB (bootstrap, dbAuth false) the pre-bridge upload fills the DB.
        const bool ownPreBridge =
            !Pulsar::InventorySync::DirectRestore::HasBridgedThisSession() &&
            Pulsar::InventorySync::DirectRestore::LoadedSaveIsOwn() &&
            !Pulsar::InventorySync::DirectRestore::IsDbAuthoritative();
        // Phase C3 - the post-restore upload IS the co-op gains-capture. In dbAuth
        // mode CaptureProfile does a delta-merge (frozen DB baseline + session
        // delta), which is drift-safe now that mod-restore is faithful - so the
        // upload is re-enabled. It must wait for the +10s gains snapshot baseline
        // (per-function guard below), else the delta has no reference. Legacy
        // (non-dbAuth / bootstrap) co-op uploads as before.
        // FIX (2026-06-21): the post-restore upload is ONLY safe in dbAuth
        // mode, where RestoreClientState wiped the host items and applied the
        // client's DB loadout (live = DB + gains). In BOOTSTRAP (empty DB) there
        // is no loadout to restore, so a post-bridge "post-restore" live state is
        // still the HOST's bridged items → uploading poisons the DB. Require
        // dbAuth here; bootstrap may upload ONLY via ownPreBridge (clean/solo).
        const bool clientPostRestore =
            Pulsar::InventorySync::DirectRestore::IsDbAuthoritative() &&
            Pulsar::InventorySync::DirectRestore::RestoreComplete();
        if (!ownPreBridge && !clientPostRestore) {
            PLOG_INFO("[PULSAR] [LobbyHandoff] auto-save SKIPPED - live inventory "
                      "not client-owned yet (pre-restore foreign/bridge state) "
                      "[ownPreBridge={} dbAuth={} bridged={}]"sv,
                      ownPreBridge,
                      Pulsar::InventorySync::DirectRestore::IsDbAuthoritative(),
                      Pulsar::InventorySync::DirectRestore::HasBridgedThisSession());
            return;
        }

        // Phase C3 - in dbAuth mode the upload is a gains delta-capture; wait for
        // the +10s post-restore snapshot baseline (else there's no delta reference).
        // Re-arm so a fast disconnect still captures gains once the baseline lands.
        if (Pulsar::InventorySync::DirectRestore::IsDbAuthoritative() &&
            !Pulsar::InventorySync::DirectRestore::IsGainsSnapshotValid()) {
            g_autoSaveRequested.store(true, std::memory_order_relaxed);
            return;
        }

        // Safe-point: only capture when the player is fully spawned (reuse the
        // connect-gate that TickApplyProfile uses). If not ready, re-arm and retry.
        if (!Pulsar::InventorySync::DirectRestore::IsPlayerReadyForConnect()) {
            g_autoSaveRequested.store(true, std::memory_order_relaxed);
            return;
        }

        // Debounce bursts (e.g. several disconnects in a row). Drop - the next
        // trigger will capture the latest state.
        uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (s_lastAutoSaveMs != 0 && now - s_lastAutoSaveMs < 15000) return;
        s_lastAutoSaveMs = now;

        WriteProfileOut();
        PLOG_INFO("[PULSAR] [LobbyHandoff] auto-save captured profile-out"sv);
    }

    void TickCleanProfileCapture()
    {
        // Capture only when the live inventory is the CLIENT's own:
        //   (a) own save, before any bridge (solo / pre-join), OR
        //   (b) post-restore during co-op - RestoreClientState wiped the host
        //       items, so the live inventory is the client's items + the loot
        //       gained THIS session. This is how co-op gains reach the DB
        //       (Phase A). The engine's leave-co-op auto-save is the host's save
        //       and the client's own save is stale, so only the DB can hold them.
        // A foreign/host save BEFORE its restore is never captured (poison).
        // Phase C - when DB-authoritative, NEVER upload pre-bridge: the loaded save
        // may be stale and would overwrite the DB's newer gains. Only the
        // post-restore upload (live = DB + gains) runs in that mode. With an empty
        // DB (bootstrap, dbAuth false) the pre-bridge upload fills the DB.
        const bool ownPreBridge =
            !Pulsar::InventorySync::DirectRestore::HasBridgedThisSession() &&
            Pulsar::InventorySync::DirectRestore::LoadedSaveIsOwn() &&
            !Pulsar::InventorySync::DirectRestore::IsDbAuthoritative();
        // Phase C3 - the post-restore upload IS the co-op gains-capture. In dbAuth
        // mode CaptureProfile does a delta-merge (frozen DB baseline + session
        // delta), which is drift-safe now that mod-restore is faithful - so the
        // upload is re-enabled. It must wait for the +10s gains snapshot baseline
        // (per-function guard below), else the delta has no reference. Legacy
        // (non-dbAuth / bootstrap) co-op uploads as before.
        // FIX (2026-06-21): see TickAutoSave - post-restore upload is only
        // safe in dbAuth mode. In bootstrap a post-bridge live state is the
        // HOST's items; require dbAuth so bootstrap uploads ONLY via ownPreBridge.
        const bool clientPostRestore =
            Pulsar::InventorySync::DirectRestore::IsDbAuthoritative() &&
            Pulsar::InventorySync::DirectRestore::RestoreComplete();
        if (!ownPreBridge && !clientPostRestore) return;

        // Phase C3 - dbAuth gains delta-capture needs the +10s snapshot baseline
        // first; until then skip (the periodic tick retries).
        if (Pulsar::InventorySync::DirectRestore::IsDbAuthoritative() &&
            !Pulsar::InventorySync::DirectRestore::IsGainsSnapshotValid())
            return;

        const bool ready =
            Pulsar::InventorySync::DirectRestore::IsPlayerReadyForConnect();

        static bool     s_lastReady   = false;
        static uint64_t s_lastWriteMs = 0;
        static uint64_t s_lastHash    = 0;

        const bool transitioned = ready && !s_lastReady;  // a fresh save just loaded
        s_lastReady = ready;
        if (!ready) return;

        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        // Capture on a fresh save-load (immediately) or at a slow steady-state
        // cadence so in-game solo progression travels to the next session.
        constexpr uint64_t kPeriodMs = 120000;  // 2 min
        if (!transitioned && s_lastWriteMs != 0 && (now - s_lastWriteMs) < kPeriodMs) {
            return;
        }

        std::vector<uint8_t> blob = Pulsar::Profile::SerializeProfile();
        if (blob.empty()) return;

        const uint64_t h = BlobHash(blob);
        if (h == s_lastHash) {            // unchanged since last write - skip the upload
            s_lastWriteMs = now;
            return;
        }
        s_lastHash    = h;
        s_lastWriteMs = now;

        WriteProfileBlob(blob);
        PLOG_INFO("[PULSAR] [LobbyHandoff] clean profile-capture ({}) - host-free "
                  "(pre-bridge), {} bytes"sv,
                  transitioned ? "save-load" : "periodic", blob.size());
    }
}
