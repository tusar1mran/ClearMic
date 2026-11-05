# Builds a shareable ZIP: dist\ClearMic-<version>-win64.zip
# Run from the project folder:  pwsh -File package.ps1 [-Version 1.0.0]
param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$name = "ClearMic-$Version-win64"
$stage = Join-Path $root "dist\$name"
$zip = Join-Path $root "dist\$name.zip"

# Fresh build into the staging folder (the running app may lock build\ClearMic.exe).
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, "$stage\licences" | Out-Null
& cmd /c "`"$root\build.bat`" build\ClearMic-dist.exe" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Move-Item -Force "$root\build\ClearMic-dist.exe" "$stage\ClearMic.exe"

# Optional components (ClearMic falls back to the Standard model / no voice match without them).
Copy-Item "$root\third_party\deepfilter\deep_filter_ladspa.dll" "$stage\deepfilter.dll"
Copy-Item "$root\third_party\onnxruntime\onnxruntime-win-x64-1.20.1\lib\onnxruntime.dll" "$stage\onnxruntime.dll"
Copy-Item "$root\third_party\speaker\3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx" "$stage\speaker.onnx"

# Licences and notices for everything bundled.
Copy-Item "$root\licences\*" "$stage\licences\"
Copy-Item "$root\INSTALL.txt" "$stage\INSTALL.txt"

if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-FileHash -Algorithm SHA256 $zip | ForEach-Object { "$($_.Hash.ToLower())  $name.zip" } |
    Set-Content "$zip.sha256"
"Created $zip  ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)
