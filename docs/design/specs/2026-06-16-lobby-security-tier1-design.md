# Lobby Backend Security - Tier 1 (Pre-Online Hardening) - Design

**Datum:** 2026-06-16
**Status:** Design (genehmigt: Tier-1 jetzt, Tier-2 in Beta)

## Ziel
Die Lese-Exposition und Transport-Lücken schließen, **bevor** das SpacetimeDB-Backend
online auf dem VPS gehostet wird - ohne echte User-Accounts (die kommen als Tier 2 in der Beta).

## Kontext (Ist-Stand, auditiert)
`lobby/server/spacetimedb/Lib.cs` - **Write-Autorisierung ist bereits korrekt:** alle
mutierenden Reducer gaten auf `ctx.Sender`, Identity-als-Argument-Spoofing ist unmöglich.
`UpsertProfile` schreibt nur in die eigene Row → fremde Profile sind nicht überschreibbar.

**Die echten Lücken:**
1. **Lese-Sichtbarkeit (Hauptloch):** alle Tabellen `Public=true`. Die `Profile`-Tabelle ist
   welt-lesbar; jeder Client kann `SELECT * FROM profile` und **alle** Profil-Blobs laden.
   Der Companion macht das selbst (`Subscribe("SELECT * FROM profile")` + lokal filtern).
2. **Token at rest:** anonyme STDB-Identity = Bearer-JWT im Klartext in `companion.token`.
3. **Transport:** lokal `http://`(ws); auf dem VPS aktuell ungeschützt.
4. **Input-Härtung:** kein Blob-Größen-Cap (Storage-DoS), keine String-Längen-Limits,
   `HostAddr` ist client-kontrolliert.

## Scope (Tier 1)
1. **Profil-Lese-Isolation** - `Profile` nicht mehr welt-lesbar; ein Client sieht nur die eigene Row.
2. **LobbyMember nicht leaken** - Mitglieder-Identitäten nicht öffentlich; nur Mitglieder-*Zahl* zeigen.
3. **Reducer-Input-Härtung** - Blob-Größe, String-Längen, `HostAddr`-Format, Status-Whitelist.
4. **Transport** - TLS (`wss://`) für den VPS; Companion-URI konfigurierbar statt hartkodiert.
5. **Token at rest** - `companion.token` per Windows-DPAPI verschlüsseln.

## Out of Scope (Tier 2 / Beta+)
OIDC-Accounts, Login/Recovery/Revoke, Bann-Liste, private Lobbies/Invite-Codes,
Rate-Limiting, Audit-Log.

## Design

### 1. Profil-Lese-Isolation
Zwei Kandidaten-Mechanismen in SpacetimeDB; Entscheidung im Plan (kurzer Spike):
- **(A) Per-user View** - `Profile` privat machen (`Public=true` entfernen) + eine
  `[SpacetimeDB.View(Accessor="MyProfile", Public=true)]`, die
  `ctx.Db.Profile.Identity.Find(ctx.Sender)` zurückgibt. Companion liest die View.
- **(B) Row-Level-Security** - Tabelle abonnierbar lassen, aber Client-Visibility-Filter
  auf `:sender` einschränken.

**Entscheidungskriterium:** Kann der C#-ClientSDK 2.5 eine View abonnieren? Falls ja → (A)
(klarer, denormalisiert nichts); falls nein → (B). **Invariante in beiden Fällen:** ein Client
empfängt ausschließlich seine eigene `Profile`-Row. Die Owner-Identity (Modul-Publisher) behält
vollen Zugriff (Operator).

**Companion-Änderung:** `pull-profile` und der GUI-`serve`-Modus dürfen **nicht** mehr
`SELECT * FROM profile` nutzen → View/gefilterte Subscription. `FindMyProfile` gibt dann
trivial die einzige Row zurück.

### 2. LobbyMember nicht leaken
- `LobbyMember` privat machen (`Public=true` entfernen).
- Denormalisiertes Feld `MemberCount` (uint) auf `Lobby`, gepflegt in `JoinLobby` (+1),
  `LeaveLobby` (−1, floor 0) und `HandleDisconnect` (auf 0 / korrekt nachziehen).
- Der Server-Browser zeigt „MemberCount / MaxPlayers" ohne je Identitäten zu leaken.
- `Lobby` und `Account` bleiben public (Browser braucht Lobby-Liste + Host-Anzeigename;
  Anzeigenamen sind niedrig sensibel).

### 3. Reducer-Input-Härtung
- `UpsertProfile`: `blob.Count > MAX_BLOB_BYTES (262144 = 256 KB)` → `throw`.
- `RegisterAccount`: `displayName` trimmen, Länge 1..32, sonst `throw`.
- `CreateLobby`: `name` Länge 1..48; `maxPlayers` in 1..8; `hostAddr` Länge ≤ 48 und
  Format `host:port` (einfacher Validator: enthält genau einen `:`, Port 1..65535) → sonst `throw`.
- `UpdateLobbyStatus`: `status ∈ {"open","in_game","closed"}` → sonst `throw`.

### 4. Transport (TLS)
- SpacetimeDB self-hosted auf dem VPS hinter TLS-Reverse-Proxy (Caddy/nginx, terminiert
  `https`→`ws`) **oder** STDB-natives TLS. Companion-`Uri` → `wss://<vps-host>`.
- Companion liest URI + Modulname aus Env/Config (`PULSAR_STDB_URI`, `PULSAR_STDB_DB`),
  Default lokal (`http://127.0.0.1:3000`, `pulsar-lobby`) für Dev.
- **Verify:** STDB-ClientSDK akzeptiert `WithUri("https://…")` und upgradet auf `wss`.

### 5. Token at rest (DPAPI)
- Vor dem Schreiben mit `System.Security.Cryptography.ProtectedData.Protect(bytes, null,
  DataProtectionScope.CurrentUser)`; beim Lesen `Unprotect`. Fällt bei Entschlüsselungs-Fehler
  auf Klartext zurück (Migration alter Tokens), dann beim nächsten Save neu verschlüsselt.
- Datei wird Base64(ciphertext); Format-Marker, damit alte Klartext-Tokens erkannt werden.

## Tests
- **Modul-Lese-Isolation:** als Nicht-Owner-Identity `spacetime sql <db> "SELECT * FROM profile"`
  → liefert nur eigene/keine Rows. (Owner-SQL sieht alle - erwartet.)
- **Reducer-Caps:** `spacetime call` mit Übergröße-Blob / langem String / krummem `hostAddr`
  → Reducer wirft, keine Row entsteht.
- **Companion-Round-Trip:** push→pull über den View/RLS-Pfad → Blob MATCH wie bisher.
- **DPAPI:** Token-Datei ist auf der Platte Ciphertext; Reconnect nutzt dieselbe Identity.
- **TLS:** Smoke gegen einen `wss`-Endpunkt (oder als VPS-Deploy-Schritt dokumentiert).

## Risiken / während Implementierung verifizieren
- View-Subscribe-Support im C#-SDK 2.5 (sonst RLS).
- RLS-Syntax/-Verfügbarkeit in der installierten STDB-Version.
- STDB-self-hosted-TLS-Konfiguration (Proxy vs. nativ).

## Reihenfolge
Tier 1 sollte **vor** oder zeitgleich mit der In-Game-GUI landen, damit der GUI-`serve`-Daemon
von Anfang an den sicheren Lese-Pfad (View/RLS) verwendet - kein nachträglicher Umbau.
