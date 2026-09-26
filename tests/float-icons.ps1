# The component pictures the Float Ops package ships (tc.component.render V5).
#
# examples/float-ops/icons.cpp generates them from the Mod's *own* drawing code -
# it includes components.cpp and runs the real draw callback against a rasterising
# draw table - so this case checks the claim that makes that worth doing: the
# picture is the part's board appearance, at the board's own scale, rather than a
# blank, a placeholder or a differently laid out thumbnail.
#
# The numbers come from the layout the drawing code uses (components_internal.hpp):
# one cell is 26 px, the body is 4.92 x 2.93 cells and the pins sit on the
# +-3.0-cell lane with a radius of 0.33 cells.  The canvas is the 192x192 square
# the game draws into its 60x60 palette item (ITEM_SIZE in the pinned build).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$taskRepo = Split-Path $PSScriptRoot

# The staging directory is what the package is packed from, so this is the exact
# data the player receives.
& (Join-Path $taskRepo 'examples\float-ops\build.ps1') -NoPackage | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops build failed' }
$taskIcons = Join-Path $taskRepo 'build\float-ops-package\files\asset\float-ops-icons'
$taskFiles = @(Get-ChildItem -LiteralPath $taskIcons -Filter '*.png' -ErrorAction Stop |
  Sort-Object Name)
if ($taskFiles.Count -ne 22) {
  throw "The package must ship one picture per type; it has $($taskFiles.Count)"
}

