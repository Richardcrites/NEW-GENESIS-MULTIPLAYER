using System;
using System.Runtime.InteropServices;

namespace Pulsar.Companion.Voice;

// PTT key / mouse-button state via GetAsyncKeyState.
//
// We deliberately do NOT use WH_KEYBOARD_LL / WH_MOUSE_LL hooks: Starfield grabs
// the mouse (and keys) via raw input while focused, which suppresses the legacy
// window messages those low-level hooks depend on, so held PTT buttons were never
// seen in-game (confirmed 2026-07-04: micOpen stayed False while Mouse4 held).
//
// GetAsyncKeyState reads the OS async key-state table directly and reflects the
// physical state regardless of focus / raw input. Works for keyboard VKs AND
// mouse buttons (0x01 L, 0x02 R, 0x04 middle, 0x05 XBUTTON1 "Mouse 4",
// 0x06 XBUTTON2 "Mouse 5").
//
// Caveat: if the mouse's own driver remaps a side button to something else, the
// OS never sees XBUTTON1/2 and no method can detect it - that's a mouse-config
// issue, not ours.
public sealed class PttHook : IDisposable
{
    public const string BuildTag = "dll-ptt-v2-settingsHB";

    // High bit (0x8000) set => the key/button is currently down.
    public bool IsKeyDown(int vk) => vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;

    // Diagnostic: which VKs are currently down (hex). Lets us see what a mouse
    // side button actually produces (0x05/0x06, a remapped key, or nothing).
    public string DownKeysHex()
    {
        var sb = new System.Text.StringBuilder();
        for (int vk = 0x01; vk <= 0xFE; vk++)
        {
            if (vk == 0x03) continue;   // VK_CANCEL noise
            if ((GetAsyncKeyState(vk) & 0x8000) != 0)
                sb.Append("0x").Append(vk.ToString("X2")).Append(' ');
        }
        return sb.Length == 0 ? "(none)" : sb.ToString().Trim();
    }

    public void Dispose() { }

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);
}
