using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace DongleLight;

internal static class HookInstaller
{
    private static readonly (string Event, string State, string? Matcher)[] ClaudeEvents =
    [
        ("Notification", "needs-you", "permission_prompt|idle_prompt"),
        ("UserPromptSubmit", "working", null), ("Stop", "done", null),
        ("StopFailure", "error", null), ("SessionEnd", "idle", null)
    ];
    private static readonly (string Event, string State, string? Matcher)[] CopilotEvents =
    [
        ("userPromptSubmitted", "working", null), ("agentStop", "done", null),
        ("errorOccurred", "error", null), ("sessionEnd", "idle", null),
        ("permissionRequest", "needs-you", null),
        ("notification", "needs-you", "elicitation_dialog")
    ];

    internal static string PathFor(string provider)
    {
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (provider == "claude") return Path.Combine(home, ".claude", "settings.json");
        string copilot = Environment.GetEnvironmentVariable("COPILOT_HOME") ?? Path.Combine(home, ".copilot");
        return Path.Combine(copilot, "hooks", "donglelight.json");
    }

    internal static JsonObject MergeClaude(JsonObject root, string executable)
    {
        if (root["hooks"] is not null and not JsonObject) throw new JsonException("The hooks field must be an object.");
        var hooks = root["hooks"] as JsonObject ?? new JsonObject();
        if (root["hooks"] is null) root["hooks"] = hooks;
        foreach (var (evt, state, matcher) in ClaudeEvents)
        {
            if (hooks[evt] is not null and not JsonArray) throw new JsonException(evt + " must be an array.");
            var groups = hooks[evt] as JsonArray ?? new JsonArray();
            if (hooks[evt] is null) hooks[evt] = groups;
            // Only replace our entries. Never remove ks.py or another user's hooks.
            foreach (var old in groups.ToArray())
            {
                if (old?["hooks"] is not JsonArray handlers) continue;
                foreach (var handler in handlers.ToArray())
                    if (handler?["command"]?.GetValue<string>().Contains("--hook claude ", StringComparison.Ordinal) == true)
                        handlers.Remove(handler);
                if (handlers.Count == 0) groups.Remove(old);
            }
            var group = new JsonObject
            {
                ["hooks"] = new JsonArray(new JsonObject
                {
                    ["type"] = "command", ["shell"] = "powershell", ["async"] = true, ["timeout"] = 10,
                    ["command"] = "& '" + executable.Replace("'", "''") + "' --hook claude " + state
                })
            };
            if (matcher is not null) group["matcher"] = matcher;
            groups.Add(group);
        }
        return root;
    }

    internal static JsonObject CopilotConfig(string executable)
    {
        var hooks = new JsonObject();
        foreach (var (evt, state, matcher) in CopilotEvents)
        {
            var hook = new JsonObject
            {
                ["type"] = "command", ["exec"] = executable,
                ["args"] = new JsonArray("--hook", "copilot", state), ["timeoutSec"] = 10
            };
            if (matcher is not null) hook["matcher"] = matcher;
            hooks[evt] = new JsonArray(hook);
        }
        return new JsonObject { ["version"] = 1, ["hooks"] = hooks };
    }

    internal static string Install(string provider)
    {
        string path = PathFor(provider);
        JsonObject root = File.Exists(path)
            ? JsonNode.Parse(File.ReadAllText(path))?.AsObject() ?? throw new JsonException("Empty hook configuration.")
            : new JsonObject();
        if (provider == "claude") root = MergeClaude(root, Environment.ProcessPath!);
        else
        {
            if (File.Exists(path) && root["hooks"] is not JsonObject)
                throw new JsonException("Existing donglelight.json does not have the expected structure.");
            var generated = CopilotConfig(Environment.ProcessPath!);
            var hooks = root["hooks"] as JsonObject ?? new JsonObject();
            if (root["hooks"] is null) root["hooks"] = hooks;
            root["version"] = 1;
            foreach (var entry in generated["hooks"]!.AsObject())
            {
                if (hooks[entry.Key] is not null and not JsonArray)
                    throw new JsonException(entry.Key + " must be an array.");
                var list = hooks[entry.Key] as JsonArray ?? new JsonArray();
                if (hooks[entry.Key] is null) hooks[entry.Key] = list;
                foreach (var item in list.ToArray())
                    if (item?["args"] is JsonArray args && args.Count >= 2 &&
                        args[0]?.GetValue<string>() == "--hook" && args[1]?.GetValue<string>() == "copilot")
                        list.Remove(item);
                foreach (var item in entry.Value!.AsArray()) list.Add(item!.DeepClone());
            }
        }
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        if (File.Exists(path) && !File.Exists(path + ".donglelight-backup"))
            File.Copy(path, path + ".donglelight-backup");
        string temporary = path + ".donglelight-tmp";
        File.WriteAllText(temporary, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + Environment.NewLine);
        File.Move(temporary, path, true);
        return path;
    }
}
