using System.Diagnostics;
using System.Runtime.InteropServices;

internal static class FixtureToken
{
    internal static bool LaunchStandard()
    {
        if (!OpenProcessToken(Process.GetCurrentProcess().Handle, 8, out var current)) return false;
        try
        {
            var elevated = 0;
            if (!GetTokenInformation(current, 20, ref elevated, 4, out _) || elevated == 0) return false;
        }
        finally { CloseHandle(current); }
        if (!SaferCreateLevel(1, 0x20000, 1, out var level, IntPtr.Zero)) throw new InvalidOperationException("Fixture restricted token unavailable");
        try
        {
            if (!SaferComputeTokenFromLevel(level, IntPtr.Zero, out var token, 0, IntPtr.Zero)) throw new InvalidOperationException("Fixture token creation failed");
            try
            {
                if (!ConvertStringSidToSid("S-1-16-8192", out var sid)) throw new InvalidOperationException("Fixture integrity SID failed");
                try
                {
                    var label = new LABEL { Sid = sid, Attributes = 0x20 };
                    if (!SetTokenInformation(token, 25, ref label, Marshal.SizeOf<LABEL>() + (int)GetLengthSid(sid))) throw new InvalidOperationException("Fixture integrity adjustment failed");
                }
                finally { LocalFree(sid); }
                var startup = new STARTUPINFO { Size = Marshal.SizeOf<STARTUPINFO>(), Flags = 0x100, StdInput = GetStdHandle(-10), StdOutput = GetStdHandle(-11), StdError = GetStdHandle(-12), Desktop = "winsta0\\default" };
                var command = new System.Text.StringBuilder($"\"{Environment.ProcessPath}\" --fixture");
                if (!CreateProcessAsUser(token, null, command, IntPtr.Zero, IntPtr.Zero, true, 0, IntPtr.Zero, null, ref startup, out var process)) throw new InvalidOperationException($"Fixture launch failed: {Marshal.GetLastWin32Error()}");
                CloseHandle(process.Thread);
                try { WaitForSingleObject(process.Process, 60000); }
                finally { TerminateProcess(process.Process, 0); CloseHandle(process.Process); }
                return true;
            }
            finally { CloseHandle(token); }
        }
        finally { SaferCloseLevel(level); }
    }
    [StructLayout(LayoutKind.Sequential)] private struct LABEL { public IntPtr Sid; public uint Attributes; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct STARTUPINFO { public int Size; public string? Reserved; public string? Desktop; public string? Title; public uint X, Y, Width, Height, XChars, YChars, Fill, Flags; public ushort Show, ReservedSize; public IntPtr ReservedPointer, StdInput, StdOutput, StdError; }
    [StructLayout(LayoutKind.Sequential)] private struct PROCESS { public IntPtr Process, Thread; public uint ProcessId, ThreadId; }
    [DllImport("advapi32.dll")] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll")] private static extern bool GetTokenInformation(IntPtr token, int info, ref int value, int size, out int needed);
    [DllImport("advapi32.dll")] private static extern bool SaferCreateLevel(uint scope, uint level, uint flags, out IntPtr handle, IntPtr reserved);
    [DllImport("advapi32.dll")] private static extern bool SaferComputeTokenFromLevel(IntPtr level, IntPtr input, out IntPtr token, uint flags, IntPtr reserved);
    [DllImport("advapi32.dll")] private static extern bool SaferCloseLevel(IntPtr level);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] private static extern bool ConvertStringSidToSid(string text, out IntPtr sid);
    [DllImport("advapi32.dll")] private static extern bool SetTokenInformation(IntPtr token, int info, ref LABEL value, int size);
    [DllImport("advapi32.dll")] private static extern uint GetLengthSid(IntPtr sid);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool CreateProcessAsUser(IntPtr token, string? application, System.Text.StringBuilder command, IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles, uint flags, IntPtr environment, string? directory, ref STARTUPINFO startup, out PROCESS process);
    [DllImport("kernel32.dll")] private static extern IntPtr GetStdHandle(int kind);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern IntPtr LocalFree(IntPtr pointer);
    [DllImport("kernel32.dll")] private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll")] private static extern bool TerminateProcess(IntPtr handle, uint code);
}
