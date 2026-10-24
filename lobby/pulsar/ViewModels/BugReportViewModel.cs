using System;
using System.Diagnostics;
using System.Reflection;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Pulsar.Services;

namespace Pulsar.ViewModels;

// Dedicated "Report a bug" page (split out of Help, which is now pure FAQ). Two steps:
// (1) collect the logs + crash metadata into one ZIP on the Desktop, (2) open the issue
// tracker so the user can attach it. The tracker URL is derived from the mod repo set in
// Settings (gameplay bugs land there) so there is no extra setting to keep in sync.
public partial class BugReportViewModel : ViewModelBase
{
    private readonly MainWindowViewModel _shell;
    private string _lastZip = "";

    public BugReportViewModel(MainWindowViewModel shell) { _shell = shell; }

    public override string Title => "Report a bug";
    public override string Glyph => char.ConvertFromUtf32(0xED15); // Feedback

    [ObservableProperty] private bool _diagBusy;
    [ObservableProperty] private string _diagStatus = "";
    [ObservableProperty] private bool _hasDiagFile;

    [RelayCommand]
    private async Task SendDiagnostics()
    {
        if (DiagBusy) return;
        DiagBusy = true;
        DiagStatus = "Collecting logs ...";
        HasDiagFile = false;

        var gamePath = _shell.Config.StarfieldPath;
        var toolVersion = Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "0.0.0";

        var reportRepo = _shell.Config.ModRepo;
        var result = await Task.Run(() => DiagnosticsCollector.Collect(gamePath, toolVersion, reportRepo));

        _lastZip = result.ZipPath;
        HasDiagFile = result.Ok && !string.IsNullOrEmpty(result.ZipPath);
        DiagStatus = result.Summary;
        DiagBusy = false;
    }

    [RelayCommand]
    private void ShowDiagnosticsFile()
    {
        if (string.IsNullOrEmpty(_lastZip)) return;
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = "explorer.exe",
                Arguments = $"/select,\"{_lastZip}\"",
                UseShellExecute = true
            });
        }
        catch (Exception ex) { DiagStatus = "Could not open the folder: " + ex.Message; }
    }

    [RelayCommand]
    private void OpenIssueTracker()
    {
        var url = DiagnosticsCollector.IssueTrackerUrl(_shell.Config.ModRepo);
        try
        {
            Process.Start(new ProcessStartInfo { FileName = url, UseShellExecute = true });
        }
        catch { /* best-effort */ }
    }
}
