Scriptname PulsarRemoteControl extends Quest conditional
{PULSAR Co-op Bridge: Spawns remote player avatar, writes FormID to GLOBs.
C++ handles movement at framerate when it finds the actor (PulsarSpawn=3.0).
Papyrus provides fallback movement when C++ hasn't taken over (PulsarSpawn=2.0).}

; RECONSTRUCTION, NOT THE ORIGINAL SOURCE. UNTESTED.
;
; The original .psc for this script was lost. Only the compiled
; Scripts/PulsarRemoteControl.pex survives, and that .pex is what still ships
; and still runs. This file was rebuilt from three sources that agree with each
; other:
;
;   1. The .pex itself. Its string table names every function, every local
;      variable and every log line, and its debug section gives the line span
;      of each function in the lost file. Both are reproduced here.
;   2. The C++ side of the protocol, which is complete: see
;      RemotePlayerManager::DoSpawn / FindSpawnedActor / FindLookMarker /
;      DoDespawn and the trampoline block in NPCSync.cpp.
;   3. docs/PULSAR_Technical.md section 4, which documents the state machine.
;
; What that does NOT give back is the body of each function, so the control
; flow below is inferred. It follows the protocol the C++ side expects, but it
; has never been compiled against the Creation Kit and never run in a two
; player session, which is the only way to prove it behaves like the .pex.
;
; DO NOT compile this over Scripts/PulsarRemoteControl.pex expecting a
; no-op. Treat it as a starting point: read it, fix it, test it with two
; players, and only then replace the binary.

GlobalVariable Property PulsarPosX Auto
GlobalVariable Property PulsarPosY Auto
GlobalVariable Property PulsarPosZ Auto
GlobalVariable Property PulsarRotZ Auto
GlobalVariable Property PulsarSpeed Auto
GlobalVariable Property PulsarDirty Auto
GlobalVariable Property PulsarSpawn Auto
GlobalVariable Property PulsarFormHi Auto
GlobalVariable Property PulsarFormLo Auto
GlobalVariable Property PulsarLookMarkerHi Auto
GlobalVariable Property PulsarLookMarkerLo Auto
GlobalVariable Property PulsarDropHi Auto
GlobalVariable Property PulsarDropLo Auto

Actor RemotePlayer
ObjectReference LookMarker
Float fCloneX
Float fCloneY
Float fCloneZ
Float fCloneRotZ
Bool bGlobsReady
Bool bFirstMove

Event OnInit()
    Debug.Trace("[PULSAR] PulsarRemoteControl: Quest initialized")
    StartTimer(0.05)
EndEvent

Event OnTimer(Int aiTimerID)
    ; Re-arm FIRST. If anything below throws, the Papyrus VM aborts the rest
    ; of this frame's body, and a StartTimer at the bottom would never run:
    ; the bridge would go silent for the rest of the session with no error
    ; the player could see. Arming here costs nothing and cannot be skipped.
    StartTimer(0.05)

    If (!bGlobsReady)
        EnsureGlobs()
    EndIf

    DoTick()
EndEvent

; Resolves the GLOBs by plugin + local FormID instead of Creation Kit
; properties, so the bridge does not care where PULSAR.esp sits in the load
; order. Called every tick until it succeeds; PULSAR.esp may not be resolvable
; on the very first frames.
Function EnsureGlobs()
    PulsarPosX = Game.GetFormFromFile(0x00000800, "PULSAR.esp") as GlobalVariable
    PulsarPosY = Game.GetFormFromFile(0x00000801, "PULSAR.esp") as GlobalVariable
    PulsarPosZ = Game.GetFormFromFile(0x00000802, "PULSAR.esp") as GlobalVariable
    PulsarRotZ = Game.GetFormFromFile(0x00000803, "PULSAR.esp") as GlobalVariable
    PulsarSpeed = Game.GetFormFromFile(0x00000804, "PULSAR.esp") as GlobalVariable
    PulsarDirty = Game.GetFormFromFile(0x00000805, "PULSAR.esp") as GlobalVariable
    PulsarSpawn = Game.GetFormFromFile(0x00000806, "PULSAR.esp") as GlobalVariable
    PulsarFormHi = Game.GetFormFromFile(0x00000808, "PULSAR.esp") as GlobalVariable
    PulsarFormLo = Game.GetFormFromFile(0x00000809, "PULSAR.esp") as GlobalVariable
    PulsarLookMarkerHi = Game.GetFormFromFile(0x0000080A, "PULSAR.esp") as GlobalVariable
    PulsarLookMarkerLo = Game.GetFormFromFile(0x0000080B, "PULSAR.esp") as GlobalVariable
    PulsarDropHi = Game.GetFormFromFile(0x0000080D, "PULSAR.esp") as GlobalVariable
    PulsarDropLo = Game.GetFormFromFile(0x0000080E, "PULSAR.esp") as GlobalVariable

    bGlobsReady = PulsarSpawn && PulsarFormHi && PulsarFormLo && PulsarPosX && PulsarDirty
    bFirstMove = true
EndFunction

; The whole protocol is one number. C++ writes PulsarSpawn, we react, we write
; it back. Values above 9 are the NPC trampoline (NPCSync), not the player clone.
;
;   0.0  C++ wants the clone gone
;   1.0  C++ wants a clone           -> we spawn, then 2.0
;   2.0  clone exists, C++ has not taken over yet -> we drive movement
;   3.0  C++ owns the clone          -> hands off, it moves it every frame
;  10.0  spawn the NPC in PulsarDropHi/Lo -> we spawn, then 11.0
;  11.0  NPC spawn done, result in PulsarDropHi/Lo (C++ collects it)
;  12.0  disable the native REFR in PulsarDropHi/Lo -> we disable, then 3.0
Function DoTick()
    If (!bGlobsReady)
        Debug.Trace("[PULSAR] DoTick: skip, bGlobsReady=" + bGlobsReady)
        Return
    EndIf

    Float spawnVal = PulsarSpawn.GetValue()

    If (spawnVal == 1.0)
        If (RemotePlayer)
            ; A save was loaded under a live clone, or C++ re-requested without
            ; a despawn in between. Tear the old one down first, otherwise we
            ; leak an actor that nothing owns any more.
            Debug.Trace("[PULSAR] DoTick: spawnVal=1.0 with stale RemotePlayer, clearing first")
            DoDespawn()
        EndIf
        Debug.Trace("[PULSAR] DoTick: spawnVal=1.0, calling DoSpawn")
        DoSpawn()

    ElseIf (spawnVal == 0.0)
        If (RemotePlayer)
            DoDespawn()
        EndIf

    ElseIf (spawnVal == 2.0)
        ; C++ has not picked the actor up yet. Keep it moving from Papyrus so
        ; the clone does not stand frozen at the spawn point in the meantime.
        DoMove()

    ElseIf (spawnVal == 10.0)
        DoNpcSpawn()

    ElseIf (spawnVal == 12.0)
        DoDisableNative()
    EndIf

    ; Head tracking has to be re-applied: the engine drops the LookAt target
    ; on cell changes, and the clone then stares straight ahead.
    If (RemotePlayer && LookMarker)
        RemotePlayer.SetLookAt(LookMarker)
    EndIf
EndFunction

; PlaceAtMe has to run on the game thread. Papyrus inherently does, which is
; the reason this bridge exists at all.
Function DoSpawn()
    Debug.Trace("[PULSAR] Spawning remote player avatar...")

    Actor playerActor = Game.GetPlayer()
    ActorBase playerBase = playerActor.GetLeveledActorBase()

    ; persist=true: the clone must survive a cell unload, C++ holds a pointer.
    ObjectReference newRef = playerActor.PlaceAtMe(playerBase, 1, true, false, false)
    If (!newRef)
        Debug.Trace("[PULSAR] ERROR: PlaceAtMe returned None!")
        Return
    EndIf

    RemotePlayer = newRef as Actor
    Int formID = newRef.GetFormID()

    ; FormIDs do not survive a round trip through a Float GLOB: above 2^24 the
    ; mantissa runs out and the value comes back rounded. Split into two halves
    ; that each stay small enough. Papyrus Int division is signed, so a 0xFF
    ; dynamic FormID splits into two negative halves; C++ reassembles with the
    ; matching signed arithmetic (hi * 65536 + lo), NOT with a bit shift.
    Int hi = formID / 65536
    Int lo = formID - hi * 65536
    PulsarFormHi.SetValue(hi as Float)
    PulsarFormLo.SetValue(lo as Float)

    Debug.Trace("[PULSAR] Remote player spawned: " + RemotePlayer + " FormID=" + formID)
    Debug.Trace("[PULSAR] FormID split: hi=" + hi + " lo=" + lo)

    ; Hand over. C++ polls for 2.0, resolves the actor from the two halves,
    ; installs its hooks and then writes 3.0 itself.
    PulsarSpawn.SetValue(2.0)

    ; XMarker, vanilla Starfield.esm 0x3B. The clone needs something to look
    ; at; C++ moves this marker to wherever the remote player is aiming.
    Form markerForm = Game.GetFormFromFile(0x0000003B, "Starfield.esm")
    If (!markerForm)
        Debug.Trace("[PULSAR] ERROR: XMarker 0x3B not found in Starfield.esm!")
        Return
    EndIf

    LookMarker = playerActor.PlaceAtMe(markerForm, 1, true, false, false)
    If (!LookMarker)
        Debug.Trace("[PULSAR] ERROR: LookMarker PlaceAtMe returned None!")
        Return
    EndIf

    LookMarker.SetPosition(playerActor.GetPositionX(), playerActor.GetPositionY(), playerActor.GetPositionZ())

    Int mID = LookMarker.GetFormID()
    Int mHi = mID / 65536
    Int mLo = mID - mHi * 65536
    PulsarLookMarkerHi.SetValue(mHi as Float)
    PulsarLookMarkerLo.SetValue(mLo as Float)
    Debug.Trace("[PULSAR] LookMarker spawned: FormID=" + mID + " hi=" + mHi + " lo=" + mLo)

    RemotePlayer.SetLookAt(LookMarker)
    bFirstMove = true
EndFunction

Function DoDespawn()
    Debug.Trace("[PULSAR] Despawning remote player...")

    If (RemotePlayer)
        RemotePlayer.Disable()
        RemotePlayer.Delete()
        RemotePlayer = None
    EndIf

    If (LookMarker)
        LookMarker.Disable()
        LookMarker.Delete()
        LookMarker = None
    EndIf

    ; Clear the halves. A stale FormID here is worse than none: after a save
    ; load C++ would resolve it, find some unrelated form, and drive that.
    PulsarFormHi.SetValue(0.0)
    PulsarFormLo.SetValue(0.0)
    PulsarLookMarkerHi.SetValue(0.0)
    PulsarLookMarkerLo.SetValue(0.0)

    bFirstMove = true
EndFunction

; NPC trampoline. C++ cannot call the engine's PlaceActorAtMe directly, it is
; framed for the Papyrus VM, so NPCSync routes NPC spawns through here the same
; way the player clone goes through DoSpawn. One spawn in flight at a time;
; C++ serializes that side.
;
; NOTE: on the C++ side this path is currently compiled out. The protocol is
; kept intact here because the .pex still implements it and re-enabling it is a
; one line change.
Function DoNpcSpawn()
    Int hi = PulsarDropHi.GetValue() as Int
    Int lo = PulsarDropLo.GetValue() as Int
    Int localID = hi * 65536 + lo
    Debug.Trace("[PULSAR] DoNpcSpawn: localID=0x" + localID)

    Form baseForm = FindFormInAnyPlugin(localID)
    If (!baseForm)
        Debug.Trace("[PULSAR] DoNpcSpawn: no known plugin has localID=0x" + localID)
        PulsarDropHi.SetValue(0.0)
        PulsarDropLo.SetValue(0.0)
        PulsarSpawn.SetValue(11.0)
        Return
    EndIf

    ObjectReference newRef = Game.GetPlayer().PlaceAtMe(baseForm, 1, false, false, false)
    If (!newRef)
        Debug.Trace("[PULSAR] DoNpcSpawn: PlaceAtMe returned None for localID=0x" + localID)
        PulsarDropHi.SetValue(0.0)
        PulsarDropLo.SetValue(0.0)
        PulsarSpawn.SetValue(11.0)
        Return
    EndIf

    ; SetPosition immediately, still inside the same frame as the PlaceAtMe.
    ; PlaceAtMe drops the actor at the player; if we let C++ move it afterwards
    ; by writing its location directly, the engine has already decided the
    ; actor spawned far away and skips streaming its 3D, permanently. Moving it
    ; here goes through the engine's own cell re-attach path instead.
    Float tgtX = PulsarPosX.GetValue()
    Float tgtY = PulsarPosY.GetValue()
    Float tgtZ = PulsarPosZ.GetValue()
    newRef.SetPosition(tgtX, tgtY, tgtZ)

    Int newID = newRef.GetFormID()
    Int newHi = newID / 65536
    Int newLo = newID - newHi * 65536
    PulsarDropHi.SetValue(newHi as Float)
    PulsarDropLo.SetValue(newLo as Float)
    Debug.Trace("[PULSAR] DoNpcSpawn: OK localID=0x" + localID + " -> refID=0x" + newID)

    PulsarSpawn.SetValue(11.0)
EndFunction

; The client owns a native copy of a static NPC that the host also has. Once we
; have spawned our own synced copy, the native one has to go, or the player
; sees the same character twice.
Function DoDisableNative()
    Int hi = PulsarDropHi.GetValue() as Int
    Int lo = PulsarDropLo.GetValue() as Int
    Int localID = hi * 65536 + lo
    Debug.Trace("[PULSAR] DoDisableNative: localID=0x" + localID)

    Form nativeForm = FindFormInAnyPlugin(localID)
    If (!nativeForm)
        Debug.Trace("[PULSAR] DoDisableNative: no known plugin has localID=0x" + localID)
        PulsarDropHi.SetValue(0.0)
        PulsarDropLo.SetValue(0.0)
        PulsarSpawn.SetValue(3.0)
        Return
    EndIf

    ObjectReference nativeRef = nativeForm as ObjectReference
    If (!nativeRef)
        Debug.Trace("[PULSAR] DoDisableNative: form 0x" + localID + " not ObjectReference")
    Else
        nativeRef.Disable()
        Debug.Trace("[PULSAR] DoDisableNative: OK localID=0x" + localID + " disabled")
    EndIf

    PulsarDropHi.SetValue(0.0)
    PulsarDropLo.SetValue(0.0)
    PulsarSpawn.SetValue(3.0)
EndFunction

; C++ hands over a plugin-local FormID without saying which plugin it came
; from, so try each one the game may have loaded until a lookup answers.
; Order matters only for speed: base game first, the rest are rare.
;
; In the original this loop was inlined in both callers; it is factored out
; here. If you compare against a decompile of the .pex, that is the one
; structural difference you will see.
Form Function FindFormInAnyPlugin(Int aiLocalID)
    Form f = Game.GetFormFromFile(aiLocalID, "Starfield.esm")
    If (f)
        Return f
    EndIf

    String[] plugins = New String[13]
    plugins[0] = "ShatteredSpace.esm"
    plugins[1] = "Constellation.esm"
    plugins[2] = "OldMars.esm"
    plugins[3] = "SFBGS003.esm"
    plugins[4] = "SFBGS004.esm"
    plugins[5] = "SFBGS006.esm"
    plugins[6] = "SFBGS007.esm"
    plugins[7] = "SFBGS008.esm"
    plugins[8] = "SFBGS00D.esm"
    plugins[9] = "SFBGS047.esm"
    plugins[10] = "SFBGS050.esm"
    plugins[11] = "BlueprintShips-Starfield.esm"
    plugins[12] = "BlueprintShips-SFBGS050.esm"

    Int i = 0
    While (i < plugins.Length)
        f = Game.GetFormFromFile(aiLocalID, plugins[i])
        If (f)
            Return f
        EndIf
        i += 1
    EndWhile

    Return None
EndFunction

; Fallback movement, only while PulsarSpawn is 2.0. Once C++ takes over at 3.0
; it drives the clone every frame and this never runs again. Papyrus is far too
; slow to be the real movement path: at 50 ms per tick the clone would visibly
; stutter behind the other player.
Function DoMove()
    If (!RemotePlayer)
        Return
    EndIf

    Float dirtyVal = PulsarDirty.GetValue()
    If (dirtyVal == 0.0)
        Return
    EndIf
    PulsarDirty.SetValue(0.0)

    Float targetX = PulsarPosX.GetValue()
    Float targetY = PulsarPosY.GetValue()
    Float targetZ = PulsarPosZ.GetValue()
    Float targetRotZ = PulsarRotZ.GetValue()

    Float dx = targetX - fCloneX
    Float dy = targetY - fCloneY
    Float dz = targetZ - fCloneZ
    Float distXY = Math.sqrt(dx * dx + dy * dy)
    Float rotDiff = targetRotZ - fCloneRotZ

    ; Only touch the engine when something actually changed. SetPosition is not
    ; free, and calling it every tick on an unmoved actor fights the engine's
    ; own physics for the same transform.
    If (bFirstMove || distXY > 1.0 || Math.abs(dz) > 1.0)
        RemotePlayer.SetPosition(targetX, targetY, targetZ)
        fCloneX = targetX
        fCloneY = targetY
        fCloneZ = targetZ
    EndIf

    If (bFirstMove || Math.abs(rotDiff) > 1.0)
        RemotePlayer.SetAngle(0.0, 0.0, targetRotZ)
        fCloneRotZ = targetRotZ
    EndIf

    bFirstMove = false
EndFunction
