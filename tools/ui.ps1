param([string]$Action='capture',[int]$X=0,[int]$Y=0,[string]$Output='D:\p\tc-modloader\build\game.png',[int]$GameProcessId=0)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;using System.Runtime.InteropServices;
public class TCWindow {
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int L,T,R,B;}
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,IntPtr p);
 [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int command);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,UIntPtr extra);
 [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[TCWindow]::SetProcessDPIAware() | Out-Null
$taskProcess = if($GameProcessId){Get-Process -Id $GameProcessId -ErrorAction Stop}else{Get-Process -Name 'Turing Complete' -ErrorAction Stop | Where-Object { $_.Path -eq 'D:\p\Turing Complete.exe' } | Select-Object -First 1}
if(!$taskProcess){throw 'No matching game process'}
$taskHandle = $taskProcess.MainWindowHandle
if ($Action -eq 'close') { [TCWindow]::PostMessage($taskHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null; exit }
$taskCurrentThread=[TCWindow]::GetCurrentThreadId()
$taskForegroundThread=[TCWindow]::GetWindowThreadProcessId([TCWindow]::GetForegroundWindow(),[IntPtr]::Zero)
[TCWindow]::AttachThreadInput($taskCurrentThread,$taskForegroundThread,$true) | Out-Null
try { [TCWindow]::ShowWindow($taskHandle,9) | Out-Null; [TCWindow]::SetForegroundWindow($taskHandle) | Out-Null }
finally { [TCWindow]::AttachThreadInput($taskCurrentThread,$taskForegroundThread,$false) | Out-Null }
$taskRect = New-Object TCWindow+Rect
[TCWindow]::GetWindowRect($taskHandle,[ref]$taskRect) | Out-Null
if ($Action -eq 'click') {
 if([TCWindow]::GetForegroundWindow() -ne $taskHandle){throw 'Game is not foreground; refusing to send a click to another app'}
 [TCWindow]::SetCursorPos($taskRect.L+$X,$taskRect.T+$Y) | Out-Null
 Start-Sleep -Milliseconds 120
 [TCWindow]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
 Start-Sleep -Milliseconds 120
 [TCWindow]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
}
Start-Sleep -Milliseconds 350
$taskBitmap = New-Object System.Drawing.Bitmap ($taskRect.R-$taskRect.L),($taskRect.B-$taskRect.T)
$taskGraphics = [System.Drawing.Graphics]::FromImage($taskBitmap)
$taskGraphics.CopyFromScreen($taskRect.L,$taskRect.T,0,0,$taskBitmap.Size)
$taskBitmap.Save($Output,[System.Drawing.Imaging.ImageFormat]::Png)
$taskGraphics.Dispose(); $taskBitmap.Dispose()
Write-Output $Output
