using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace Pulsar.ViewModels;

// One row in the Setup install-check list. Status drives the colored dot in the
// view; the Fix button is shown only when CanFix is true and an auto-fix exists.
public enum CheckStatus
{
    Ok,
    Warn,
    Fail
}

public partial class SetupCheckViewModel : ObservableObject
{
    private readonly SetupViewModel _owner;

    public SetupCheckViewModel(SetupViewModel owner, string name)
    {
        _owner = owner;
        _name = name;
    }

    [ObservableProperty] private string _name;
    [ObservableProperty] private string _detail = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsOk))]
    [NotifyPropertyChangedFor(nameof(IsWarn))]
    [NotifyPropertyChangedFor(nameof(IsFail))]
    [NotifyPropertyChangedFor(nameof(StatusGlyph))]
    [NotifyPropertyChangedFor(nameof(ShowLink))]
    private CheckStatus _status = CheckStatus.Warn;

    // Optional "Get it" download link, shown only when this row is failing (e.g. SFSE or
    // Address Library missing - we link the official source rather than bundling them).
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasLink))]
    [NotifyPropertyChangedFor(nameof(ShowLink))]
    private string _linkUrl = "";
    [ObservableProperty] private string _linkText = "Get it";

    public bool HasLink => !string.IsNullOrEmpty(LinkUrl);
    public bool ShowLink => HasLink && IsFail;

    // CanFix = there is an auto-fix for this row AND it is not currently satisfied
    // AND prerequisites (the Starfield path) are met. The view binds the Fix button
    // visibility to this.
    [ObservableProperty] private bool _canFix;

    public bool IsOk => Status == CheckStatus.Ok;
    public bool IsWarn => Status == CheckStatus.Warn;
    public bool IsFail => Status == CheckStatus.Fail;

    // Status icon (Segoe Fluent): check / warning / error.
    public string StatusGlyph => Status switch
    {
        CheckStatus.Ok => char.ConvertFromUtf32(0xE73E),   // CheckMark
        CheckStatus.Warn => char.ConvertFromUtf32(0xE7BA), // Warning
        _ => char.ConvertFromUtf32(0xEA39)                 // ErrorBadge
    };

    [RelayCommand]
    private void Fix() => _owner.FixOne(this);

    [RelayCommand]
    private void OpenLink()
    {
        if (string.IsNullOrEmpty(LinkUrl)) return;
        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = LinkUrl,
                UseShellExecute = true
            });
        }
        catch { /* no browser / blocked - ignore */ }
    }
}
