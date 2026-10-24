using Avalonia.Controls;
using Pulsar.ViewModels;

namespace Pulsar.Views;

public partial class SetupView : UserControl
{
    public SetupView()
    {
        InitializeComponent();
        // Run the install checks whenever the page is shown (page open / nav switch).
        DataContextChanged += (_, _) => (DataContext as SetupViewModel)?.RunChecks();
        AttachedToVisualTree += (_, _) => (DataContext as SetupViewModel)?.RunChecks();
    }
}
