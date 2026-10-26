using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Forms = System.Windows.Forms;

namespace DongleLight;

public partial class App : Application
{
    private readonly bool background;
    private readonly CancellationTokenSource shutdown = new();
    private Forms.NotifyIcon? tray;
    private MainWindow? window;
    private AudioBridge? bridge;
    private AppSettings settings = null!;
    private DispatcherQueue dispatcher = null!;
    private readonly SemaphoreSlim connection = new(1);
    private DispatcherQueueTimer? retry;
    private DateTimeOffset started;
    private bool exiting;

    internal App(bool background)
    {
        this.background = background;
        InitializeComponent();
        UnhandledException += (_, e) => AppSettings.Log(e.Exception.ToString());
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        dispatcher = DispatcherQueue.GetForCurrentThread();
        settings = AppSettings.Load();
        bridge = new AudioBridge();
        window = new MainWindow(settings, bridge, ConnectAsync, Quit);
        bridge.Message += message => dispatcher.TryEnqueue(() => window.Receive(message));
        bridge.Error += error =>
        {
            AppSettings.Log(error);
            dispatcher.TryEnqueue(() => window.ShowError(error));
        };
        window.AppWindow.Closing += (_, e) =>
        {
            if (exiting) return;
            e.Cancel = true;
            window.AppWindow.Hide();
        };
        tray = new Forms.NotifyIcon
        {
            Text = "dongleLight - audio control plane",
            Icon = System.Drawing.SystemIcons.Application,
            Visible = true,
            ContextMenuStrip = new Forms.ContextMenuStrip()
        };
        tray.ContextMenuStrip.Items.Add("Open dongleLight", null, (_, _) => dispatcher.TryEnqueue(Show));
        tray.ContextMenuStrip.Items.Add("Acknowledge all lights", null, (_, _) => dispatcher.TryEnqueue(async () =>
        {
            try { await bridge.SendAsync(new { action = "ack", lamp = 0 }); }
            catch (Exception error) { window.ShowError(error.Message); }
        }));
        tray.ContextMenuStrip.Items.Add(new Forms.ToolStripSeparator());
        tray.ContextMenuStrip.Items.Add("Exit", null, (_, _) => dispatcher.TryEnqueue(Quit));
        tray.DoubleClick += (_, _) => dispatcher.TryEnqueue(Show);
        _ = ListenAsync();
        retry = dispatcher.CreateTimer();
        retry.Interval = TimeSpan.FromSeconds(3);
        retry.Tick += async (_, _) =>
        {
            window.UpdateFreshness();
            if (settings.ConnectAtLaunch && ((!bridge.Running && DateTimeOffset.UtcNow - started > TimeSpan.FromSeconds(5)) ||
                (DateTimeOffset.UtcNow - started > TimeSpan.FromSeconds(45) &&
                 DateTimeOffset.UtcNow - bridge.LastSnapshot > TimeSpan.FromSeconds(25))))
                await ConnectAsync();
        };
        retry.Start();
        if (!background) Show();
        if (settings.ConnectAtLaunch) _ = ConnectAsync();
    }

    private void Show()
    {
        window!.AppWindow.Show();
        window.Activate();
    }

    private async Task ConnectAsync()
    {
        if (!await connection.WaitAsync(0)) return;
        try
        {
            started = DateTimeOffset.UtcNow;
            window!.ShowNotice("Connecting audio", "Listening for the dongle return stream and reserving your channel.");
            await bridge!.StartAsync(settings);
        }
        catch (Exception error)
        {
            AppSettings.Log(error.Message);
            window!.ShowError(error.Message);
        }
        finally { connection.Release(); }
    }

    private async Task ListenAsync()
    {
        while (!shutdown.IsCancellationRequested)
        {
            try
            {
                using var server = new NamedPipeServerStream(Program.PipeName, PipeDirection.In, 4,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await server.WaitForConnectionAsync(shutdown.Token);
                using var reader = new StreamReader(server);
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(shutdown.Token);
                timeout.CancelAfter(TimeSpan.FromSeconds(2));
                string? line = await reader.ReadLineAsync(timeout.Token);
                if (line is null || line.Length > 1024) continue;
                using var doc = JsonDocument.Parse(line);
                JsonElement message = doc.RootElement.Clone();
                dispatcher.TryEnqueue(async () =>
                {
                    try
                    {
                        string? action = message.GetProperty("action").GetString();
                        if (action == "show") { Show(); return; }
                        if (action != "signal") return;
                        string? provider = message.GetProperty("provider").GetString();
                        string? state = message.GetProperty("state").GetString();
                        if (provider is not ("copilot" or "claude") ||
                            state is not ("idle" or "working" or "done" or "error" or "needs-you")) return;
                        if (!settings.ConnectAtLaunch)
                            throw new IOException("Hook received. Finish Audio channels setup and connect first.");
                        if (!bridge!.Running) await ConnectAsync();
                        await bridge.SendAsync(new { action = "signal", provider, state });
                        window!.AddActivity(provider + ": " + state + " queued");
                    }
                    catch (Exception error) { window!.ShowError(error.Message); }
                });
            }
            catch (OperationCanceledException) { }
            catch (Exception error) when (error is IOException or JsonException)
            {
                AppSettings.Log(error.Message);
            }
        }
    }

    private void Quit()
    {
        exiting = true;
        shutdown.Cancel();
        retry?.Stop();
        tray?.Dispose();
        bridge?.Dispose();
        window?.Close();
        Application.Current.Exit();
    }
}
