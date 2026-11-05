// ClearMic audio engine: physical mic -> RNNoise -> virtual cable render endpoint.
#pragma once

#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

// Lists active endpoints. capture=true for microphones, false for playback devices.
std::vector<AudioDevice> ListDevices(bool capture);

// True if the name looks like the capture side of a virtual cable (would cause a feedback loop).
bool IsVirtualCableCapture(const std::wstring& name);

// Finds the render side of a known virtual cable ("CABLE Input (VB-Audio Virtual Cable)" etc).
// Returns an empty id if none is installed.
AudioDevice FindVirtualCableRender();

enum class EngineState { Stopped, Starting, Running, Error };

struct EngineStatus {
    EngineState state = EngineState::Stopped;
    std::wstring message;     // human-readable detail
    std::wstring inputName;
    std::wstring outputName;
};

class Engine {
public:
    Engine();
    ~Engine();

    // notifyWnd receives notifyMsg (posted) whenever status changes.
    void Start(HWND notifyWnd, UINT notifyMsg);
    void Stop();

    // Empty id = system default microphone / auto-detect virtual cable.
    void SetDevices(const std::wstring& inputId, const std::wstring& outputId);
    void SetDenoise(bool on);
    bool Denoise() const;

    // Voice focus: mutes speech that is quieter than the close-talking voice
    // (people further from the mic). 0 = off, 1 = normal, 2 = strict, 3 = very strict,
    // 4 = manual (only sound louder than the manual level passes).
    static constexpr int kFocusManual = 4;
    // 5 = voice match: only speech that sounds like the enrolled user passes.
    static constexpr int kFocusVoiceMatch = 5;
    void SetFocus(int level);
    int Focus() const;

    // Voice match (speaker recognition). Needs onnxruntime.dll + speaker.onnx next to the exe.
    static bool VoiceMatchInstalled();
    bool HasVoiceprint();
    void StartEnrollment();            // records ~20 s of the user's speech, then builds a voiceprint
    void CancelEnrollment();
    void ForgetVoice();                // deletes the stored voiceprint
    // -1 idle, 0..1 recording progress, 2 = saved, 3 = failed (see EnrollError()).
    float EnrollProgress() const { return enrollProgress_.load(); }
    std::wstring EnrollError();
    void SetMatchThreshold(float t) { matchThresh_.store(t); }
    float SuggestedThreshold() const { return suggested_.load(); }
    float MatchScore() const { return matchScore_.load(); }   // last similarity, -1 = none yet
    bool MatchIsUser() const { return isUser_.load(); }

    // Noise model: 0 = Standard (RNNoise), 1 = High quality (DeepFilterNet3, needs deepfilter.dll).
    void SetModel(int model);
    static bool HighQualityInstalled();

    // Voice enhancer (EQ + compressor + auto level + limiter) on the clean output.
    void SetEnhance(bool on) { InterlockedExchange(&enhance_, on ? 1 : 0); }

    // Manual gate level in dBFS (used when focus == kFocusManual).
    void SetManualLevel(float db);
    // Output volume, linear (1.0 = unchanged, 2.0 = +6 dB).
    void SetVolume(float gain);

    // Live meter for the UI (dBFS, per 10 ms frame).
    //   inDb:  raw microphone level
    //   outDb: level sent to the virtual mic (after noise removal / voice focus)
    //   gateDb/open: voice-focus threshold and state (meaningful when focus > 0)
    struct Meter { float inDb; float outDb; float gateDb; bool open; };
    Meter GetMeter() const;

    // Asks the engine to reopen devices (e.g. after a device was plugged in).
    void Restart();

    // Called from the Windows device-notification thread.
    void OnDeviceEvent(bool defaultCaptureChanged);

    EngineStatus Status();

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void Run();
    HRESULT RunSession(void* denoiser);
    void SetStatus(EngineState s, const std::wstring& msg,
                   const std::wstring& in = L"", const std::wstring& out = L"");

    HANDLE thread_ = nullptr;
    HANDLE wake_ = nullptr;           // auto-reset: restart or quit requested
    volatile LONG quit_ = 0;
    volatile LONG restart_ = 0;
    volatile LONG denoise_ = 1;
    volatile LONG focus_ = 1;
    volatile LONG model_ = 0;
    volatile LONG enhance_ = 0;
    volatile LONG dfFailed_ = 0;      // don't retry loading DeepFilterNet until the model is re-selected
    void* df_ = nullptr;              // DeepFilter*, owned by the engine thread
    std::wstring dfError_;            // engine thread only
    std::atomic<float> manualDb_{-35.0f};
    std::atomic<float> volume_{1.0f};
    std::atomic<float> meterIn_{-100.0f};
    std::atomic<float> meterOut_{-100.0f};
    std::atomic<float> meterGate_{-100.0f};
    std::atomic<bool> meterOpen_{false};

    // --- Voice match (speaker thread) ---
    static DWORD WINAPI SpeakerThreadProc(LPVOID self);
    void SpeakerRun();
    void PushSpeech(const float* s16k, size_t n, bool speech);   // audio thread
    bool LoadVoiceprint();
    bool SaveVoiceprint(const std::vector<float>& vp, float thresh);

    HANDLE spkThread_ = nullptr;
    HANDLE spkWake_ = nullptr;
    CRITICAL_SECTION spkLock_;
    std::vector<float> speech16k_;        // last few seconds of speech only (16 kHz), spkLock_
    size_t speechNew_ = 0;                // samples added since the last check, spkLock_
    std::vector<float> enroll16k_;        // enrollment speech, spkLock_
    bool enrolling_ = false;              // spkLock_
    std::vector<float> voiceprint_;       // spkLock_
    std::wstring enrollError_;            // spkLock_
    std::atomic<float> enrollProgress_{-1.0f};
    std::atomic<float> matchThresh_{0.45f};
    std::atomic<float> suggested_{0.45f};
    std::atomic<float> matchScore_{-1.0f};
    std::atomic<bool> isUser_{false};

    CRITICAL_SECTION lock_;
    std::wstring inputId_, outputId_;
    EngineStatus status_;

    HWND notifyWnd_ = nullptr;
    UINT notifyMsg_ = 0;
    void* notifier_ = nullptr;        // IMMNotificationClient registration
};
