namespace Pulsar.ViewModels;

// Plain-language FAQ for non-technical users (content is static in HelpView.axaml).
// The bug-report / diagnostics flow lives on its own page now (BugReportViewModel).
public partial class HelpViewModel : ViewModelBase
{
    // Shell ref no longer needed (pure FAQ); ctor keeps the param for the uniform
    // `new HelpViewModel(this)` construction pattern.
    public HelpViewModel(MainWindowViewModel shell) { _ = shell; }

    public override string Title => "Help";
    public override string Glyph => char.ConvertFromUtf32(0xE897); // Help
}
