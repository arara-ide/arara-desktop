using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Automation;
using System.Collections.Concurrent;

internal sealed class DesktopFailure(string code) : Exception(code);
internal sealed record Target(IntPtr Handle, int ProcessId, AutomationElement Element);
internal sealed record Node(string WindowId, AutomationElement Element, string Name, int[] RuntimeId);
internal sealed record Frame(string WindowId, Rectangle Bounds, DateTime Created);

internal static class Program
{
    private static readonly Dictionary<string, Target> Windows = [];
    private static readonly Dictionary<string, Node> Nodes = [];
    private static readonly Dictionary<string, Frame> Frames = [];
    private static readonly HashSet<ushort> Keys = [];
    private static readonly HashSet<string> Buttons = [];
    private static readonly BlockingCollection<string> Requests = new(16);
    private static CancellationTokenSource Cancellation = new();
    private static readonly object CancellationLock = new();
    private static JsonSerializerOptions JsonOptions => new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    [STAThread]
    private static void Main()
    {
        SetProcessDpiAwarenessContext(new IntPtr(-4));
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; Cancel(); };
        var worker = new Thread(Work) { IsBackground = false };
        worker.SetApartmentState(ApartmentState.STA);
        worker.Start();
        string? line;
        while ((line = Console.ReadLine()) != null)
        {
            if (line.Length > 64000) { Reply(null, "invalid_request"); continue; }
            try
            {
                using var parsed = JsonDocument.Parse(line);
                if (String(parsed.RootElement, "action") == "stop") Cancel();
            }
            catch (JsonException) { Reply(null, "invalid_request"); continue; }
            Requests.Add(line);
        }
        Cancel();
        Requests.CompleteAdding();
        worker.Join(6000);
    }

    private static void Work()
    {
        try
        {
            foreach (var line in Requests.GetConsumingEnumerable())
            {
                try
                {
                    using var parsed = JsonDocument.Parse(line);
                    Reply(Execute(parsed.RootElement), null);
                }
                catch (DesktopFailure error) { Reply(null, error.Message); }
                catch { Reply(null, "failed"); }
            }
        }
        finally { ReleaseInput(); }
    }

    private static void Reply(object? result, string? error)
    {
        var reply = error == null ? new { version = 1, result } as object : new { version = 1, error };
        lock (Console.Out) Console.WriteLine(JsonSerializer.Serialize(reply, JsonOptions));
    }
    private static string String(JsonElement request, string key) => request.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
    private static int Number(JsonElement request, string key, int fallback = 0) => request.TryGetProperty(key, out var value) && value.TryGetInt32(out var number) ? number : fallback;
    private static void Cancel() { lock (CancellationLock) Cancellation.Cancel(); }
    private static Target Window(string id)
    {
        if (!Windows.TryGetValue(id, out var target) || !IsWindow(target.Handle)) throw new DesktopFailure("stale");
        GetWindowThreadProcessId(target.Handle, out var process);
        if (process != target.ProcessId || !Allowed(target.Handle, target.ProcessId)) throw new DesktopFailure("stale");
        return target;
    }
    private static bool Allowed(IntPtr handle, int pid)
    {
        try
        {
            var name = Process.GetProcessById(pid).ProcessName;
            if (new[] { "consent", "CredentialUIBroker", "LogonUI", "SystemSettings" }.Contains(name, StringComparer.OrdinalIgnoreCase)) return false;
            var process = OpenProcess(0x1000, false, (uint)pid);
            if (process == IntPtr.Zero) return false;
            try
            {
                if (!OpenProcessToken(process, 8, out var token)) return false;
                try
                {
                    var elevation = 0;
                    if (!GetTokenInformation(token, 20, ref elevation, 4, out _)) return false;
                    return elevation == 0 && IsWindowVisible(handle);
                }
                finally { CloseHandle(token); }
            }
            finally { CloseHandle(process); }
        }
        catch { return false; }
    }
    private static object ListWindows(JsonElement request)
    {
        Windows.Clear(); Nodes.Clear(); Frames.Clear();
        var result = new List<object>();
        EnumWindows((handle, _) =>
        {
            if (result.Count >= 100) return false;
            GetWindowThreadProcessId(handle, out var pid);
            if (!Allowed(handle, (int)pid)) return true;
            var app = Process.GetProcessById((int)pid).ProcessName;
            var selected = String(request, "application");
            if (selected.Length > 0 && !string.Equals(app, selected, StringComparison.OrdinalIgnoreCase)) return true;
            try
            {
                var root = AutomationElement.FromHandle(handle);
                var title = root.Current.Name;
                if (title.Length == 0) return true;
                var id = Guid.NewGuid().ToString();
                Windows[id] = new(handle, (int)pid, root);
                result.Add(new { windowId = id, app, title = title[..Math.Min(title.Length, 160)], active = GetForegroundWindow() == handle });
            }
            catch { }
            return true;
        }, IntPtr.Zero);
        return new { windows = result, truncated = result.Count >= 100 };
    }
    private static string[] Operations(AutomationElement element)
    {
        var result = new List<string>();
        if (element.TryGetCurrentPattern(InvokePattern.Pattern, out _)) result.Add("press");
        if (element.TryGetCurrentPattern(ValuePattern.Pattern, out var value) && !((ValuePattern)value).Current.IsReadOnly) result.Add("set_value");
        if (element.Current.IsKeyboardFocusable) result.Add("focus");
        return [.. result];
    }
    private static object Snapshot(string id)
    {
        var target = Window(id);
        var queue = new Queue<(AutomationElement Element, int Depth)>();
        queue.Enqueue((target.Element, 0));
        var next = new Dictionary<string, Node>();
        var nodes = new List<object>();
        var watch = Stopwatch.StartNew();
        var visited = 0;
        while (queue.TryDequeue(out var item) && nodes.Count < 100 && visited++ < 600 && watch.ElapsedMilliseconds < 3000)
        {
            var element = item.Element;
            try
            {
                if (element.Current.IsPassword) continue;
                var name = element.Current.Name;
                var runtime = element.GetRuntimeId();
                var uid = Nodes.FirstOrDefault(pair => pair.Value.WindowId == id && pair.Value.RuntimeId.SequenceEqual(runtime)).Key ?? Guid.NewGuid().ToString();
                next[uid] = new(id, element, name, runtime);
                var value = element.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern) ? ((ValuePattern)pattern).Current.Value : null;
                nodes.Add(new { uid, role = element.Current.ControlType.ProgrammaticName, name = name[..Math.Min(name.Length, 160)], value = value?[..Math.Min(value.Length, 240)], enabled = element.Current.IsEnabled, operations = Operations(element) });
                if (item.Depth < 12)
                {
                    var child = TreeWalker.ControlViewWalker.GetFirstChild(element);
                    var count = 0;
                    while (child != null && count++ < 100) { queue.Enqueue((child, item.Depth + 1)); child = TreeWalker.ControlViewWalker.GetNextSibling(child); }
                }
            }
            catch (ElementNotAvailableException) { }
        }
        Nodes.Clear(); foreach (var pair in next) Nodes[pair.Key] = pair.Value;
        return new { windowId = id, nodes, truncated = queue.Count > 0 };
    }
    private static Node TargetNode(JsonElement request)
    {
        var id = String(request, "windowId");
        var target = Window(id);
        if (!Nodes.TryGetValue(String(request, "uid"), out var node) || node.WindowId != id || !node.RuntimeId.SequenceEqual(node.Element.GetRuntimeId()) || node.Name != node.Element.Current.Name) throw new DesktopFailure("stale");
        if (node.Element.Current.IsPassword) throw new DesktopFailure("secure");
        if (target.ProcessId != node.Element.Current.ProcessId) throw new DesktopFailure("stale");
        return node;
    }
    private static void FocusCheck(Target target)
    {
        if (GetForegroundWindow() != target.Handle || !Allowed(target.Handle, target.ProcessId)) throw new DesktopFailure("focus");
        try { if (AutomationElement.FocusedElement?.Current.IsPassword == true) throw new DesktopFailure("secure"); }
        catch (ElementNotAvailableException) { throw new DesktopFailure("stale"); }
    }
    private static Rectangle Bounds(Target target)
    {
        if (!GetWindowRect(target.Handle, out var rectangle)) throw new DesktopFailure("stale");
        return Rectangle.FromLTRB(rectangle.Left, rectangle.Top, rectangle.Right, rectangle.Bottom);
    }
    private static object Screenshot(string id)
    {
        var target = Window(id); FocusCheck(target);
        var bounds = Bounds(target);
        if (bounds.Width <= 0 || bounds.Height <= 0 || bounds.Width > 16000 || bounds.Height > 16000) throw new DesktopFailure("stale");
        using var bitmap = new Bitmap(bounds.Width, bounds.Height);
        using (var graphics = Graphics.FromImage(bitmap)) graphics.CopyFromScreen(bounds.Location, Point.Empty, bounds.Size);
        var factor = Math.Min(1.0, 1280.0 / bounds.Width);
        using var resized = new Bitmap(bitmap, new Size(Math.Max(1, (int)(bounds.Width * factor)), Math.Max(1, (int)(bounds.Height * factor))));
        using var stream = new MemoryStream(); resized.Save(stream, ImageFormat.Png);
        if (stream.Length > 4000000) throw new DesktopFailure("failed");
        var frameId = Guid.NewGuid().ToString();
        Frames.Clear(); Frames[frameId] = new(id, bounds, DateTime.UtcNow);
        return new { frameId, windowId = id, width = resized.Width, height = resized.Height, coordinateWidth = bounds.Width, coordinateHeight = bounds.Height, images = new[] { new { data = Convert.ToBase64String(stream.ToArray()), mimeType = "image/png" } } };
    }
    private static void Send(INPUT input)
    {
        if (SendInput(1, [input], Marshal.SizeOf<INPUT>()) != 1) throw new DesktopFailure("failed");
    }
    private static void Key(ushort key, bool down) => Send(new INPUT { Type = 1, Data = new InputUnion { Keyboard = new KEYBDINPUT { Key = key, Flags = down ? 0u : 2u } } });
    private static void Mouse(string button, bool down)
    {
        var flag = button == "left" ? (down ? 2u : 4u) : button == "right" ? (down ? 8u : 16u) : (down ? 32u : 64u);
        Send(new INPUT { Data = new InputUnion { Mouse = new MOUSEINPUT { Flags = flag } } });
    }
    private static void ReleaseInput()
    {
        foreach (var key in Keys) { try { Key(key, false); } catch { } }
        foreach (var button in Buttons) { try { Mouse(button, false); } catch { } }
        Keys.Clear(); Buttons.Clear();
    }
    private static ushort KeyCode(string key) => key.ToUpperInvariant() switch
    {
        "ENTER" => 13, "TAB" => 9, "SPACE" => 32, "ESCAPE" => 27, "BACKSPACE" => 8, "META" => 91, "SHIFT" => 16, "CTRL" => 17, "ALT" => 18,
        "LEFT" => 37, "UP" => 38, "RIGHT" => 39, "DOWN" => 40,
        var name when name.Length == 1 && char.IsAsciiLetterOrDigit(name[0]) => name[0],
        _ => throw new DesktopFailure("unsupported")
    };
    private static object Input(JsonElement request)
    {
        var id = String(request, "windowId"); var target = Window(id); FocusCheck(target);
        if (!Frames.TryGetValue(String(request, "frameId"), out var frame) || frame.WindowId != id || DateTime.UtcNow - frame.Created > TimeSpan.FromSeconds(15) || Bounds(target) != frame.Bounds) throw new DesktopFailure("stale");
        var duration = Number(request, "durationMs");
        if (duration < 0 || duration > 5000) throw new DesktopFailure("unsupported");
        lock (CancellationLock) { Cancellation.Dispose(); Cancellation = new(); }
        try
        {
            var operation = String(request, "operation");
            switch (operation)
            {
                case "move": case "click":
                    var x = Number(request, "x", -1); var y = Number(request, "y", -1);
                    if (x < 0 || y < 0 || x >= frame.Bounds.Width || y >= frame.Bounds.Height) throw new DesktopFailure("unsupported");
                    if (!SetCursorPos(frame.Bounds.X + x, frame.Bounds.Y + y)) throw new DesktopFailure("failed");
                    if (operation == "click") { var button = String(request, "button"); if (button == "") button = "left"; if (!new[] { "left", "right", "middle" }.Contains(button)) throw new DesktopFailure("unsupported"); Buttons.Add(button); Mouse(button, true); }
                    break;
                case "relative_move":
                    var dx = Number(request, "dx"); var dy = Number(request, "dy");
                    if (Math.Abs((long)dx) > 2000 || Math.Abs((long)dy) > 2000) throw new DesktopFailure("unsupported");
                    Send(new INPUT { Data = new InputUnion { Mouse = new MOUSEINPUT { X = dx, Y = dy, Flags = 1 } } });
                    break;
                case "keys":
                    var keys = request.GetProperty("keys").EnumerateArray().Select(value => KeyCode(value.GetString() ?? "")).ToArray();
                    if (keys.Length < 1 || keys.Length > 6 || keys.Any(key => (GetAsyncKeyState(key) & 0x8000) != 0)) throw new DesktopFailure("intervention");
                    foreach (var key in keys) { Keys.Add(key); Key(key, true); }
                    break;
                case "text":
                    var text = String(request, "value"); if (text.Length > 8000) throw new DesktopFailure("unsupported");
                    foreach (var character in text)
                    {
                        Send(new INPUT { Type = 1, Data = new InputUnion { Keyboard = new KEYBDINPUT { Scan = character, Flags = 4 } } });
                        Send(new INPUT { Type = 1, Data = new InputUnion { Keyboard = new KEYBDINPUT { Scan = character, Flags = 6 } } });
                    }
                    break;
                default: throw new DesktopFailure("unsupported");
            }
            var watch = Stopwatch.StartNew();
            while (watch.ElapsedMilliseconds < duration) { if (Cancellation.IsCancellationRequested) throw new DesktopFailure("cancelled"); FocusCheck(target); if (Bounds(target) != frame.Bounds) throw new DesktopFailure("stale"); Thread.Sleep(10); }
            return new { performed = operation, released = true, verified = false };
        }
        finally { ReleaseInput(); Frames.Clear(); }
    }
    private static object Execute(JsonElement request)
    {
        if (Number(request, "version") != 1) throw new DesktopFailure("unsupported");
        var action = String(request, "action");
        if (action == "status") return new { platform = "win32", accessibility = Environment.UserInteractive, input = Environment.UserInteractive, screenshots = Environment.UserInteractive, audio = false, actions = new[] { "list_windows", "take_snapshot", "read", "act", "focus_window", "take_screenshot", "input", "launch_app", "stop" } };
        if (action == "stop") { ReleaseInput(); Frames.Clear(); return new { stopped = true }; }
        if (action == "list_windows") return ListWindows(request);
        if (action == "launch_app")
        {
            var application = String(request, "application");
            if (!Path.IsPathFullyQualified(application) || !File.Exists(application) || !application.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) throw new DesktopFailure("unsupported");
            Process.Start(new ProcessStartInfo(application) { UseShellExecute = false }); return new { requested = true };
        }
        var id = String(request, "windowId"); var target = Window(id);
        if (action == "focus_window") { if (!SetForegroundWindow(target.Handle)) throw new DesktopFailure("focus"); Nodes.Clear(); return new { focused = true }; }
        if (action == "take_snapshot") return Snapshot(id);
        if (action == "take_screenshot") return Screenshot(id);
        if (action == "input") return Input(request);
        var node = TargetNode(request);
        if (action == "read")
        {
            var text = node.Element.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern) ? ((ValuePattern)pattern).Current.Value : node.Element.Current.Name;
            var offset = Math.Clamp(Number(request, "offset"), 0, text.Length); var count = Math.Clamp(Number(request, "limit", 100), 1, 100) * 40;
            return new { value = text.Substring(offset, Math.Min(count, text.Length - offset)), offset, truncated = offset + count < text.Length };
        }
        if (action != "act") throw new DesktopFailure("unsupported");
        FocusCheck(target);
        switch (String(request, "operation"))
        {
            case "press": ((InvokePattern)node.Element.GetCurrentPattern(InvokePattern.Pattern)).Invoke(); break;
            case "set_value": var value = String(request, "value"); if (value.Length > 8000) throw new DesktopFailure("unsupported"); ((ValuePattern)node.Element.GetCurrentPattern(ValuePattern.Pattern)).SetValue(value); break;
            case "focus": node.Element.SetFocus(); break;
            default: throw new DesktopFailure("unsupported");
        }
        Nodes.Clear(); return new { performed = String(request, "operation"), verified = false };
    }

    private delegate bool EnumWindowsCallback(IntPtr handle, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)] private struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct INPUT { public uint Type; public InputUnion Data; }
    [StructLayout(LayoutKind.Explicit)] private struct InputUnion { [FieldOffset(0)] public MOUSEINPUT Mouse; [FieldOffset(0)] public KEYBDINPUT Keyboard; }
    [StructLayout(LayoutKind.Sequential)] private struct MOUSEINPUT { public int X, Y; public uint Data, Flags, Time; public UIntPtr Extra; }
    [StructLayout(LayoutKind.Sequential)] private struct KEYBDINPUT { public ushort Key, Scan; public uint Flags, Time; public UIntPtr Extra; }
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint process);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr handle);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr handle);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr handle, out RECT rectangle);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr handle);
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll")] private static extern uint SendInput(uint count, INPUT[] input, int size);
    [DllImport("user32.dll")] private static extern bool SetProcessDpiAwarenessContext(IntPtr context);
    [DllImport("kernel32.dll")] private static extern IntPtr OpenProcess(uint access, bool inherit, uint process);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll")] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll")] private static extern bool GetTokenInformation(IntPtr token, int information, ref int value, int size, out int needed);
}
