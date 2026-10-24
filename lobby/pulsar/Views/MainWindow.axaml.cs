using FluentAvalonia.UI.Windowing;

namespace Pulsar.Views;

// AppWindow gives the modern Mica/acrylic titlebar integration on Windows; it
// degrades gracefully to a normal window elsewhere.
public partial class MainWindow : AppWindow
{
    public MainWindow()
    {
        InitializeComponent();
        TitleBar.ExtendsContentIntoTitleBar = false;
    }
}