$taskBodyColour = @(122, 79, 192)     # bodyFill()
$taskPinColour = @(214, 84, 84)       # pinColor()
$taskFailures = @()
$taskSummary = @()
foreach ($taskFile in $taskFiles) {
  # Every type's custom id starts with the Mod's own "F32" marker, so a picture
  # named after some other id is a picture of some other component.
  $taskId = 0L
  if (![long]::TryParse([IO.Path]::GetFileNameWithoutExtension($taskFile.Name), [ref]$taskId)) {
    $taskFailures += "$($taskFile.Name): the name is not a decimal custom id"
    continue
  }
  if ($taskId -lt 0x4633320000000000 -or $taskId -ge 0x4633330000000000) {
    $taskFailures += "$($taskFile.Name): not one of this Mod's custom ids"
    continue
  }
  $taskBitmap = [System.Drawing.Bitmap]::FromFile($taskFile.FullName)
  try {
    if ($taskBitmap.Width -ne 192 -or $taskBitmap.Height -ne 192) {
      $taskFailures += "$($taskFile.Name): $($taskBitmap.Width)x$($taskBitmap.Height), wanted 192x192"
      continue
    }
    if ($taskBitmap.GetPixel(0, 0).A -ne 0 -or $taskBitmap.GetPixel(191, 191).A -ne 0) {
      $taskFailures += "$($taskFile.Name): the canvas is not transparent"
      continue
    }
    $taskPurple = 0; $taskPins = 0; $taskInk = 0
    $taskMinX = 999; $taskMaxX = -1; $taskMinY = 999; $taskMaxY = -1
    $taskBodyMinX = 999; $taskBodyMaxX = -1; $taskBodyMinY = 999; $taskBodyMaxY = -1
    for ($taskY = 0; $taskY -lt 192; ++$taskY) {
      for ($taskX = 0; $taskX -lt 192; ++$taskX) {
        $taskPixel = $taskBitmap.GetPixel($taskX, $taskY)
        if ($taskPixel.A -lt 16) { continue }
        if ($taskX -lt $taskMinX) { $taskMinX = $taskX }
        if ($taskX -gt $taskMaxX) { $taskMaxX = $taskX }
        if ($taskY -lt $taskMinY) { $taskMinY = $taskY }
        if ($taskY -gt $taskMaxY) { $taskMaxY = $taskY }
        if ($taskPixel.R -eq $taskBodyColour[0] -and $taskPixel.G -eq $taskBodyColour[1] -and
            $taskPixel.B -eq $taskBodyColour[2]) {
          ++$taskPurple
          if ($taskX -lt $taskBodyMinX) { $taskBodyMinX = $taskX }
          if ($taskX -gt $taskBodyMaxX) { $taskBodyMaxX = $taskX }
          if ($taskY -lt $taskBodyMinY) { $taskBodyMinY = $taskY }
          if ($taskY -gt $taskBodyMaxY) { $taskBodyMaxY = $taskY }
          continue
        }
        if ($taskPixel.R -eq $taskPinColour[0] -and $taskPixel.G -eq $taskPinColour[1] -and
            $taskPixel.B -eq $taskPinColour[2]) { ++$taskPins; continue }
        if ($taskPixel.R -gt 200 -and $taskPixel.G -gt 200 -and $taskPixel.B -gt 200) {
          ++$taskInk
        }
      }
    }
    # The body is 4.92 x 2.93 cells = 128 x 76 px; the digits and the name take
    # some of it back, so the threshold is well under that.
    if ($taskPurple -lt 5000) {
      $taskFailures += "$($taskFile.Name): only $taskPurple body pixels; the purple face is missing"
    }
    # Four pin dots of radius 0.33 cells (8.6 px) are about 230 px each.
    if ($taskPins -lt 200) {
      $taskFailures += "$($taskFile.Name): only $taskPins pin pixels; the lane is missing"
    }
    if ($taskInk -lt 150) {
      $taskFailures += "$($taskFile.Name): only $taskInk white pixels; the text is missing"
    }
    # The face is the stock 4.92 x 2.93 cells = 128 x 76 px, centred on the canvas;
    # a type with more pins grows taller, so only the width is exact.
    $taskBodyWidth = $taskBodyMaxX - $taskBodyMinX + 1
    $taskBodyHeight = $taskBodyMaxY - $taskBodyMinY + 1
    if ($taskBodyWidth -lt 126 -or $taskBodyWidth -gt 132) {
      $taskFailures += "$($taskFile.Name): the face is $taskBodyWidth px wide, wanted 128 " +
        "(4.92 cells at 26 px/cell)"
    }
    if ($taskBodyHeight -lt 74) {
      $taskFailures += "$($taskFile.Name): the face is only $taskBodyHeight px tall"
    }
    $taskBodyCentre = 0.5 * ($taskBodyMinX + $taskBodyMaxX)
    if ([Math]::Abs($taskBodyCentre - 95.5) -gt 3) {
      $taskFailures += "$($taskFile.Name): the face is centred on $taskBodyCentre, not the canvas"
    }
    # The pins sit on the +-3.0-cell lane and stick out by their radius, so the
    # picture has to reach at least 3.2 cells = 83 px from the canvas centre.
    $taskReach = [Math]::Max(95.5 - $taskMinX, $taskMaxX - 95.5)
    if ($taskReach -lt 83) {
      $taskFailures += "$($taskFile.Name): the picture reaches only $taskReach px from the " +
        "centre; the pin lane is missing"
    }
    $taskSummary += [pscustomobject]@{ Id = [IO.Path]::GetFileNameWithoutExtension($taskFile.Name)
                                       Bytes = $taskFile.Length; Body = $taskPurple
                                       Pins = $taskPins; Ink = $taskInk
                                       Face = "$taskBodyWidth x $taskBodyHeight"
                                       Reach = [Math]::Round($taskReach) }
  } finally {
    $taskBitmap.Dispose()
  }
}

$taskSummary | Format-Table -AutoSize | Out-String | Write-Host
if ($taskFailures.Count) {
  throw ("The shipped component pictures are not the parts' board appearance: " +
         ($taskFailures -join '; '))
}

"PASS component pictures: $($taskFiles.Count) transparent 192x192 PNGs, each with the board's own " +
  "purple face, red pin lane and white text at 26 px per cell"
