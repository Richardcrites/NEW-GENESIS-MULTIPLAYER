using Avalonia.Input;

namespace Pulsar.Services;

// Maps an Avalonia Key to the VK-name string the DLL's ParseVKey understands:
//   F1..F24, A..Z, 0..9, Numpad0..Numpad9, or raw 0xNN.
// Designed to never throw on odd keys: unsupported keys return null so the caller
// can show a small "unsupported key" note and keep the previous binding.
public static class KeyName
{
    // Returns the PULSAR ini name for the key, or null if it can't be represented.
    public static string? FromAvalonia(Key key)
    {
        // Function keys F1..F24 (Avalonia Key.F1..Key.F24 are contiguous).
        if (key >= Key.F1 && key <= Key.F24)
            return "F" + (int)(key - Key.F1 + 1);

        // Letters A..Z.
        if (key >= Key.A && key <= Key.Z)
            return ((char)('A' + (key - Key.A))).ToString();

        // Top-row digits D0..D9 -> "0".."9".
        if (key >= Key.D0 && key <= Key.D9)
            return ((char)('0' + (key - Key.D0))).ToString();

        // Numpad digits NumPad0..NumPad9 -> "Numpad0".."Numpad9".
        if (key >= Key.NumPad0 && key <= Key.NumPad9)
            return "Numpad" + (int)(key - Key.NumPad0);

        // Fall back to a raw VK byte when the platform can resolve one.
        var vk = TryResolveVirtualKey(key);
        if (vk.HasValue && vk.Value is > 0 and <= 0xFF)
            return "0x" + vk.Value.ToString("X2");

        return null;
    }

    // Best-effort resolution of the Win32 virtual-key code for an Avalonia Key.
    // Returns null when no sensible mapping exists. Kept conservative on purpose -
    // we only resolve keys ParseVKey would accept as 0xNN.
    private static int? TryResolveVirtualKey(Key key) => key switch
    {
        Key.Space => 0x20,
        Key.Enter => 0x0D,
        Key.Tab => 0x09,
        Key.Back => 0x08,
        Key.Insert => 0x2D,
        Key.Delete => 0x2E,
        Key.Home => 0x24,
        Key.End => 0x23,
        Key.PageUp => 0x21,
        Key.PageDown => 0x22,
        Key.Up => 0x26,
        Key.Down => 0x28,
        Key.Left => 0x25,
        Key.Right => 0x27,
        Key.OemTilde => 0xC0,   // `~ key, a common hotkey choice
        Key.OemMinus => 0xBD,
        Key.OemPlus => 0xBB,
        _ => null
    };
}
