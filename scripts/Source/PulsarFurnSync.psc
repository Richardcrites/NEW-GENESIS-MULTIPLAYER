Scriptname PulsarFurnSync extends ScriptObject Hidden

; Vehicle-sync helper called from C++ via DispatchStaticCall.
; When the host enters a map-placed FURN (Moon Jumper etc.) but the client
; doesn't have a matching FURN-REFR nearby, the host-driven FURN is spawned
; here as a temporary representation. Clone sits in it, FURN position is
; driven by host's pose from C++ side.

; Spawns akBase at akClone's position, moves to (x,y,z,angleZ), then activates
; the new FURN with akClone as action-ref - engine-native sit sequence plays
; and the clone gets anchored to the FURN's sit marker.
; Returns the spawned ObjectReference (or None on failure) via GLOB.
ObjectReference Function SpawnAndSitForClone(Form akBase, Float afX, Float afY, Float afZ, Float afAngleZ, Actor akClone) Global
    if !akClone || !akBase
        return None
    endif

    ObjectReference furn = akClone.PlaceAtMe(akBase, 1, false, false, false)
    if !furn
        return None
    endif

    furn.SetPosition(afX, afY, afZ)
    furn.SetAngle(0.0, 0.0, afAngleZ)
    furn.Activate(akClone, false)
    return furn
EndFunction

; Deletes a previously-spawned FURN when the host unseats.
Function DespawnFurn(ObjectReference akFurn) Global
    if akFurn
        akFurn.Delete()
    endif
EndFunction

; Flips a reference's Havok body motion type. For Moon Jumper sync:
; Keyframed (4) = physics-inactive, teleportable via SetPosition.
; Dynamic   (1) = physics-active (default for vehicles - Havok drives it).
Function DoSetMotionType(ObjectReference akRef, int aiMotionType) Global
    if akRef
        akRef.SetMotionType(aiMotionType, false)
    endif
EndFunction

; Cell-transition fix: forces the engine's cell-attach pipeline to re-init
; a REFR's 3D state. After a mid-loading-screen PlaceAtMe respawn, the clone
; ends up with data3D allocated but NiNode children=0 (no biped/skeleton).
; MoveTo with abCopyCell=true triggers the engine's normal cell-attach path
; which DOES attach the biped properly. Bethesda removed Enable from Papyrus
; in Starfield, so the simpler Disable→Enable cycle isn't available.
Function DoMoveRefresh(ObjectReference akRef, ObjectReference akTarget) Global
    if akRef && akTarget
        akRef.MoveTo(akTarget, 0.0, 0.0, 0.0, true, true)
    endif
EndFunction

; K.2 V5 - Auto-Follow through doors. Called from C++ when peer transitioned
; through a static (plugin-FormID) door. The destination ref is the door on
; the OTHER side of the threshold (extracted from source door's ExtraTeleport
; on receiver side). MoveTo here lets the engine handle the cell-load +
; teleport - same pipeline the engine uses when activating a door normally,
; but bypasses the "player must be near door" cross-cell precondition that
; blocks our direct C++ Activate call.
;
; abMatchRotation=false: keep player's current facing (server-side rotation
; would just re-snap on next position-sync packet).
Function DoPlayerFollowMoveTo(ObjectReference akDestinationRef) Global
    if akDestinationRef
        Game.GetPlayer().MoveTo(akDestinationRef, 0.0, 0.0, 0.0, false, false)
    endif
EndFunction

; K.2 V6 - Auto-Follow generic ref. Caller passes a known-loaded ref
; that's near peer's position (typically peer's clone). MoveTo it to
; let engine handle cell-load + teleport.
Function DoFollowToRef(ObjectReference akTargetRef) Global
    if akTargetRef
        Game.GetPlayer().MoveTo(akTargetRef, 0.0, 0.0, 0.0, false, false)
    endif
EndFunction

