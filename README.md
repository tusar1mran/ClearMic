# ClearMic

ClearMic is a lightweight Windows app that cleans up your microphone in real time. It removes background noise (keyboard, fans, traffic, dishes), can block other people's voices, and can make your voice clearer. The cleaned audio goes to a virtual cable, which Google Meet, Zoom, Teams, Discord, OBS and browsers can pick as a microphone.

```
Your mic -> ClearMic (AI noise removal, voice focus, enhancer) -> "CABLE Input" ==VB-CABLE==> "CABLE Output" -> Meet / Zoom / OBS
```

## Download and install

1. Download `ClearMic-<version>-win64.zip` from [Releases](../../releases).
2. Follow `INSTALL.txt` inside the ZIP. In short:
   - install [VB-CABLE](https://vb-audio.com/Cable/) (free) and reboot
   - install the [Microsoft Visual C++ runtime](https://aka.ms/vs/17/release/vc_redist.x64.exe) if it's missing
   - run `ClearMic.exe`
   - in your meeting app, choose **CABLE Output (VB-Audio Virtual Cable)** as the microphone

Requirements: Windows 10/11 64-bit, and a CPU with AVX2 (Intel 2013+ / AMD Zen).
ClearMic is not code-signed yet. SmartScreen will warn you ("More info" > "Run anyway"), and PCs with Smart App Control turned on will block it.

## Build from source

No Visual Studio needed. A portable Zig toolchain is downloaded for you.

```powershell
pwsh -File fetch-deps.ps1   # downloads the toolchain, RNNoise, ONNX Runtime and models (SHA-256 verified)
.\build.bat                  # -> build\ClearMic.exe (+ deepfilter.dll, onnxruntime.dll, speaker.onnx)
pwsh -File package.ps1 -Version 1.0.0   # -> dist\ClearMic-1.0.0-win64.zip
```

| Path | Contents |
|---|---|
| `src/main.cpp` | Window and tray UI (native Win32) |
| `src/engine.cpp` | Audio engine: WASAPI capture, then noise removal, voice focus and the enhancer, then render to the virtual cable |
| `src/deepfilter.cpp` | DeepFilterNet3 via its LADSPA plugin DLL |
| `src/speaker.cpp` | Speaker recognition (Kaldi fbank front end + ONNX Runtime) |
| `src/enhance.cpp` | Voice enhancer (EQ, compressor, auto level, limiter) |
| `tools/` | Offline test tools for the speaker model and the enhancer |

Third-party licences are in [`licences/`](licences): RNNoise (BSD-3-Clause), DeepFilterNet (MIT / Apache-2.0), ONNX Runtime (MIT) and the 3D-Speaker CAM++ model (Apache-2.0). VB-CABLE is not bundled.

## Using the virtual mic

Keep your real microphone as the Windows default input, or set ClearMic's **Microphone** to your real mic explicitly. If ClearMic is set to "Windows default microphone" and the Windows default is "CABLE Output", ClearMic would be listening to itself; it detects this and warns you.

### AI model

| Model | Removes | Measured on i7-12700 (whole machine) |
|---|---|---|
| Standard - RNNoise | Steady noise (fan, hum, traffic) and most clicks between words | ~0.2% CPU, ~3 MB private RAM |
| High quality - DeepFilterNet3 | Also handles harder noise better (keyboard, dishes, doors, music) | ~2.1% CPU, ~45 MB private RAM |

High quality needs `deepfilter.dll` next to `ClearMic.exe` (`build.bat` copies it from `third_party/deepfilter/`). It is the official DeepFilterNet v0.5.6 LADSPA plugin for Windows (MIT / Apache-2.0, licence files in `third_party/deepfilter/`). It is 48 MB and adds roughly 10–20 ms of latency, so it exceeds the SRS limits of 25 MB RAM and 15 MB install size. Standard mode stays within them.

### Enhance voice (HD)

This is broadcast-style processing applied after noise removal:
- **High-pass at 80 Hz:** removes rumble and desk knocks
- **Cut of −2 dB at 300 Hz:** reduces muddiness
- **Presence boost of +3 dB at 3.5 kHz:** makes speech more intelligible
- **High shelf of +2 dB above 9 kHz:** adds "air"
- **Compressor:** 3:1 above −24 dBFS, so loud and soft words sit closer together
- **Auto level:** brings speech towards −20 dBFS, at most +12 dB. It only adjusts while you are speaking.
- **Peak limiter at −1 dBFS:** prevents distortion when you laugh or shout

Offline checks (`tools/enhtest.cpp`) confirmed the EQ curve matches the design and that a 0 dBFS input never exceeds −1 dBFS at the output. It adds no latency and uses well under 0.1% CPU. It cannot add detail the microphone didn't capture.

### My voice only (AI voice match)

1. Click **Teach my voice** and talk normally for about 20 seconds of speech (pauses don't count). Use the mic you will use in calls.
2. ClearMic saves a voiceprint and switches Voice focus to **My voice only - AI voice match**.
3. Every 0.1 s, the last 1 s of speech is compared with your voiceprint. Speech that doesn't match (other people) is muted. The meter shows the live match score.
4. **Match above** (the slider) sets how close a voice must be to yours. Raise it if other people still get through; lower it if your own words get cut.

How it works: the [3D-Speaker](https://github.com/modelscope/3D-Speaker) CAM++ speaker-embedding model (zh/en "advanced", ONNX export from sherpa-onnx), run with Microsoft ONNX Runtime 1.20.1 (MIT), loaded from `speaker.onnx` and `onnxruntime.dll` next to the exe. In an offline check on the sherpa-onnx test recordings, it accepted 0 of 34 one-second windows from other speakers and 19 of 22 from the enrolled speaker.

Limits:
- When you and someone else talk **at the same time**, the gate can only pass or mute both.
- After someone else stops and you start, your first ~0.5 s may be muted while the window fills with your voice.

Privacy: the voiceprint is biometric information. It is stored only on this PC at `%LOCALAPPDATA%\ClearMic\voiceprint.bin`, never uploaded, and **Forget** deletes it. Before shipping this to customers, get a privacy review: biometric templates are "sensitive information" under the Australian Privacy Act 1988, so collection needs consent and a clear notice. Confirm the speaker model's licence for commercial use first as well.

### Voice focus (block other people's voices)

RNNoise removes non-speech noise but keeps every human voice. **Voice focus** adds a gate on top. It learns the level of the loudest recent speech (you, close to the mic) and mutes speech that is quieter, such as people further away or a TV.

| Setting | Passes speech within | Use when |
|---|---|---|
| Off | (all voices) | You want everyone in the room heard |
| Normal (default) | 18 dB of your voice | Laptop or desk mic, others a few metres away |
| Strict | 12 dB | Others nearby; you sit close to the mic |
| Very strict | 8 dB | Headset or mouth-close mic in a noisy shared room |

The window shows the live level, the gate threshold (the black line on the "clean" bar), and whether the gate is open. If your own quiet words get cut, step down one level.

**Manual mode** lets you set the level yourself. Choose *Manual - I choose the level*, then drag the **Pass above** slider, or click and drag directly on the "clean" bar. Sound louder than the line passes; anything quieter is muted. Speak normally and set the line just below where your voice reaches.

**Volume** (0–200%) sets how loud the audio sent to the meeting is.

Limits: the gate can't separate two voices at the same moment. If someone talks while you talk, both are heard. A headset or dynamic mic held close to your mouth makes the gate work much better.

Tray icon colours: green = filtering, grey with a slash = pass-through, amber = needs attention (hover over it for the reason).

## Choose the microphone in each app

Select **CABLE Output (VB-Audio Virtual Cable)** as the microphone:

| App | Where |
|---|---|
| Google Meet | In a call, open the three-dot menu, then *Settings > Audio > Microphone* |
| Zoom | *Settings > Audio > Microphone*. Also set *Suppress background noise* to **Low** so the two noise filters don't fight |
| Microsoft Teams | *Settings > Devices > Microphone*. Set noise suppression to **Off** or **Low** |
| Discord | *User Settings > Voice & Video > Input Device*. Turn off Krisp noise suppression |
| OBS Studio | *Sources > + > Audio Input Capture > CABLE Output (VB-Audio Virtual Cable)*, or *Settings > Audio > Mic/Auxiliary Audio* |

## Troubleshooting

- **The app hears nothing:** turn on *Settings > Privacy & security > Microphone > Let desktop apps access your microphone*.
- **Amber icon "Virtual cable not installed":** install VB-CABLE (step 1) and reboot. ClearMic picks it up automatically.
- **You unplugged your headset:** ClearMic falls back to the Windows default mic and switches back when the headset returns. The virtual mic stays available to the meeting app throughout.
- **Voice sounds robotic or clipped:** RNNoise is aggressive on very quiet or distant voices. Move closer to the mic or raise its input level in Windows sound settings.

## Notes

- Settings are stored in `HKCU\Software\ClearMic`, and autostart in `HKCU\...\CurrentVersion\Run`. Nothing is sent over the network.
- The build targets CPUs with AVX2 (Intel 2013+, AMD Zen). For older CPUs, change `CPU=-mcpu=x86_64_v3` to `CPU=-mcpu=x86_64` in `build.bat`.
- Latency: about 10 ms for the RNNoise frame, plus up to 30 ms of buffering in ClearMic, plus VB-CABLE's own buffer. End-to-end latency has not been measured yet; it is likely above the SRS's 20 ms target.
- Licences: RNNoise is BSD-3-Clause (`third_party/rnnoise/COPYING`). VB-CABLE is donationware from VB-Audio and is not bundled.
