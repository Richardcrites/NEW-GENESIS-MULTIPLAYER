using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Pulsar.Services;
using Pulsar.ViewModels;
using Pulsar.Views;

namespace Pulsar;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var config = AppConfig.Load();
            var bridge = new DaemonBridge(config);
            var vm = new MainWindowViewModel(config, bridge);

            var window = new MainWindow { DataContext = vm };

            // Kill the daemon when the window closes (Kill(true) in the bridge).
            window.Closing += (_, _) =>
            {
                vm.SaveConfig();
                vm.Shutdown();
                bridge.Shutdown();
            };

            desktop.MainWindow = window;
            vm.Start();
        }

        base.OnFrameworkInitializationCompleted();
    }
}
