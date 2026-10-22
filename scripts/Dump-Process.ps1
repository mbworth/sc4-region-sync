# Writes a minidump (threads, stacks, handle names) of a running game instance. Run under
# 32-bit PowerShell (C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe) so the x86
# contexts are captured; a 64-bit host records only the WOW64 layer. Type 0x1004 =
# WithThreadInfo | WithHandleData (about 170 KB). Read with tools/minidump_stack.py.
param([int]$ProcessId, [string]$Out, [uint32]$Type = 0x1004)
"bitness=$([IntPtr]::Size)"
$sig = @"
using System; using System.Runtime.InteropServices;
public static class MD { [DllImport("dbghelp.dll", SetLastError=true)] public static extern bool MiniDumpWriteDump(IntPtr h, uint pid, IntPtr f, uint type, IntPtr a, IntPtr b, IntPtr c); }
"@
Add-Type -TypeDefinition $sig
$p = Get-Process -Id $ProcessId
$fs = [IO.File]::Create($Out)
$ok = [MD]::MiniDumpWriteDump($p.Handle, $ProcessId, $fs.SafeFileHandle.DangerousGetHandle(), $Type, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
$err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
$fs.Close()
"ok=$ok err=$err size=$((Get-Item $Out).Length)"
