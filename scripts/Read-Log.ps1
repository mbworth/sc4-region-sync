# Reads a file the game still has open for writing and prints it as base64 (make logs).
param([string]$Path)
$f = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
$m = New-Object IO.MemoryStream
$f.CopyTo($m); $f.Close()
[Convert]::ToBase64String($m.ToArray())
