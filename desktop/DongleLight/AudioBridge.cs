using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace DongleLight;

internal sealed class AudioBridge : IDisposable
{
    private Process? process;
    private readonly SemaphoreSlim writes = new(1);
    private int nextId;
    private bool disposed;
    internal event Action<JsonElement>? Message;
    internal event Action<string>? Error;
    internal bool Running => process is { HasExited: false };
    internal int ConnectionId { get; private set; }
    internal DateTimeOffset LastSnapshot { get; private set; }

    private static ProcessStartInfo StartInfo(AppSettings settings, string arguments = "")
    {
        var start = new ProcessStartInfo(settings.Python)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true
        };
        start.ArgumentList.Add("-u");
        start.ArgumentList.Add(Path.Combine(AppContext.BaseDirectory, "host", "ks_audio.py"));
        if (arguments.Length > 0) start.ArgumentList.Add(arguments);
        return start;
    }

    internal static async Task<JsonElement> DevicesAsync(AppSettings settings)
    {
        using var child = Process.Start(StartInfo(settings, "--devices")) ?? throw new IOException("Could not start Python.");
        var stdout = child.StandardOutput.ReadToEndAsync();
        var stderr = child.StandardError.ReadToEndAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        try { await child.WaitForExitAsync(timeout.Token); }
        catch { child.Kill(true); throw; }
        string text = await stdout;
        if (child.ExitCode != 0) throw new IOException(await stderr);
        return JsonDocument.Parse(text).RootElement.Clone();
    }

    internal async Task StartAsync(AppSettings settings)
    {
        Stop();
        JsonElement devices = await DevicesAsync(settings);
        int Resolve(string direction, string name)
        {
            var matches = devices.GetProperty(direction).EnumerateArray()
                .Where(x => x.GetProperty("name").GetString() == name).ToArray();
            if (matches.Length != 1)
                throw new IOException($"Audio device '{name}' is missing or ambiguous. Select it again in Audio channels.");
            return matches[0].GetProperty("id").GetInt32();
        }
        settings.Output = Resolve("outputs", settings.OutputName);
        settings.Input = Resolve("inputs", settings.InputName);
        LastSnapshot = default;
        process = Process.Start(StartInfo(settings)) ?? throw new IOException("Could not start Python.");
        Process child = process;
        await child.StandardInput.WriteLineAsync(JsonSerializer.Serialize(new
        {
            name = settings.Name, channel = settings.Channel, input = settings.Input, output = settings.Output
        }));
        await child.StandardInput.FlushAsync();
        _ = ReadAsync(child);
        _ = ReadErrorsAsync(child);
        await SendAsync(new { action = "connect" });
    }

    private async Task ReadAsync(Process child)
    {
        try
        {
            while (await child.StandardOutput.ReadLineAsync() is string line)
            {
                try
                {
                    using var doc = JsonDocument.Parse(line);
                    var message = doc.RootElement.Clone();
                    if (message.GetProperty("kind").GetString() == "snapshot") LastSnapshot = DateTimeOffset.UtcNow;
                    Message?.Invoke(message);
                }
                catch (JsonException) { Error?.Invoke("Transport returned invalid JSON."); }
            }
            if (!disposed && ReferenceEquals(child, process))
                Error?.Invoke("Audio worker stopped. Reconnecting automatically.");
        }
        catch (Exception error) when (error is IOException or ObjectDisposedException)
        {
            if (!disposed && ReferenceEquals(child, process)) Error?.Invoke(error.Message);
        }
    }

    private async Task ReadErrorsAsync(Process child)
    {
        try
        {
            string text = await child.StandardError.ReadToEndAsync();
            if (!string.IsNullOrWhiteSpace(text)) Error?.Invoke(text.Trim());
        }
        catch (ObjectDisposedException) { }
    }

    internal async Task SendAsync(object request)
    {
        await writes.WaitAsync();
        try
        {
            if (process is not { HasExited: false }) throw new IOException("Connect audio devices first.");
            var data = JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(JsonSerializer.Serialize(request))!;
            data["id"] = JsonSerializer.SerializeToElement(Interlocked.Increment(ref nextId));
            await process.StandardInput.WriteLineAsync(JsonSerializer.Serialize(data));
            await process.StandardInput.FlushAsync();
        }
        finally { writes.Release(); }
    }

    internal void Stop()
    {
        ConnectionId++;
        Process? old = process;
        process = null;
        if (old is null) return;
        try
        {
            old.StandardInput.Close();
            if (!old.WaitForExit(500)) old.Kill(true);
        }
        catch (InvalidOperationException) { }
        old.Dispose();
    }

    public void Dispose() { disposed = true; Stop(); }
}
