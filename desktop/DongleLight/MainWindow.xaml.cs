using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace DongleLight;

public sealed partial class MainWindow : Window
{
    private readonly AppSettings settings;
    private readonly AudioBridge bridge;
    private readonly Func<Task> connect;
    private readonly Action exit;
    private JsonElement? snapshot;
    private string page = "overview";
    private readonly List<string> activity = [];
    private StackPanel? liveLamps, liveSources, liveChannels, liveActivity;
    private TextBlock? freshness;
    private Button? addLamp;
    private TextBlock? pairingStatus;
    private ProgressRing? pairingProgress;
    private bool pairingRequested;
    private int pairingConnection;
    private ComboBox? ruleChannel, ruleProvider;
    private StackPanel? ruleRows;
    private readonly List<RuleEditor> editors = [];
    private bool rulesDirty;
    private static readonly string[] StateNames = ["idle", "working", "done", "error", "needs-you"];
    private static readonly string[] StateLabels = ["Idle", "Working", "Done", "Error", "Needs you"];

    internal MainWindow(AppSettings settings, AudioBridge bridge, Func<Task> connect, Action exit)
    {
        this.settings = settings; this.bridge = bridge; this.connect = connect; this.exit = exit;
        InitializeComponent();
        Title = "dongleLight";
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(TitleBar);
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1160, 820));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = 760;
            presenter.PreferredMinimumHeight = 600;
        }
        Root.RequestedTheme = Enum.TryParse<ElementTheme>(settings.Theme, out var theme) ? theme : ElementTheme.Default;
        MachineLabel.Text = settings.Name;
        Navigation.RegisterPropertyChangedCallback(NavigationView.IsPaneOpenProperty, (_, _) =>
            PaneIdentity.Visibility = Navigation.IsPaneOpen ? Visibility.Visible : Visibility.Collapsed);
        Render();
    }

    private static TextBlock Text(string value, string style = "BodyTextBlockStyle") =>
        new() { Text = value, Style = (Style)Application.Current.Resources[style], TextWrapping = TextWrapping.Wrap };

    private static StackPanel Column(double spacing = 12) => new() { Spacing = spacing };
    private static SolidColorBrush Brush(string key) => (SolidColorBrush)Application.Current.Resources[key];

    private static Border Surface(UIElement child) => new()
    {
        Background = Brush("CardBackgroundFillColorDefaultBrush"), CornerRadius = new CornerRadius(8),
        Padding = new Thickness(20), Child = child
    };

    private static Button Button(string label, Action action, bool primary = false)
    {
        var button = new Button { Content = label, VerticalAlignment = VerticalAlignment.Center };
        if (primary) button.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        button.Click += (_, _) => action();
        return button;
    }

    private async void Run(Func<Task> action)
    {
        try { await action(); }
        catch (Exception error) { ShowError(error.Message); }
    }

    private async Task SendCommandAsync(object request)
    {
        if (!bridge.Running && settings.ConnectAtLaunch) await connect();
        await bridge.SendAsync(request);
    }

    private StackPanel Section(string title, string? description = null)
    {
        var section = Column();
        section.Children.Add(Text(title, "SubtitleTextBlockStyle"));
        if (description is not null) section.Children.Add(Text(description));
        PageContent.Children.Add(section);
        return section;
    }

    private static ComboBox Choices(string header, IEnumerable<string> items, int selected = 0)
    {
        var box = new ComboBox { Header = header, HorizontalAlignment = HorizontalAlignment.Stretch, MinWidth = 120 };
        foreach (string item in items) box.Items.Add(item);
        box.SelectedIndex = selected;
        return box;
    }

    private void Navigate(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (PageContent is null || settings is null) return;
        string next = args.IsSettingsSelected ? "settings" : (args.SelectedItem as NavigationViewItem)?.Tag?.ToString() ?? "overview";
        if (page == "rules" && rulesDirty && next != "rules")
            AddActivity("Unsaved rule edits discarded when leaving Event rules.");
        page = next;
        Render();
    }

    private void Render()
    {
        PageContent.Children.Clear();
        liveLamps = liveSources = liveChannels = liveActivity = null;
        freshness = null;
        addLamp = null; pairingStatus = null; pairingProgress = null;
        switch (page)
        {
            case "audio": AudioPage(); break;
            case "rules": RulesPage(); break;
            case "integrations": IntegrationsPage(); break;
            case "settings": SettingsPage(); break;
            default: OverviewPage(); break;
        }
    }

    private void Heading(string title, string subtitle)
    {
        PageTitle.Text = title; PageSubtitle.Text = subtitle;
    }

    private void OverviewPage()
    {
        Heading("Shared lights", "One view of the dongle, every lamp, and every connected devbox.");
        var command = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        command.Children.Add(Button("Acknowledge all", () => Run(() => SendCommandAsync(new { action = "ack", lamp = 0 })), true));
        freshness = Text("No return stream received yet.", "CaptionTextBlockStyle");
        freshness.VerticalAlignment = VerticalAlignment.Center;
        command.Children.Add(freshness);
        PageContent.Children.Add(command);
        liveLamps = Column(8);
        var lamps = Section("Lamps", "Power on the lamp's controller nearby and close its phone app. Add lamp pairs the strongest nearby supported light without forgetting existing lamps.");
        var pairing = Column(8);
        var pairingActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        addLamp = Button("Add lamp", () => Run(PairLampAsync));
        pairingProgress = new ProgressRing { Width = 20, Height = 20, IsActive = false, Visibility = Visibility.Collapsed };
        AutomationProperties.SetName(pairingProgress, "Adding a lamp");
        pairingActions.Children.Add(addLamp); pairingActions.Children.Add(pairingProgress);
        pairing.Children.Add(pairingActions);
        pairingStatus = Text("", "CaptionTextBlockStyle"); pairing.Children.Add(pairingStatus);
        lamps.Children.Add(pairing); lamps.Children.Add(liveLamps);
        liveSources = Column(8);
        Section("Agent activity", "C = Copilot CLI, A = Claude Code. Labels include the devbox channel.").Children.Add(liveSources);
        var manual = Column();
        var manualColor = new ColorPicker { Color = ColorHelper.FromArgb(255, 0, 200, 180), IsAlphaEnabled = false, IsColorSliderVisible = true };
        var colorButton = new Button { Content = "Choose light color", Flyout = new Flyout { Content = manualColor } };
        var target = Choices("Lamp", ["All lamps", "Lamp 1", "Lamp 2", "Lamp 3", "Lamp 4"]);
        var brightness = new Slider { Header = "Brightness", Minimum = 0, Maximum = 100, Value = 75, StepFrequency = 1 };
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        actions.Children.Add(colorButton);
        actions.Children.Add(Button("Apply color", () => Run(() => SendCommandAsync(new
        {
            action = "rgb", color = Hex(manualColor.Color), brightness = (int)brightness.Value, lamp = target.SelectedIndex
        }))));
        actions.Children.Add(Button("Turn off", () => Run(() => SendCommandAsync(new { action = "off", lamp = target.SelectedIndex }))));
        manual.Children.Add(target); manual.Children.Add(brightness); manual.Children.Add(actions);
        Section("Manual light", "Alerts temporarily override this base color. Acknowledging restores it.").Children.Add(Surface(manual));
        liveActivity = Column(6);
        Section("This devbox", "Local diagnostics only. Prompts, tool arguments, and source code are never transmitted.").Children.Add(liveActivity);
        RefreshLive();
    }

    private void RefreshLive()
    {
        if (liveLamps is not null)
        {
            liveLamps.Children.Clear(); liveSources!.Children.Clear();
            if (snapshot is null)
            {
                var empty = Column();
                empty.Children.Add(Text("Your lights will appear here", "BodyStrongTextBlockStyle"));
                empty.Children.Add(Text("Connect both audio directions. The microphone return stream shares state with every devbox."));
                empty.Children.Add(Button("Set up audio", () =>
                {
                    Navigation.SelectedItem = Navigation.MenuItems.OfType<NavigationViewItem>().First(x => (string)x.Tag == "audio");
                }));
                liveLamps.Children.Add(Surface(empty));
            }
            else
            {
                foreach (var lamp in snapshot.Value.GetProperty("lamps").EnumerateArray())
                {
                    var row = new Grid { ColumnSpacing = 20 };
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(64) });
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                    var swatch = new Ellipse { Width = 48, Height = 48, Fill = new SolidColorBrush(ParseColor(lamp.GetProperty("color").GetString()!)), VerticalAlignment = VerticalAlignment.Center };
                    AutomationProperties.SetName(swatch, "Current scene " + lamp.GetProperty("color").GetString());
                    row.Children.Add(swatch);
                    var detail = Column(4);
                    detail.Children.Add(Text($"Lamp {lamp.GetProperty("number").GetInt32()}  /  {lamp.GetProperty("name").GetString()}", "BodyStrongTextBlockStyle"));
                    detail.Children.Add(Text(!lamp.GetProperty("connected").GetBoolean() ? "Searching for lamp" :
                        lamp.GetProperty("verified").GetBoolean() ? "Connected · color confirmed by lamp" :
                        lamp.GetProperty("readback").GetBoolean() ? "Connected · awaiting readback" : "Connected · no hardware readback", "CaptionTextBlockStyle"));
                    Grid.SetColumn(detail, 1); row.Children.Add(detail);
                    int number = lamp.GetProperty("number").GetInt32();
                    var ack = Button("Acknowledge", () => Run(() => SendCommandAsync(new { action = "ack", lamp = number })));
                    Grid.SetColumn(ack, 2); row.Children.Add(ack);
                    liveLamps.Children.Add(Surface(row));
                }
                if (liveLamps.Children.Count == 0) liveLamps.Children.Add(Text("No lamps paired yet. Use Add lamp to connect your first light."));
                foreach (var source in snapshot.Value.GetProperty("sources").EnumerateArray())
                {
                    int lamp = source.GetProperty("lamp").GetInt32();
                    liveSources.Children.Add(Text($"{source.GetProperty("name").GetString()}  ·  {source.GetProperty("state").GetString()}" +
                        $"  ·  {(lamp == 0 ? "all lamps" : "lamp " + lamp)}" +
                        (source.GetProperty("acked").GetBoolean() ? "  ·  acknowledged" : "")));
                }
            }
            if (liveSources.Children.Count == 0) liveSources.Children.Add(Text("No active agents. Install a hook or preview an event in Event rules."));
            UpdateFreshness();
        }
        if (liveChannels is not null)
        {
            liveChannels.Children.Clear();
            for (int ch = 0; ch < 4; ch++)
            {
                string owner = "Not observed";
                if (snapshot is not null)
                {
                    var channel = snapshot.Value.GetProperty("channels")[ch];
                    owner = channel.GetProperty("used").GetBoolean() ? channel.GetProperty("name").GetString()! : "Available";
                }
                liveChannels.Children.Add(Text($"Channel {ch}  ·  {owner}" + (ch == settings.Channel ? "  ·  this devbox" : "")));
            }
        }
        if (liveActivity is not null)
        {
            liveActivity.Children.Clear();
            foreach (string item in activity.TakeLast(8).Reverse()) liveActivity.Children.Add(Text(item, "CaptionTextBlockStyle"));
            if (activity.Count == 0) liveActivity.Children.Add(Text("No events yet.", "CaptionTextBlockStyle"));
        }
    }

    private async Task PairLampAsync()
    {
        if (pairingRequested) return;
        pairingRequested = true;
        pairingConnection = bridge.ConnectionId;
        RefreshPairing();
        try
        {
            await SendCommandAsync(new { action = "claim" });
            ShowNotice("Adding a lamp", "Waiting for the dongle's pairing result. Results arrive over the microphone return stream.");
        }
        catch
        {
            pairingRequested = false; RefreshPairing();
            throw;
        }
    }

    private void RefreshPairing()
    {
        if (!bridge.Running || pairingConnection != bridge.ConnectionId) pairingRequested = false;
        if (addLamp is null || pairingStatus is null || pairingProgress is null) return;
        bool fresh = bridge.Running && snapshot is not null && bridge.LastSnapshot != default &&
            DateTimeOffset.UtcNow - bridge.LastSnapshot <= TimeSpan.FromSeconds(15);
        JsonElement status = default;
        bool supported = snapshot is not null && snapshot.Value.TryGetProperty("pairing", out status) &&
            status.ValueKind == JsonValueKind.Object;
        string? state = supported ? status.GetProperty("state").GetString() : null;
        int count = snapshot?.GetProperty("lamps").GetArrayLength() ?? 0;
        var owner = snapshot?.GetProperty("channels")[settings.Channel];
        bool mine = owner is not null && owner.Value.GetProperty("used").GetBoolean() &&
            owner.Value.GetProperty("name").GetString() == settings.Name;
        bool busy = pairingRequested || state == "searching";
        addLamp.IsEnabled = fresh && supported && mine && count < 4 && !busy;
        pairingProgress.IsActive = fresh && busy;
        pairingProgress.Visibility = pairingProgress.IsActive ? Visibility.Visible : Visibility.Collapsed;
        pairingStatus.Text = !fresh ? "Connect both audio directions and wait for live shared state to add a lamp."
            : !supported ? "Update the dongle firmware to add lamps here. Holding BOOT for 3 seconds still works."
            : !mine ? "Reserve this devbox's audio channel before adding a lamp."
            : count >= 4 ? "All four lamp slots are paired."
            : pairingRequested ? "Looking for a nearby lamp. Waiting for confirmation from the dongle..."
            : state switch
            {
                "searching" => "A lamp search is running. Its result will appear here on every devbox.",
                "paired" => $"Lamp {status.GetProperty("lamp").GetInt32()} paired. Its connection status appears below.",
                "not-found" => "No unpaired lamp found. Move it closer, close its phone app, and try again.",
                "full" => "All four lamp slots are paired.",
                "forgotten" => "Lamp memory was cleared on the dongle. Use Add lamp to pair again.",
                _ => "Ready to add a lamp. Existing lamps stay paired."
            };
    }

    private void AudioPage()
    {
        Heading("Audio channels", "Separate uplink channels. One shared microphone return stream.");
        var instructions = Section("Connect both directions");
        instructions.Children.Add(Text("On the laptop, route each RDP client's playback to KS Light Bridge and select its microphone for remote recording. In Remote Desktop, enable Play on this computer and Record from this computer. Disable microphone noise suppression and audio enhancements."));
        instructions.Children.Add(Text("Each devbox needs its own channel (0-3) and a unique 8-character name. All devboxes must receive the same dongle microphone stream; the app verifies that return path before sending commands."));
        var form = Column(16);
        var name = new TextBox { Header = "Devbox name", Text = settings.Name, MaxLength = 8, Description = "Printable ASCII, unique across your devboxes." };
        var channel = Choices("Uplink channel", ["0", "1", "2", "3"], settings.Channel);
        var output = Choices("Playback device", ["Loading devices..."]);
        var input = Choices("Microphone return device", ["Loading devices..."]);
        var devices = new List<(int Id, string Name)>[2] { [], [] };
        async Task RefreshDevices()
        {
            try
            {
                JsonElement list = await AudioBridge.DevicesAsync(settings);
                for (int side = 0; side < 2; side++)
                {
                    var box = side == 0 ? output : input;
                    var entries = devices[side]; entries.Clear(); box.Items.Clear();
                    string wanted = side == 0 ? settings.OutputName : settings.InputName;
                    foreach (var endpoint in list.GetProperty(side == 0 ? "outputs" : "inputs").EnumerateArray())
                    {
                        var entry = (endpoint.GetProperty("id").GetInt32(), endpoint.GetProperty("name").GetString()!);
                        entries.Add(entry); box.Items.Add(entry.Item2);
                    }
                    int selected = entries.FindIndex(x => x.Name == wanted);
                    if (wanted == "Windows default device")
                    {
                        int usb = entries.FindIndex(x => x.Name.Contains("usb uac", StringComparison.OrdinalIgnoreCase));
                        if (usb >= 0) selected = usb;
                    }
                    box.SelectedIndex = selected < 0 ? 0 : selected;
                }
            }
            catch (Exception error) { ShowError("Could not list audio devices. Set a working Python executable in Settings. " + error.Message); }
        }
        form.Children.Add(name); form.Children.Add(channel); form.Children.Add(output); form.Children.Add(input);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        actions.Children.Add(Button("Save and connect", () => Run(async () =>
        {
            string clean = name.Text.Trim();
            if (clean.Length == 0 || clean.Any(c => c < 32 || c > 126)) throw new ArgumentException("Use 1-8 printable ASCII characters.");
            if (output.SelectedIndex < 0 || input.SelectedIndex < 0 || devices[0].Count == 0 || devices[1].Count == 0)
                throw new InvalidOperationException("Refresh and select both audio devices first.");
            settings.Name = clean; settings.Channel = channel.SelectedIndex;
            var outDevice = devices[0][output.SelectedIndex]; var inDevice = devices[1][input.SelectedIndex];
            settings.Output = outDevice.Id; settings.OutputName = outDevice.Name;
            settings.Input = inDevice.Id; settings.InputName = inDevice.Name;
            settings.ConnectAtLaunch = true; settings.Save(); MachineLabel.Text = clean;
            snapshot = null;
            await connect();
        }), true));
        actions.Children.Add(Button("Refresh devices", () => Run(RefreshDevices)));
        actions.Children.Add(Button("Disconnect", () =>
        {
            settings.ConnectAtLaunch = false; settings.Save(); bridge.Stop(); snapshot = null;
            ShowNotice("Disconnected", "No commands will be sent until you connect again."); RefreshLive();
        }));
        form.Children.Add(actions);
        PageContent.Children.Add(Surface(form));
        liveChannels = Column(10);
        Section("Shared channel ownership", "Reservations expire after 60 seconds without a heartbeat. A busy channel is never silently taken.").Children.Add(liveChannels);
        RefreshLive();
        Run(RefreshDevices);
    }

    private void RulesPage()
    {
        Heading("Event rules", "Colors travel with each event. The dongle broadcasts the same configuration to every devbox.");
        rulesDirty = false;
        ruleChannel = Choices("Devbox channel", ["Channel 0", "Channel 1", "Channel 2", "Channel 3"], settings.Channel);
        ruleProvider = Choices("Agent", ["GitHub Copilot CLI", "Claude Code"]);
        ruleChannel.SelectionChanged += (_, _) => BuildRuleRows();
        ruleProvider.SelectionChanged += (_, _) => BuildRuleRows();
        PageContent.Children.Add(ruleChannel); PageContent.Children.Add(ruleProvider);
        ruleRows = Column(8); PageContent.Children.Add(ruleRows);
        PageContent.Children.Add(Button("Save shared rules", () => Run(async () =>
        {
            if (ruleChannel.SelectedIndex != settings.Channel) throw new InvalidOperationException("Only the owning devbox can edit its channel.");
            if (snapshot is null) throw new InvalidOperationException("Connect the microphone return stream first.");
            if (editors.Any(e => !double.IsFinite(e.Brightness.Value) || e.Brightness.Value < 0 || e.Brightness.Value > 100))
                throw new ArgumentException("Enter a brightness from 0 to 100 for every event.");
            string provider = ruleProvider.SelectedIndex == 0 ? "copilot" : "claude";
            var rules = editors.Select(e => new { provider, state = e.State, color = Hex(e.Color),
                brightness = (int)e.Brightness.Value, lamp = e.Lamp.SelectedIndex }).ToArray();
            await SendCommandAsync(new { action = "rules", rules });
            ShowNotice("Saving rules", "Changes are pending until the dongle broadcasts matching values. Audio updates can take several seconds per event.");
        }), true));
        BuildRuleRows();
    }

    private sealed record RuleEditor(string State, Color InitialColor, NumberBox Brightness, ComboBox Lamp)
    {
        internal Color Color { get; set; } = InitialColor;
    }

    private void BuildRuleRows()
    {
        if (ruleRows is null || ruleChannel is null || ruleProvider is null) return;
        ruleRows.Children.Clear(); editors.Clear(); rulesDirty = false;
        bool own = ruleChannel.SelectedIndex == settings.Channel;
        if (snapshot is null)
        {
            ruleRows.Children.Add(Text("Connect to the dongle to load its shared rules. No local defaults will overwrite another devbox."));
            return;
        }
        if (!own) ruleRows.Children.Add(Text("Viewing another devbox. Edit these rules from the devbox that owns this channel."));
        string provider = ruleProvider.SelectedIndex == 0 ? "copilot" : "claude";
        for (int state = 0; state < 5; state++)
        {
            string key = StateNames[state];
            var rule = snapshot.Value.GetProperty("rules").EnumerateArray().First(r =>
                r.GetProperty("channel").GetInt32() == ruleChannel.SelectedIndex &&
                r.GetProperty("provider").GetString() == provider && r.GetProperty("state").GetString() == key);
            Color color = ParseColor(rule.GetProperty("color").GetString()!);
            var picker = new ColorPicker { Color = color, IsAlphaEnabled = false };
            var swatch = new Rectangle { Width = 20, Height = 20, RadiusX = 4, RadiusY = 4, Fill = new SolidColorBrush(color) };
            var colorContent = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            var hex = Text(Hex(color)); colorContent.Children.Add(swatch); colorContent.Children.Add(hex);
            var colorButton = new Button { Content = colorContent, Flyout = new Flyout { Content = picker }, IsEnabled = own };
            AutomationProperties.SetName(colorButton, StateLabels[state] + " color");
            var bright = new NumberBox { Header = "Brightness %", Minimum = 0, Maximum = 100,
                Value = rule.GetProperty("brightness").GetInt32(), SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact, Width = 120, IsEnabled = own };
            var lamp = Choices("Lamp", ["Auto", "1", "2", "3", "4"], rule.GetProperty("lamp").GetInt32());
            lamp.MinWidth = 80;
            lamp.IsEnabled = own;
            var editor = new RuleEditor(key, color, bright, lamp);
            editors.Add(editor);
            picker.ColorChanged += (_, e) =>
            {
                editor.Color = e.NewColor; swatch.Fill = new SolidColorBrush(e.NewColor);
                hex.Text = Hex(e.NewColor); rulesDirty = true;
            };
            bright.ValueChanged += (_, _) => rulesDirty = true;
            lamp.SelectionChanged += (_, _) => rulesDirty = true;
            var content = Column(12);
            content.Children.Add(Text(StateLabels[state], "BodyStrongTextBlockStyle"));
            if (state == 0) content.Children.Add(Text("Clears this agent; the lamp returns to its base or another active alert.", "CaptionTextBlockStyle"));
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
            if (state != 0) { row.Children.Add(colorButton); row.Children.Add(bright); row.Children.Add(lamp); }
            var preview = Button(state == 0 ? "Clear agent" : "Preview event", () => Run(async () =>
            {
                // Preview uses an actual signal so the physical BOOT button can acknowledge it.
                if (!double.IsFinite(bright.Value)) throw new ArgumentException("Enter a brightness from 0 to 100.");
                string agentName = settings.Name[..Math.Min(5, settings.Name.Length)] + settings.Channel + (provider == "copilot" ? "C" : "A");
                await SendCommandAsync(new { action = "preview", provider, state = key, color = Hex(editor.Color),
                    brightness = (int)bright.Value, lamp = lamp.SelectedIndex, name = agentName });
                ShowNotice("Preview sent", key == "working" ? "This is a real working event. Use Clear agent to restore the base color."
                    : key == "idle" ? "This agent was cleared. Other active alerts still take priority."
                    : "This is a real event. Acknowledge it or clear the agent to restore the base color.");
            }));
            preview.FontSize = 12;
            preview.IsEnabled = own; row.Children.Add(preview);
            content.Children.Add(row); ruleRows.Children.Add(Surface(content));
        }
    }

    private void IntegrationsPage()
    {
        Heading("Integrations", "Native hooks, local delivery. No prompt or tool content leaves the devbox.");
        foreach (string provider in new[] { "copilot", "claude" })
        {
            var section = Section(provider == "copilot" ? "GitHub Copilot CLI" : "Claude Code",
                "Working, done, error, needs-you, and idle events use this channel's shared color rules.");
            section.Children.Add(Text(HookInstaller.PathFor(provider), "CaptionTextBlockStyle"));
            var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
            actions.Children.Add(Button("Install / update hooks", () =>
            {
                try
                {
                    string path = HookInstaller.Install(provider);
                    ShowNotice("Hooks installed", "Restart agent sessions to load them. A backup of existing configuration was saved alongside " + path);
                }
                catch (Exception error) { ShowError(error.Message); }
            }, true));
            actions.Children.Add(Button("Send test event", () => Run(() => SendCommandAsync(new { action = "signal", provider, state = "needs-you" }))));
            section.Children.Add(actions);
        }
        Section("Safe by design").Children.Add(Text("Hooks send only the agent name and event type to a current-user local pipe. If the app is closed, the hook launches it in the background. Hook failures never block your coding session. Existing Claude hooks are preserved; legacy ks.py light hooks should be removed manually to avoid duplicate signals."));
    }

    private void SettingsPage()
    {
        Heading("Settings", "Quiet in the background. Ready when an agent needs you.");
        var startup = new ToggleSwitch { Header = "Start at Windows sign-in", IsOn = AppSettings.StartupEnabled,
            OnContent = "Runs in the notification area", OffContent = "Launch manually or from an agent hook" };
        startup.Toggled += (_, _) =>
        {
            try { AppSettings.StartupEnabled = startup.IsOn; }
            catch (Exception error) { ShowError(error.Message); }
        };
        PageContent.Children.Add(Surface(startup));
        var theme = Choices("App theme", ["Use Windows setting", "Light", "Dark"], settings.Theme == "Light" ? 1 : settings.Theme == "Dark" ? 2 : 0);
        theme.SelectionChanged += (_, _) =>
        {
            settings.Theme = theme.SelectedIndex == 1 ? "Light" : theme.SelectedIndex == 2 ? "Dark" : "Default";
            Root.RequestedTheme = Enum.Parse<ElementTheme>(settings.Theme); settings.Save();
        };
        PageContent.Children.Add(theme);
        var python = new TextBox { Header = "Python executable", Text = settings.Python,
            Description = "Python 3.10+ with host\\requirements.txt installed (Reed-Solomon audio recovery)." };
        PageContent.Children.Add(python);
        PageContent.Children.Add(Button("Save Python path", () =>
        {
            settings.Python = python.Text.Trim(); settings.Save();
            ShowNotice("Python path saved", "Reconnect in Audio channels to use it.");
        }));
        Section("Background behavior").Children.Add(Text("Closing the window keeps the audio bridge and hooks live in the notification area. The audio worker reconnects after failure. Sign-in startup is per user; a Windows desktop session must be running. Use Exit to stop the application."));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        actions.Children.Add(Button("Open diagnostics", () =>
        {
            System.IO.Directory.CreateDirectory(AppSettings.DirectoryPath);
            Process.Start(new ProcessStartInfo("explorer.exe", AppSettings.DirectoryPath) { UseShellExecute = true });
        }));
        actions.Children.Add(Button("Exit dongleLight", exit));
        PageContent.Children.Add(actions);
    }

    internal void Receive(JsonElement message)
    {
        string? kind = message.GetProperty("kind").GetString();
        if (kind == "snapshot")
        {
            bool first = snapshot is null;
            snapshot = message;
            var owner = message.GetProperty("channels")[settings.Channel];
            bool mine = owner.GetProperty("used").GetBoolean() && owner.GetProperty("name").GetString() == settings.Name;
            if (mine) ShowNotice("Duplex audio live", $"Channel {settings.Channel} reserved. Shared state is arriving from the dongle.");
            else ShowNotice("Return audio live", "Waiting for channel reservation. If another devbox owns it, select a free channel.");
            RefreshLive();
            if (page == "rules" && first) BuildRuleRows();
            // Preserve edits, but keep non-edited views synchronized with remote changes.
            else if (page == "rules" && !rulesDirty) BuildRuleRows();
        }
        else if (kind == "result")
        {
            if (message.TryGetProperty("action", out var action) && action.GetString() == "claim")
            {
                pairingRequested = false;
                RefreshPairing();
            }
            string text = message.GetProperty("message").GetString()!;
            if (message.GetProperty("ok").GetBoolean())
            {
                AddActivity(text);
                if (text == "Rules confirmed by dongle" && snapshot is not null && page == "rules")
                {
                    string provider = ruleProvider!.SelectedIndex == 0 ? "copilot" : "claude";
                    rulesDirty = editors.Any(e =>
                    {
                        var rule = snapshot.Value.GetProperty("rules").EnumerateArray().First(r =>
                            r.GetProperty("channel").GetInt32() == ruleChannel!.SelectedIndex &&
                            r.GetProperty("provider").GetString() == provider && r.GetProperty("state").GetString() == e.State);
                        return rule.GetProperty("color").GetString() != Hex(e.Color) ||
                            rule.GetProperty("brightness").GetInt32() != e.Brightness.Value ||
                            rule.GetProperty("lamp").GetInt32() != e.Lamp.SelectedIndex;
                    });
                    if (!rulesDirty) BuildRuleRows();
                }
                ShowNotice("Audio bridge", text);
            }
            else ShowError(text);
        }
        else if (kind == "error") ShowError(message.GetProperty("message").GetString()!);
    }

    internal void UpdateFreshness()
    {
        bool received = snapshot is not null && bridge.LastSnapshot != default;
        double seconds = received ? (DateTimeOffset.UtcNow - bridge.LastSnapshot).TotalSeconds : double.PositiveInfinity;
        if (freshness is not null)
            freshness.Text = !received ? "Waiting for shared state." : $"Shared state received {Math.Max(0, (int)seconds)}s ago";
        if (snapshot is not null && seconds > 15)
        {
            StatusBanner.Severity = InfoBarSeverity.Warning;
            StatusBanner.Title = "Return audio is stale";
            StatusBanner.Message = "Displayed state is cached. Check microphone routing; commands will wait for a live return stream.";
        }
        RefreshPairing();
    }

    internal void ShowNotice(string title, string message)
    {
        StatusBanner.IsOpen = true; StatusBanner.Severity = InfoBarSeverity.Informational;
        StatusBanner.Title = title; StatusBanner.Message = message;
    }

    internal void ShowError(string error)
    {
        RefreshPairing();
        StatusBanner.IsOpen = true; StatusBanner.Severity = InfoBarSeverity.Error;
        StatusBanner.Title = "Audio bridge needs attention"; StatusBanner.Message = error;
        AddActivity(error); AppSettings.Log(error);
    }

    internal void AddActivity(string message)
    {
        activity.Add(DateTime.Now.ToString("HH:mm:ss") + "  " + message);
        if (activity.Count > 100) activity.RemoveAt(0);
        if (liveActivity is not null) RefreshLive();
    }

    private static string Hex(Color c) => $"#{c.R:X2}{c.G:X2}{c.B:X2}";
    private static Color ParseColor(string value) => ColorHelper.FromArgb(255,
        Convert.ToByte(value.Substring(1, 2), 16), Convert.ToByte(value.Substring(3, 2), 16), Convert.ToByte(value.Substring(5, 2), 16));
}
