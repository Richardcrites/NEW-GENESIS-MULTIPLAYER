using System.Collections.Generic;
using System.Linq;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using Pulsar.ViewModels;

namespace Pulsar.Views;

public partial class PlayView : UserControl
{
    private PlayViewModel? _vm;

    public PlayView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
    }

    private void OnDataContextChanged(object? sender, System.EventArgs e)
    {
        if (_vm != null) _vm.BrowseRequested -= BrowseForStarfield;
        _vm = DataContext as PlayViewModel;
        if (_vm != null) _vm.BrowseRequested += BrowseForStarfield;
    }

    private async void BrowseForStarfield()
    {
        var top = TopLevel.GetTopLevel(this);
        if (top == null || _vm == null) return;

        var files = await top.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Select Starfield / SFSE loader",
            AllowMultiple = false,
            FileTypeFilter = new List<FilePickerFileType>
            {
                new("Executables") { Patterns = new[] { "*.exe" } }
            }
        });

        var picked = files.FirstOrDefault();
        if (picked != null)
        {
            _vm.StarfieldPath = picked.TryGetLocalPath() ?? picked.Path.LocalPath;
            _vm.Shell.SaveConfig();
        }
    }

    private async void OnCopyCode(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not PlayViewModel vm) return;
        var code = vm.Shell.HostInviteCode;
        if (string.IsNullOrEmpty(code)) return;

        var clipboard = TopLevel.GetTopLevel(this)?.Clipboard;
        if (clipboard != null)
        {
            await clipboard.SetTextAsync(code);
            vm.Shell.Log("Invite code copied.");
        }
    }

    private async void OnCopyInvite(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not PlayViewModel vm) return;
        var invite = vm.InviteAddress;
        if (string.IsNullOrEmpty(invite)) return;

        var clipboard = TopLevel.GetTopLevel(this)?.Clipboard;
        if (clipboard != null)
        {
            await clipboard.SetTextAsync(invite);
            vm.Shell.Log("Direct invite copied.");
        }
    }
}
