# Downloads the build toolchain and third-party components into third_party\,
# verifying each file's SHA-256. Safe to re-run: anything already present is skipped.
# Usage (from the project folder):  pwsh -File fetch-deps.ps1
$ErrorActionPreference = "Stop"
$tp = Join-Path $PSScriptRoot "third_party"
$dl = Join-Path $tp "dl"
New-Item -ItemType Directory -Force $dl | Out-Null

$deps = @(
    @{ Name = "zig.zip"; Sha = "d859994725ef9402381e557c60bb57497215682e355204d754ee3df75ee3c158"
       Url = "https://ziglang.org/download/0.13.0/zig-windows-x86_64-0.13.0.zip" },
    @{ Name = "rnnoise.tar.gz"; Sha = "90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37"
       Url = "https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz" },
    @{ Name = "ort.zip"; Sha = "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581"
       Url = "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip" },
    @{ Name = "deep_filter_ladspa.dll"; Sha = "f97e34275eb884b6076a96d261683d2a43c6b2d97ddf9e346bc06f7c8057eb04"
       Url = "https://github.com/Rikorose/DeepFilterNet/releases/download/v0.5.6/deep_filter_ladspa-0.5.6-x86_64-pc-windows-msvc.dll" },
    @{ Name = "speaker.onnx"; Sha = "aa3cfc16963a10586a9393f5035d6d6b57e98d358b347f80c2a30bf4f00ceba2"
       Url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx" }
)

foreach ($d in $deps) {
    $file = Join-Path $dl $d.Name
    if (-not (Test-Path $file) -or (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower() -ne $d.Sha) {
        "Downloading $($d.Name)..."
        Invoke-WebRequest -Uri $d.Url -OutFile $file -UseBasicParsing
    }
    $hash = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
    if ($hash -ne $d.Sha) {
        Remove-Item -Force $file
        throw "Checksum mismatch for $($d.Name) (got $hash). Download removed."
    }
    "OK  $($d.Name)"
}

# Extract / place (skipped if already there)
if (-not (Test-Path "$tp\zig\zig.exe")) {
    Expand-Archive -Force "$dl\zig.zip" $tp
    Rename-Item "$tp\zig-windows-x86_64-0.13.0" "zig"
}
if (-not (Test-Path "$tp\rnnoise\src\denoise.c")) {
    tar -xzf "$dl\rnnoise.tar.gz" -C $tp
    Rename-Item "$tp\rnnoise-0.2" "rnnoise"
}
if (-not (Test-Path "$tp\onnxruntime\onnxruntime-win-x64-1.20.1\lib\onnxruntime.dll")) {
    New-Item -ItemType Directory -Force "$tp\onnxruntime" | Out-Null
    Expand-Archive -Force "$dl\ort.zip" "$tp\onnxruntime"
}
New-Item -ItemType Directory -Force "$tp\deepfilter", "$tp\speaker" | Out-Null
Copy-Item "$dl\deep_filter_ladspa.dll" "$tp\deepfilter\deep_filter_ladspa.dll" -Force
Copy-Item "$dl\speaker.onnx" "$tp\speaker\3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx" -Force
"All dependencies ready. Next: build.bat"
