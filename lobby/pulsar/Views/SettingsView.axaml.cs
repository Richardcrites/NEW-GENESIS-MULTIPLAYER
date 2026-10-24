using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using Pulsar.ViewModels;

namespace Pulsar.Views;

public partial class SettingsView : UserControl
{
    private SettingsViewModel? _vm;

    public SettingsView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
    }

    private void OnDataContextChanged(object? sender, EventArgs e)
    {
        if (_vm != null)
        {
            _vm.ExportRequested -= ExportCharacter;
            _vm.ImportRequested -= ImportCharacter;
        }
        _vm = DataContext as SettingsViewModel;
        if (_vm != null)
        {
            _vm.ExportRequested += ExportCharacter;
            _vm.ImportRequested += ImportCharacter;
        }
    }

    private static readonly FilePickerFileType CharFileType =
        new("PULSAR character") { Patterns = new[] { "*.pulsarchar" } };

    private async void ExportCharacter()
    {
        var top = TopLevel.GetTopLevel(this);
        if (top == null || _vm == null) return;

        var file = await top.StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title = "Export character",
            SuggestedFileName = "my-character.pulsarchar",
            DefaultExtension = "pulsarchar",
            FileTypeChoices = new List<FilePickerFileType> { CharFileType },
        });
        if (file == null) return;

        var path = file.TryGetLocalPath() ?? file.Path.LocalPath;
        bool ok = _vm.Shell.ExportCharacter(path, out var error);
        _vm.BackupStatus = ok ? "Exported." : ("Export failed: " + error);
    }

    private async void ImportCharacter()
    {
        var top = TopLevel.GetTopLevel(this);
        if (top == null || _vm == null) return;

        var files = await top.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Import character",
            AllowMultiple = false,
            FileTypeFilter = new List<FilePickerFileType> { CharFileType },
        });
        var picked = files.FirstOrDefault();
        if (picked == null) return;

        var path = picked.TryGetLocalPath() ?? picked.Path.LocalPath;
        bool ok = _vm.Shell.ImportCharacter(path, out var error);
        _vm.BackupStatus = ok ? "Imported - it will upload on the next sync." : ("Import failed: " + error);
    }
}
