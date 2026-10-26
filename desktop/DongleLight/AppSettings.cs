using System;
using System.IO;
using System.Text.Json;
using Microsoft.Win32;

namespace DongleLight;

internal sealed class AppSettings
{
    internal static readonly string DirectoryPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DongleLight");
    internal static readonly string FilePath = Path.Combine(DirectoryPath, "settings.json");
    public string Name { get; set; } = Environment.MachineName.Length > 8 ? Environment.MachineName[..8] : Environment.MachineName;
    public int Channel { get; set; }
    public int Input { get; set; } = -1;
    public int Output { get; set; } = -1;
    public string InputName { get; set; } = "Windows default device";
    public string OutputName { get; set; } = "Windows default device";
    public string Python { get; set; } = "python.exe";
    public string Theme { get; set; } = "Default";
    public bool ConnectAtLaunch { get; set; }

    internal static AppSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath), Program.Json) ?? new();
        }
        catch (Exception error) when (error is IOException or JsonException)
        {
            Log("Settings could not be loaded: " + error.Message);
        }
        return new();
    }

    internal void Save()
    {
        Directory.CreateDirectory(DirectoryPath);
        File.WriteAllText(FilePath + ".tmp", JsonSerializer.Serialize(this, new JsonSerializerOptions(Program.Json) { WriteIndented = true }));
        File.Move(FilePath + ".tmp", FilePath, true);
    }

    internal static bool StartupEnabled
    {
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
            return key?.GetValue("DongleLight") is string;
        }
        set
        {
            using var key = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
            if (value) key.SetValue("DongleLight", "\"" + Environment.ProcessPath + "\" --background");
            else key.DeleteValue("DongleLight", false);
        }
    }

    internal static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(DirectoryPath);
            string path = Path.Combine(DirectoryPath, "diagnostics.log");
            if (File.Exists(path) && new FileInfo(path).Length > 1_000_000)
                File.Move(path, path + ".previous", true);
            File.AppendAllText(path, DateTimeOffset.Now.ToString("O") + " " + message + Environment.NewLine);
        }
        catch (IOException) { }
    }
}
