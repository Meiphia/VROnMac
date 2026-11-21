using Avalonia;
using ReactiveUI.Avalonia;

namespace VRCFaceTracking;

internal class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        // Install only this bundled local module; leave other user modules intact.
        var module = Path.Combine(AppContext.BaseDirectory, "BundledModules", "VRConMac.Tracking.dll");
        if (File.Exists(module))
        {
            Directory.CreateDirectory(Core.Utils.CustomLibsDirectory);
            File.Copy(module, Path.Combine(Core.Utils.CustomLibsDirectory, "VRConMac.Tracking.dll"), true);
        }
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp()
    {
        var builder = AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .LogToTrace()
            .UseReactiveUI();

        /*if (OperatingSystem.IsLinux() && Environment.GetEnvironmentVariable("WAYLAND_DISPLAY") is not null)
        {
            builder = builder.UseWayland();
        }*/

        return builder;
    }
}