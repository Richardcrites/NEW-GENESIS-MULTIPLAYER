Scriptname PulsarBridge extends ScriptObject Native Hidden

; Called from C++ via DispatchStaticCall
; Moves a reference smoothly to coordinates with animation
Function DoTranslateTo(ObjectReference akRef, Float afX, Float afY, Float afZ, Float afAngleZ, Float afSpeed) Global Native
