@echo off
rem Builds build\ClearMic.exe using the portable Zig toolchain in third_party\zig.
setlocal
cd /d "%~dp0"

set ZIG=third_party\zig\zig.exe
set RN=third_party\rnnoise
set ORT=third_party\onnxruntime\onnxruntime-win-x64-1.20.1
set SPK=third_party\speaker\3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx
set TARGET=-target x86_64-windows-gnu
rem x86_64_v3 = AVX2/FMA (Intel Haswell 2013+, AMD Zen+). Use x86_64 for older CPUs.
set CPU=-mcpu=x86_64_v3
set OPT=-O2
set OUT=build\ClearMic.exe
if not "%~1"=="" set OUT=%~1

if not exist "%ZIG%" (
  echo Zig not found at %ZIG%
  exit /b 1
)
if not exist build mkdir build

echo [1/2] RNNoise
for %%f in (denoise rnn pitch kiss_fft celt_lpc nnet nnet_default parse_lpcnet_weights rnnoise_data rnnoise_tables) do (
  "%ZIG%" cc %TARGET% %CPU% %OPT% -c %RN%\src\%%f.c -I%RN%\include -I%RN%\src -DRNNOISE_BUILD -D_USE_MATH_DEFINES -w -o build\%%f.o || exit /b 1
)

echo [2/2] ClearMic
"%ZIG%" c++ %TARGET% %CPU% %OPT% -std=c++17 -DUNICODE -D_UNICODE -DNOMINMAX -U_WIN32_WINNT -UWINVER -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  -I%RN%\include -I%ORT%\include src\main.cpp src\engine.cpp src\deepfilter.cpp src\speaker.cpp src\enhance.cpp src\app.rc ^
  build\denoise.o build\rnn.o build\pitch.o build\kiss_fft.o build\celt_lpc.o build\nnet.o ^
  build\nnet_default.o build\parse_lpcnet_weights.o build\rnnoise_data.o build\rnnoise_tables.o ^
  -lole32 -luser32 -lgdi32 -lshell32 -ladvapi32 -lavrt -lcomctl32 ^
  -Wl,--subsystem,windows -s -o %OUT% || exit /b 1

rem Optional high-quality model: DeepFilterNet3 LADSPA plugin (MIT / Apache-2.0).
rem Skipped if already present (the running app keeps it locked).
if exist third_party\deepfilter\deep_filter_ladspa.dll if not exist build\deepfilter.dll (
  copy /y third_party\deepfilter\deep_filter_ladspa.dll build\deepfilter.dll >nul
)

rem Optional voice match: ONNX Runtime (MIT) + 3D-Speaker CAM++ speaker model.
if exist %ORT%\lib\onnxruntime.dll if not exist build\onnxruntime.dll (
  copy /y %ORT%\lib\onnxruntime.dll build\onnxruntime.dll >nul
)
if exist %SPK% if not exist build\speaker.onnx (
  copy /y %SPK% build\speaker.onnx >nul
)

echo Built %OUT%
