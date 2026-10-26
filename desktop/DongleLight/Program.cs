using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace DongleLight;

internal static class Program
{
    internal static readonly string Identity = WindowsIdentity.GetCurrent().User!.Value + "-" + Process.GetCurrentProcess().SessionId;
    internal static readonly string PipeName = "DongleLight-" + Identity;
    internal static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, PropertyNameCaseInsensitive = true };

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length == 3 && args[0] == "--hook")
        {
            if (Array.IndexOf(new[] { "copilot", "claude" }, args[1]) < 0 ||
                Array.IndexOf(new[] { "idle", "working", "done", "error", "needs-you" }, args[2]) < 0) return 0;
            RelayAsync(new { action = "signal", provider = args[1], state = args[2] }, true).GetAwaiter().GetResult();
            return 0; // Status lights must never block or reject an agent action.
        }
        using var singleton = new Mutex(true, @"Local\" + PipeName, out bool first);
        if (!first)
        {
            if (Array.IndexOf(args, "--background") < 0)
                RelayAsync(new { action = "show" }, false).GetAwaiter().GetResult();
            return 0;
        }
        WinRT.ComWrappersSupport.InitializeComWrappers();
        Application.Start(initialization =>
        {
            SynchronizationContext.SetSynchronizationContext(
                new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread()));
            _ = new App(Array.IndexOf(args, "--background") >= 0);
        });
        return 0;
    }

    internal static async Task RelayAsync(object message, bool launch)
    {
        try
        {
            using var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.Out, PipeOptions.Asynchronous);
            try { await pipe.ConnectAsync(500); }
            catch (TimeoutException) when (launch)
            {
                Process.Start(new ProcessStartInfo(Environment.ProcessPath!, "--background") { UseShellExecute = false });
                await pipe.ConnectAsync(6000);
            }
            using var writer = new StreamWriter(pipe) { AutoFlush = true };
            await writer.WriteLineAsync(JsonSerializer.Serialize(message, Json));
        }
        catch (Exception error) when (error is IOException or TimeoutException or System.ComponentModel.Win32Exception)
        {
            AppSettings.Log("Hook relay: " + error.Message);
        }
    }
}
