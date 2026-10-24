using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Pulsar.ViewModels;

namespace Pulsar.Views;

public partial class VoiceChatView : UserControl
{
    public VoiceChatView()
    {
        InitializeComponent();
        // Tunnel + handledEventsToo so we grab the key/mouse before the focused
        // bind button consumes Space/Enter/click. Mirrors KeybindsView.
        AddHandler(KeyDownEvent, OnKeyDownTunnel, RoutingStrategies.Tunnel, handledEventsToo: true);
        AddHandler(PointerPressedEvent, OnPointerPressed, RoutingStrategies.Tunnel, handledEventsToo: true);
        // Live mic meter runs only while this tab is on screen.
        Loaded += (_, _) => (DataContext as VoiceChatViewModel)?.StartMeter();
        Unloaded += (_, _) => (DataContext as VoiceChatViewModel)?.StopMeter();
    }

    private void OnKeyDownTunnel(object? sender, KeyEventArgs e)
    {
        if (DataContext is not VoiceChatViewModel vm || !vm.IsCapturing) return;
        if (vm.HandleKey(e.Key)) e.Handled = true;
    }

    private void OnPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (DataContext is not VoiceChatViewModel vm || !vm.IsCapturing) return;
        // Only side/middle buttons bind; left/right stay usable for the UI.
        int vk = e.GetCurrentPoint(this).Properties.PointerUpdateKind switch
        {
            PointerUpdateKind.XButton1Pressed => 0x05,
            PointerUpdateKind.XButton2Pressed => 0x06,
            PointerUpdateKind.MiddleButtonPressed => 0x04,
            _ => 0
        };
        if (vk != 0 && vm.HandleMouse(vk)) e.Handled = true;
    }
}
