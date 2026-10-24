using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Pulsar.ViewModels;

namespace Pulsar.Views;

public partial class KeybindsView : UserControl
{
    public KeybindsView()
    {
        InitializeComponent();
        // Tunnel so we intercept the key before any focused button/control eats it
        // (e.g. Space/Enter activating the rebind button itself).
        AddHandler(KeyDownEvent, OnKeyDownTunnel, RoutingStrategies.Tunnel, handledEventsToo: true);
    }

    private void OnKeyDownTunnel(object? sender, KeyEventArgs e)
    {
        if (DataContext is not KeybindsViewModel vm || !vm.IsCapturing) return;
        // PhysicalKey-agnostic: route the logical key to the VM mapper.
        if (vm.HandleKey(e.Key))
            e.Handled = true;
    }

    protected override void OnDataContextChanged(System.EventArgs e)
    {
        base.OnDataContextChanged(e);
        // Re-read PULSAR.ini (and re-resolve the path) whenever this page is shown,
        // in case the Starfield path changed in Settings.
        if (DataContext is KeybindsViewModel vm) vm.RaiseStateChanged();
    }
}
