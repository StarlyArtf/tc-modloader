# Generates assets/tc-modloader.ico, the installer's own icon.
#
# The repository keeps the generated .ico checked in (like src/proxy.def and
# src/version.hpp) so a build never needs a drawing tool; this script is the
# recipe, and re-running it must reproduce the same pixels.  The picture is
# deliberately flat: a dark rounded tile with the loader's cyan chip and its
# six pins, which stays readable down to 16 px.
#
# ICO layout: a 6-byte ICONDIR, one 16-byte ICONDIRENTRY per size, then each
# image.  The images are PNG (Windows Vista and later accept PNG entries at any
# size, which keeps this script short enough to read).

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$taskRoot = Split-Path $PSScriptRoot
$taskOut = Join-Path $taskRoot 'assets\tc-modloader.ico'
$taskSizes = @(16, 24, 32, 48, 64, 128, 256)

$taskTile = [System.Drawing.Color]::FromArgb(255, 11, 20, 32)
$taskChip = [System.Drawing.Color]::FromArgb(255, 50, 190, 239)
$taskPin = [System.Drawing.Color]::FromArgb(255, 132, 222, 245)

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPng([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)

    # Scale the drawing in 32-unit space so every size is the same picture.
    $unit = $size / 32.0
    $tile = New-RoundedPath (0.5 * $unit) (0.5 * $unit) (31 * $unit) (31 * $unit) (6 * $unit)
    $tileBrush = New-Object System.Drawing.SolidBrush $taskTile
    $g.FillPath($tileBrush, $tile)

    $chipRect = New-RoundedPath (8 * $unit) (8 * $unit) (16 * $unit) (16 * $unit) (2.5 * $unit)
    $pen = New-Object System.Drawing.Pen $taskChip, ([single](2.2 * $unit))
    $pen.Alignment = [System.Drawing.Drawing2D.PenAlignment]::Inset
    if ($size -le 16) { $pen.Width = [single](3.0 * $unit) }
    $g.DrawPath($pen, $chipRect)

    # Three pins on the left and right of the chip, one short notch in the middle.
    $pinPen = New-Object System.Drawing.Pen $taskPin, ([single](1.8 * $unit))
    $pinPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pinPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    foreach ($offset in 11, 16, 21) {
        $g.DrawLine($pinPen, [single](4.5 * $unit), [single]($offset * $unit), [single](8 * $unit), [single]($offset * $unit))
        $g.DrawLine($pinPen, [single](24 * $unit), [single]($offset * $unit), [single](27.5 * $unit), [single]($offset * $unit))
    }
    $g.DrawLine($pinPen, [single](13 * $unit), [single](16 * $unit), [single](19 * $unit), [single](16 * $unit))

    $stream = New-Object System.IO.MemoryStream
    $bmp.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $bmp.Dispose()
    $tileBrush.Dispose()
    $pen.Dispose()
    $pinPen.Dispose()
    $tile.Dispose()
    $chipRect.Dispose()
    $bytes = $stream.ToArray()
    $stream.Dispose()
    return $bytes
}

$taskImages = @()
foreach ($size in $taskSizes) { $taskImages += ,(New-IconPng $size) }

New-Item -ItemType Directory -Force (Split-Path $taskOut) | Out-Null
$file = [System.IO.File]::Create($taskOut)
$writer = New-Object System.IO.BinaryWriter $file
$writer.Write([uint16]0)                      # reserved
$writer.Write([uint16]1)                      # type: icon
$writer.Write([uint16]$taskSizes.Count)
$offset = 6 + 16 * $taskSizes.Count
for ($index = 0; $index -lt $taskSizes.Count; $index++) {
    $size = $taskSizes[$index]
    $bytes = $taskImages[$index]
    $writer.Write([byte]($(if ($size -ge 256) { 0 } else { $size })))
    $writer.Write([byte]($(if ($size -ge 256) { 0 } else { $size })))
    $writer.Write([byte]0)                    # palette entries
    $writer.Write([byte]0)                    # reserved
    $writer.Write([uint16]1)                  # colour planes
    $writer.Write([uint16]32)                 # bits per pixel
    $writer.Write([uint32]$bytes.Length)
    $writer.Write([uint32]$offset)
    $offset += $bytes.Length
}
foreach ($bytes in $taskImages) {
    # The three-argument overload, because the single-argument Write() resolves
    # to the scalar overload for a byte[] under Windows PowerShell.
    $writer.Write([byte[]]$bytes, 0, $bytes.Length)
}
$writer.Flush()
$writer.Dispose()
$file.Dispose()
Write-Output ("wrote {0} ({1} sizes, {2} bytes)" -f $taskOut, $taskSizes.Count, (Get-Item $taskOut).Length)
