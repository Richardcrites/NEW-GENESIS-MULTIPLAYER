using Pulsar.Services;

namespace Pulsar.ViewModels;

// A row in the open-lobbies list (Host/Join page) and (Phase 2) the Servers list.
public sealed class LobbyRowViewModel
{
    public LobbyRowViewModel(LobbyInfo info)
    {
        LobbyId = info.LobbyId;
        Name = string.IsNullOrWhiteSpace(info.Name) ? $"Lobby {info.LobbyId}" : info.Name;
        Status = info.Status;
        Players = $"{info.MemberCount}/{info.MaxPlayers}";
        HasPassword = info.HasPassword;
        ModCount = info.ModCount;
        ModList = info.ModList ?? "";
    }

    public ulong LobbyId { get; }
    public string Name { get; }
    public string Status { get; }
    public string Players { get; }
    public bool HasPassword { get; }
    public uint ModCount { get; }
    public string ModList { get; }

    // Lock glyph when password-protected, blank otherwise.
    public string LockGlyph => HasPassword ? char.ConvertFromUtf32(0xE72E) : "";

    // Mod badge: shown only when the host has user mods. Hover shows the list.
    public bool HasMods => ModCount > 0;
    public string ModBadge => ModCount > 0 ? $"Mods {ModCount}" : "";
    public string ModTooltip => ModCount > 0
        ? "Host has " + ModCount + " mod(s):\n" + ModList.Replace("|", "\n")
        : "";
}
