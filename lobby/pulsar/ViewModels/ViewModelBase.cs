using CommunityToolkit.Mvvm.ComponentModel;

namespace Pulsar.ViewModels;

// Base for all view-models. Each page VM also carries display metadata for the nav.
public abstract partial class ViewModelBase : ObservableObject
{
    // Title shown in the nav and as the page header. Set by derived pages.
    public virtual string Title => "";

    // Glyph (Segoe Fluent Icons / Segoe MDL2) for the nav entry.
    public virtual string Glyph => "";
}
