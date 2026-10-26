using System.Text.Json;
using System.Text.Json.Nodes;
using DongleLight;

int checks = 0;
void Check(bool condition, string message)
{
    if (!condition) throw new Exception(message);
    checks++;
    Console.WriteLine("PASS " + message);
}

const string executable = @"C:\test app\O'Brien\DongleLight.exe";
var original = JsonNode.Parse("""
{
  "permissions": { "allow": ["Read"] },
  "hooks": {
    "Stop": [
      { "hooks": [ { "type": "command", "command": "existing-user-hook" } ] }
    ],
    "OtherEvent": [ { "hooks": [ { "command": "untouched" } ] } ]
  }
}
""")!.AsObject();
var result = HookInstaller.MergeClaude(original, executable);
Check(result["permissions"]!["allow"]![0]!.GetValue<string>() == "Read", "unrelated settings preserved");
Check(result["hooks"]!["OtherEvent"]![0]!["hooks"]![0]!["command"]!.GetValue<string>() == "untouched", "unrelated event preserved");
var stop = result["hooks"]!["Stop"]!.AsArray();
Check(stop.Count == 2 && stop[0]!["hooks"]![0]!["command"]!.GetValue<string>() == "existing-user-hook", "existing event handler preserved");
string command = stop[1]!["hooks"]![0]!["command"]!.GetValue<string>();
Check(command == "& 'C:\\test app\\O''Brien\\DongleLight.exe' --hook claude done", "PowerShell path quoted safely");
HookInstaller.MergeClaude(result, executable);
Check(result["hooks"]!["Stop"]!.AsArray().Count == 2, "hook update idempotent");
Check(result["hooks"]!["Notification"]![0]!["matcher"]!.GetValue<string>() == "permission_prompt|idle_prompt", "notification matcher retained");
Check(result["hooks"]!["Notification"]![0]!["hooks"]![0]!["async"]!.GetValue<bool>(), "Claude hooks run asynchronously");
Check(result["hooks"]!["Notification"]![0]!["hooks"]![0]!["shell"]!.GetValue<string>() == "powershell", "Claude uses explicit Windows shell");
try
{
    HookInstaller.MergeClaude(new JsonObject { ["hooks"] = "not-an-object" }, executable);
    throw new Exception("Malformed configuration accepted");
}
catch (JsonException) { checks++; Console.WriteLine("PASS malformed hook root rejected"); }
try
{
    HookInstaller.MergeClaude(new JsonObject { ["hooks"] = new JsonObject { ["Stop"] = "not-an-array" } }, executable);
    throw new Exception("Malformed event accepted");
}
catch (JsonException) { checks++; Console.WriteLine("PASS malformed event rejected"); }
var copilot = HookInstaller.CopilotConfig(executable);
Check(copilot["version"]!.GetValue<int>() == 1, "Copilot configuration version");
Check(copilot["hooks"]!.AsObject().Count == 6, "all supported Copilot lifecycle mappings generated");
Check(copilot["hooks"]!["notification"]![0]!["matcher"]!.GetValue<string>() == "elicitation_dialog", "Copilot user questions light needs-you without background completion noise");
Check(copilot["hooks"]!["permissionRequest"]![0]!["exec"]!.GetValue<string>() == executable, "Copilot needs-you hook uses direct executable");
Check(copilot["hooks"]!["agentStop"]![0]!["args"]![2]!.GetValue<string>() == "done", "Copilot stop mapped to done");
Check(copilot["hooks"]!["errorOccurred"]![0]!["args"]![2]!.GetValue<string>() == "error", "Copilot errors mapped correctly");
Check(copilot["hooks"]!["sessionEnd"]![0]!["args"]![2]!.GetValue<string>() == "idle", "Copilot session end clears agent");
Console.WriteLine($"{checks} checks passed.");
