// ClearMic audio engine.
//
// Pipeline (single real-time thread):
//   WASAPI capture (shared, event-driven, 48 kHz mono float, engine resamples if needed)
//     -> 480-sample (10 ms) frames -> RNNoise -> WASAPI render to the virtual cable.
//
// The virtual cable (e.g. VB-CABLE) exposes the rendered audio as a microphone,
// which Meet / Zoom / Discord / OBS can select.

#include <initguid.h>
#include "engine.h"
#include "deepfilter.h"
#include "speaker.h"
#include "enhance.h"

#include <algorithm>

#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <math.h>
#include <string.h>
#include <wctype.h>

extern "C" {
#include "rnnoise.h"
}

namespace {

constexpr UINT32 kSampleRate = 48000;
constexpr UINT32 kFrame = 480;                 // RNNoise frame: 10 ms @ 48 kHz
constexpr REFERENCE_TIME kCaptureBuffer = 200000;   // 20 ms (100 ns units)
constexpr REFERENCE_TIME kRenderBuffer = 1000000;   // 100 ms of room; we normally keep ~20 ms queued
// Clock drift between the mic and the cable is corrected by adding or dropping a whole
// 10 ms frame. Doing that in the middle of speech is audible ("put put"), so:
//  - normal jitter is absorbed by a 20 ms target queue with a wide tolerance band;
//  - drift is corrected only on quiet frames, based on the averaged queue level;
//  - an immediate add/drop happens only at the hard limits (near-empty / over 80 ms).
constexpr UINT32 kTargetQueued = kFrame * 2;   // 20 ms
constexpr UINT32 kSoftLow = kFrame * 8 / 10;   // averaged queue under 8 ms: add silence when quiet
constexpr UINT32 kSoftHigh = kFrame * 4;       // averaged queue over 40 ms: drop a frame when quiet
constexpr UINT32 kHardLow = kFrame / 4;        // under 2.5 ms right now: add silence immediately
constexpr UINT32 kHardHigh = kFrame * 8;       // over 80 ms right now: drop immediately

// Voice-focus gate.
//
// A talker close to the mic is far louder than people a metre or more away
// (level falls ~6 dB per doubling of distance). The gate tracks the level of
// the loudest recent speech (assumed to be the user) and only passes frames
// that RNNoise classifies as speech AND that are within `margin` dB of it.
// It runs one frame (10 ms) behind so word onsets are not clipped.
class VoiceGate {
public:
    void Reset() { primed_ = false; hold_ = 0; gain_ = 1.0f; }

    // in: RNNoise output (16-bit scale). out: gated audio, delayed by one frame.
    // level 1..3 = automatic (relative to the learned voice level), 4 = manual (fixed manualDb),
    // 5 = voice match (speech passes only while the speaker thread says it is the user).
    void Process(const float* in, float vad, int level, float manualDb, bool userMatch, float* out,
                 float& voiceDb, float& gateDb, bool& open) {
        static const float kMargin[4] = {0.0f, 18.0f, 12.0f, 8.0f};
        const bool manual = level == 4;
        const bool match = level >= 5;
        constexpr int kHoldFrames = 25;            // keep open 250 ms after speech (word tails)

        if (!primed_) {
            memset(delayed_, 0, sizeof(delayed_));
            primed_ = true;
        }

        double sum = 0;
        for (UINT32 k = 0; k < kFrame; ++k) sum += double(in[k]) * in[k];
        const float db = 20.0f * log10f(float(sqrt(sum / kFrame)) / 32768.0f + 1e-9f);

        if (vad > 0.6f) {
            // Rise quickly to the user's voice; fall very slowly (~20 s of other
            // speech) so distant talkers don't become the reference.
            const float rate = db > speechDb_ ? 0.1f : 0.0005f;
            speechDb_ += rate * (db - speechDb_);
            if (speechDb_ < -60.0f) speechDb_ = -60.0f;
        }
        // Manual: pure level gate on the already-denoised signal (what you set is what you get).
        // Voice match: a loose level floor (30 dB below your voice) plus the speaker decision.
        const float threshold = manual ? manualDb : (match ? speechDb_ - 30.0f : speechDb_ - kMargin[level]);
        const bool pass = manual ? db > threshold
                                 : (vad > 0.5f && db > threshold && (!match || userMatch));

        // Voice match closes faster after you stop (less of the next speaker leaks through).
        if (pass) hold_ = match ? 15 : kHoldFrames;
        else if (hold_ > 0) --hold_;
        open = hold_ > 0;

        // Fast attack (one frame), ~6 dB per 10 ms release, smoothed per sample.
        float target = open ? 1.0f : gain_ * 0.5f;
        if (target < 0.001f) target = 0.0f;
        const float step = (target - gain_) / kFrame;
        float g = gain_;
        for (UINT32 k = 0; k < kFrame; ++k) {
            g += step;
            out[k] = delayed_[k] * g;
        }
        gain_ = target;
        memcpy(delayed_, in, sizeof(delayed_));

        voiceDb = db;
        gateDb = threshold;
    }

private:
    float delayed_[kFrame];
    bool primed_ = false;
    int hold_ = 0;
    float gain_ = 1.0f;
    float speechDb_ = -30.0f;
};

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T* operator->() const { return p; }
    T** put() { reset(); return &p; }
    void** putv() { reset(); return reinterpret_cast<void**>(&p); }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool Contains(const std::wstring& hay, const wchar_t* needle) {
    return hay.find(needle) != std::wstring::npos;
}

std::wstring DeviceName(IMMDevice* dev) {
    std::wstring name;
    Com<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            name = pv.pwszVal;
        PropVariantClear(&pv);
    }
    return name.empty() ? L"(unnamed device)" : name;
}

HRESULT CreateEnumerator(Com<IMMDeviceEnumerator>& en) {
    return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                            __uuidof(IMMDeviceEnumerator), en.putv());
}

std::wstring HrText(HRESULT hr) {
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED: return L"device was disconnected";
    case AUDCLNT_E_DEVICE_IN_USE: return L"device is in exclusive use by another app";
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return L"device format not supported";
    case E_ACCESSDENIED: return L"microphone access denied (check Windows privacy settings)";
    default: {
        wchar_t buf[32];
        wsprintfW(buf, L"error 0x%08X", (unsigned)hr);
        return buf;
    }
    }
}

// Receives device add/remove/default-change events so the engine can recover on its own.
class DeviceNotifier : public IMMNotificationClient {
public:
    explicit DeviceNotifier(Engine* e) : engine_(e) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { engine_->OnDeviceEvent(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { engine_->OnDeviceEvent(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eCapture && role == eConsole) engine_->OnDeviceEvent(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG ref_ = 1;
    Engine* engine_;
};

} // namespace

// ---------------------------------------------------------------------------
// Device helpers

std::vector<AudioDevice> ListDevices(bool capture) {
    std::vector<AudioDevice> out;
    Com<IMMDeviceEnumerator> en;
    if (FAILED(CreateEnumerator(en))) return out;
    Com<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, coll.put())))
        return out;
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Com<IMMDevice> dev;
        if (FAILED(coll->Item(i, dev.put()))) continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id))) continue;
        out.push_back({id, DeviceName(dev.p)});
        CoTaskMemFree(id);
    }
    return out;
}

bool IsVirtualCableCapture(const std::wstring& name) {
    std::wstring n = Lower(name);
    return Contains(n, L"vb-audio") && Contains(n, L"cable");
}

AudioDevice FindVirtualCableRender() {
    // Ranking: "CABLE Input (VB-Audio Virtual Cable)" > other "CABLE Input" (paid A/B cables)
    //          > any other VB-Audio cable endpoint (e.g. "CABLE In 16ch").
    AudioDevice best;
    int bestScore = 0;
    for (auto& d : ListDevices(false)) {
        std::wstring n = Lower(d.name);
        if (!(Contains(n, L"vb-audio") && Contains(n, L"cable"))) continue;
        int score = 1;
        if (n.rfind(L"cable input", 0) == 0) score = Contains(n, L"virtual cable") ? 3 : 2;
        if (score > bestScore) { best = d; bestScore = score; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Engine

Engine::Engine() {
    InitializeCriticalSection(&lock_);
    InitializeCriticalSection(&spkLock_);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    spkWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Engine::~Engine() {
    Stop();
    CloseHandle(wake_);
    CloseHandle(spkWake_);
    DeleteCriticalSection(&spkLock_);
    DeleteCriticalSection(&lock_);
}

void Engine::Start(HWND notifyWnd, UINT notifyMsg) {
    if (thread_) return;
    notifyWnd_ = notifyWnd;
    notifyMsg_ = notifyMsg;
    InterlockedExchange(&quit_, 0);
    if (VoiceMatchInstalled()) {
        LoadVoiceprint();
        spkThread_ = CreateThread(nullptr, 0, SpeakerThreadProc, this, 0, nullptr);
    }
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
}

void Engine::Stop() {
    if (!thread_) return;
    InterlockedExchange(&quit_, 1);
    SetEvent(wake_);
    WaitForSingleObject(thread_, 5000);
    CloseHandle(thread_);
    thread_ = nullptr;
    if (spkThread_) {
        SetEvent(spkWake_);
        WaitForSingleObject(spkThread_, 5000);
        CloseHandle(spkThread_);
        spkThread_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Voice match

namespace {
constexpr size_t kSpk16k = 16000;
constexpr size_t kEnrollSamples = 20 * kSpk16k;   // 20 s of speech
constexpr size_t kMatchWindow = kSpk16k;          // 1 s windows, both for enrollment and live checks
constexpr size_t kSpeechKeep = 3 * kSpk16k;

std::wstring VoiceprintPath() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(base) + L"\\ClearMic";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\voiceprint.bin";
}
} // namespace

bool Engine::VoiceMatchInstalled() { return SpeakerModel::Installed(); }

bool Engine::HasVoiceprint() {
    EnterCriticalSection(&spkLock_);
    bool has = !voiceprint_.empty();
    LeaveCriticalSection(&spkLock_);
    return has;
}

std::wstring Engine::EnrollError() {
    EnterCriticalSection(&spkLock_);
    std::wstring e = enrollError_;
    LeaveCriticalSection(&spkLock_);
    return e;
}

void Engine::StartEnrollment() {
    EnterCriticalSection(&spkLock_);
    enroll16k_.clear();
    enroll16k_.reserve(kEnrollSamples);
    enrolling_ = true;
    enrollError_.clear();
    LeaveCriticalSection(&spkLock_);
    enrollProgress_.store(0.0f);
}

void Engine::CancelEnrollment() {
    EnterCriticalSection(&spkLock_);
    enrolling_ = false;
    enroll16k_.clear();
    LeaveCriticalSection(&spkLock_);
    enrollProgress_.store(-1.0f);
}

void Engine::ForgetVoice() {
    EnterCriticalSection(&spkLock_);
    voiceprint_.clear();
    LeaveCriticalSection(&spkLock_);
    isUser_.store(false);
    matchScore_.store(-1.0f);
    std::wstring p = VoiceprintPath();
    if (!p.empty()) DeleteFileW(p.c_str());
}

// File: "CMV1", uint32 dim, float threshold, float[dim]. Stored only on this PC.
bool Engine::LoadVoiceprint() {
    std::wstring p = VoiceprintPath();
    HANDLE f = p.empty() ? INVALID_HANDLE_VALUE
                         : CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char magic[4] = {};
    UINT32 dim = 0;
    float thresh = 0;
    DWORD got = 0;
    bool ok = ReadFile(f, magic, 4, &got, nullptr) && got == 4 && !memcmp(magic, "CMV1", 4) &&
              ReadFile(f, &dim, 4, &got, nullptr) && got == 4 && dim > 0 && dim <= 4096 &&
              ReadFile(f, &thresh, 4, &got, nullptr) && got == 4;
    std::vector<float> vp(ok ? dim : 0);
    ok = ok && ReadFile(f, vp.data(), dim * 4, &got, nullptr) && got == dim * 4;
    CloseHandle(f);
    if (!ok) return false;
    EnterCriticalSection(&spkLock_);
    voiceprint_ = vp;
    LeaveCriticalSection(&spkLock_);
    suggested_.store(thresh);
    return true;
}

bool Engine::SaveVoiceprint(const std::vector<float>& vp, float thresh) {
    std::wstring p = VoiceprintPath();
    HANDLE f = p.empty() ? INVALID_HANDLE_VALUE
                         : CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    UINT32 dim = UINT32(vp.size());
    DWORD put = 0;
    bool ok = WriteFile(f, "CMV1", 4, &put, nullptr) && WriteFile(f, &dim, 4, &put, nullptr) &&
              WriteFile(f, &thresh, 4, &put, nullptr) && WriteFile(f, vp.data(), dim * 4, &put, nullptr);
    CloseHandle(f);
    return ok;
}

// Audio thread: keep recent speech for live checks, and collect enrollment speech.
void Engine::PushSpeech(const float* s, size_t n, bool speech) {
    if (!speech || n == 0) return;
    bool full = false;
    EnterCriticalSection(&spkLock_);
    speech16k_.insert(speech16k_.end(), s, s + n);
    if (speech16k_.size() > kSpeechKeep)
        speech16k_.erase(speech16k_.begin(), speech16k_.begin() + (speech16k_.size() - kSpeechKeep));
    speechNew_ += n;
    if (enrolling_) {
        enroll16k_.insert(enroll16k_.end(), s, s + n);
        float prog = float(enroll16k_.size()) / float(kEnrollSamples);
        enrollProgress_.store(prog < 0.999f ? prog : 0.999f);
        if (enroll16k_.size() >= kEnrollSamples) { enrolling_ = false; full = true; }
    }
    LeaveCriticalSection(&spkLock_);
    if (full) SetEvent(spkWake_);
}

DWORD WINAPI Engine::SpeakerThreadProc(LPVOID self) {
    static_cast<Engine*>(self)->SpeakerRun();
    return 0;
}

void Engine::SpeakerRun() {
    SpeakerModel* model = nullptr;
    bool loadFailed = false;
    std::wstring loadErr;
    std::vector<float> window, enrollAudio, vp;

    while (!quit_) {
        WaitForSingleObject(spkWake_, 100);
        if (quit_) break;

        bool doEnroll = false, doMatch = false;
        EnterCriticalSection(&spkLock_);
        if (!enrolling_ && enroll16k_.size() >= kEnrollSamples) {
            enrollAudio.swap(enroll16k_);
            enroll16k_.clear();
            doEnroll = true;
        }
        if (focus_ == kFocusVoiceMatch && !voiceprint_.empty() && speechNew_ >= kSpk16k / 10 &&
            speech16k_.size() >= kMatchWindow * 8 / 10) {
            const size_t len = std::min(kMatchWindow, speech16k_.size());
            window.assign(speech16k_.end() - len, speech16k_.end());
            vp = voiceprint_;
            speechNew_ = 0;
            doMatch = true;
        }
        LeaveCriticalSection(&spkLock_);
        if (!doEnroll && !doMatch) continue;

        if (!model && !loadFailed) {
            model = new SpeakerModel;
            if (!model->Load(L"", loadErr)) {
                loadFailed = true;
                delete model;
                model = nullptr;
            }
        }
        if (!model) {
            if (doEnroll) {
                EnterCriticalSection(&spkLock_);
                enrollError_ = L"Voice model failed to load: " + loadErr;
                LeaveCriticalSection(&spkLock_);
                enrollProgress_.store(3.0f);
            }
            continue;
        }

        if (doEnroll) {
            // Average the embeddings of 1 s chunks (same length as the live checks).
            std::vector<std::vector<float>> chunks;
            for (size_t pos = 0; pos + kMatchWindow <= enrollAudio.size(); pos += kMatchWindow / 2) {
                std::vector<float> e = model->Embed(enrollAudio.data() + pos, kMatchWindow);
                if (!e.empty()) chunks.push_back(std::move(e));
            }
            std::vector<float> mean;
            if (!chunks.empty()) {
                mean.assign(chunks[0].size(), 0.0f);
                for (auto& c : chunks)
                    for (size_t i = 0; i < c.size(); ++i) mean[i] += c[i];
                double norm = 0;
                for (float v : mean) norm += double(v) * v;
                norm = sqrt(norm);
                for (auto& v : mean) v = float(v / (norm > 0 ? norm : 1));
            }
            if (mean.empty()) {
                EnterCriticalSection(&spkLock_);
                enrollError_ = L"Could not analyse the recording";
                LeaveCriticalSection(&spkLock_);
                enrollProgress_.store(3.0f);
                continue;
            }
            // Threshold from the user's own scores: just below their 10th percentile.
            std::vector<float> self;
            for (auto& c : chunks) self.push_back(SpeakerModel::Cosine(c, mean));
            std::sort(self.begin(), self.end());
            float t = self[self.size() / 10] - 0.05f;
            t = t < 0.25f ? 0.25f : (t > 0.6f ? 0.6f : t);
            SaveVoiceprint(mean, t);
            EnterCriticalSection(&spkLock_);
            voiceprint_ = mean;
            speechNew_ = 0;
            LeaveCriticalSection(&spkLock_);
            suggested_.store(t);
            matchThresh_.store(t);
            enrollProgress_.store(2.0f);
            enrollAudio.clear();
        }

        if (doMatch) {
            // Two windows: 1 s (reliable) and the last 0.5 s (reacts fast when someone else
            // starts talking). Pass only if both agree it is the user.
            std::vector<float> e = model->Embed(window.data(), window.size());
            const size_t shortLen = kMatchWindow / 2;
            std::vector<float> es = window.size() > shortLen
                                        ? model->Embed(window.data() + window.size() - shortLen, shortLen)
                                        : std::vector<float>();
            if (!e.empty()) {
                const float score = SpeakerModel::Cosine(e, vp);
                const float shortScore = es.empty() ? score : SpeakerModel::Cosine(es, vp);
                const float t = matchThresh_.load();
                matchScore_.store(score < shortScore ? score : shortScore);
                // Margin 0.15 picked offline: other speakers 0/34 accepted, own voice barely affected.
                isUser_.store(score >= t && shortScore >= t - 0.15f);
            }
        }
    }
    delete model;
}

void Engine::SetDevices(const std::wstring& inputId, const std::wstring& outputId) {
    EnterCriticalSection(&lock_);
    inputId_ = inputId;
    outputId_ = outputId;
    LeaveCriticalSection(&lock_);
    Restart();
}

void Engine::SetDenoise(bool on) { InterlockedExchange(&denoise_, on ? 1 : 0); }
bool Engine::Denoise() const { return denoise_ != 0; }

void Engine::SetFocus(int level) {
    InterlockedExchange(&focus_, level < 0 ? 0 : (level > kFocusManual ? kFocusManual : level));
}
void Engine::SetModel(int model) {
    InterlockedExchange(&model_, model == 1 ? 1 : 0);
    InterlockedExchange(&dfFailed_, 0);
    Restart();   // load the model off the audio path, before streams start
}

bool Engine::HighQualityInstalled() { return DeepFilter::Installed(); }

void Engine::SetManualLevel(float db) { manualDb_.store(db < -90.0f ? -90.0f : (db > 0.0f ? 0.0f : db)); }
void Engine::SetVolume(float gain) { volume_.store(gain < 0.0f ? 0.0f : (gain > 4.0f ? 4.0f : gain)); }
int Engine::Focus() const { return focus_; }

Engine::Meter Engine::GetMeter() const {
    return {meterIn_.load(), meterOut_.load(), meterGate_.load(), meterOpen_.load()};
}

void Engine::Restart() {
    InterlockedExchange(&restart_, 1);
    SetEvent(wake_);
}

void Engine::OnDeviceEvent(bool defaultCaptureChanged) {
    bool restart = false;
    EnterCriticalSection(&lock_);
    if (status_.state != EngineState::Running)
        restart = true;                       // something was missing; try again now
    else if (defaultCaptureChanged && inputId_.empty())
        restart = true;                       // following the default mic
    LeaveCriticalSection(&lock_);
    if (restart) Restart();
}

EngineStatus Engine::Status() {
    EnterCriticalSection(&lock_);
    EngineStatus s = status_;
    LeaveCriticalSection(&lock_);
    return s;
}

void Engine::SetStatus(EngineState s, const std::wstring& msg,
                       const std::wstring& in, const std::wstring& out) {
    bool changed;
    EnterCriticalSection(&lock_);
    changed = status_.state != s || status_.message != msg ||
              status_.inputName != in || status_.outputName != out;
    status_.state = s;
    status_.message = msg;
    status_.inputName = in;
    status_.outputName = out;
    LeaveCriticalSection(&lock_);
    if (changed && notifyWnd_) PostMessageW(notifyWnd_, notifyMsg_, 0, 0);
}

DWORD WINAPI Engine::ThreadProc(LPVOID self) {
    static_cast<Engine*>(self)->Run();
    return 0;
}

void Engine::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    DenoiseState* st = rnnoise_create(nullptr);   // built-in model

    Com<IMMDeviceEnumerator> en;
    DeviceNotifier* notifier = nullptr;
    if (SUCCEEDED(CreateEnumerator(en))) {
        notifier = new DeviceNotifier(this);
        if (FAILED(en->RegisterEndpointNotificationCallback(notifier))) {
            notifier->Release();
            notifier = nullptr;
        }
    }

    while (!quit_) {
        InterlockedExchange(&restart_, 0);
        HRESULT hr = RunSession(st);
        if (quit_) break;
        if (FAILED(hr)) WaitForSingleObject(wake_, 2000);   // retry, or sooner on device events
    }

    if (notifier) {
        en->UnregisterEndpointNotificationCallback(notifier);
        notifier->Release();
    }
    en.reset();
    rnnoise_destroy(st);
    delete static_cast<DeepFilter*>(df_);
    df_ = nullptr;
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    SetStatus(EngineState::Stopped, L"Stopped");
    CoUninitialize();
}

HRESULT Engine::RunSession(void* denoiser) {
    DenoiseState* st = static_cast<DenoiseState*>(denoiser);

    // Load DeepFilterNet3 once, before audio starts (model load takes a moment).
    if (model_ == 1 && !df_ && !dfFailed_) {
        SetStatus(EngineState::Starting, L"Loading high-quality model...");
        DeepFilter* d = new DeepFilter;
        std::wstring err;
        if (d->Init(err)) {
            df_ = d;
        } else {
            dfError_ = err;
            InterlockedExchange(&dfFailed_, 1);
            delete d;
        }
    }

    std::wstring inId, outId;
    EnterCriticalSection(&lock_);
    inId = inputId_;
    outId = outputId_;
    LeaveCriticalSection(&lock_);

    Com<IMMDeviceEnumerator> en;
    HRESULT hr = CreateEnumerator(en);
    if (FAILED(hr)) { SetStatus(EngineState::Error, L"Audio system unavailable: " + HrText(hr)); return hr; }

    // --- Input (physical microphone) ---
    Com<IMMDevice> inDev;
    if (!inId.empty()) {
        DWORD state = 0;
        if (FAILED(en->GetDevice(inId.c_str(), inDev.put())) ||
            FAILED(inDev->GetState(&state)) || state != DEVICE_STATE_ACTIVE)
            inDev.reset();                    // selected mic unplugged: fall back to default
    }
    bool fellBack = !inId.empty() && !inDev;
    if (!inDev) {
        hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, inDev.put());
        if (FAILED(hr)) { SetStatus(EngineState::Error, L"No microphone found"); return hr; }
    }
    std::wstring inName = DeviceName(inDev.p);
    if (IsVirtualCableCapture(inName)) {
        SetStatus(EngineState::Error,
                  L"Input is the virtual cable itself. Pick your real microphone, or change the Windows default microphone.",
                  inName);
        return E_FAIL;
    }

    // --- Output (virtual cable playback side) ---
    Com<IMMDevice> outDev;
    if (!outId.empty()) {
        DWORD state = 0;
        if (FAILED(en->GetDevice(outId.c_str(), outDev.put())) ||
            FAILED(outDev->GetState(&state)) || state != DEVICE_STATE_ACTIVE)
            outDev.reset();
    }
    if (!outDev) {
        AudioDevice cable = FindVirtualCableRender();
        if (cable.id.empty()) {
            SetStatus(EngineState::Error, L"Virtual cable not installed. Install VB-CABLE (see tray menu).", inName);
            return E_FAIL;
        }
        hr = en->GetDevice(cable.id.c_str(), outDev.put());
        if (FAILED(hr)) { SetStatus(EngineState::Error, L"Cannot open virtual cable: " + HrText(hr), inName); return hr; }
    }
    std::wstring outName = DeviceName(outDev.p);

    // --- Open both streams as 48 kHz mono float; Windows converts to/from the device format ---
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = kSampleRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = kSampleRate * 4;
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    Com<IAudioClient> capClient, renClient;
    Com<IAudioCaptureClient> capture;
    Com<IAudioRenderClient> render;
    HANDLE capEvent = nullptr;
    UINT32 renBufFrames = 0;

    auto fail = [&](const wchar_t* what, HRESULT h) {
        SetStatus(EngineState::Error, std::wstring(what) + L": " + HrText(h), inName, outName);
        return h;
    };

    if (FAILED(hr = inDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, capClient.putv())))
        return fail(L"Microphone", hr);
    if (FAILED(hr = capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
                                          kCaptureBuffer, 0, &fmt, nullptr)))
        return fail(L"Microphone", hr);
    if (FAILED(hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, renClient.putv())))
        return fail(L"Virtual cable", hr);
    if (FAILED(hr = renClient->Initialize(AUDCLNT_SHAREMODE_SHARED, convert, kRenderBuffer, 0, &fmt, nullptr)))
        return fail(L"Virtual cable", hr);

    capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr = capClient->SetEventHandle(capEvent)) ||
        FAILED(hr = capClient->GetService(__uuidof(IAudioCaptureClient), capture.putv())) ||
        FAILED(hr = renClient->GetService(__uuidof(IAudioRenderClient), render.putv())) ||
        FAILED(hr = renClient->GetBufferSize(&renBufFrames))) {
        CloseHandle(capEvent);
        return fail(L"Audio setup", hr);
    }
    const UINT32 maxQueued = renBufFrames < kHardHigh ? renBufFrames : kHardHigh;

    auto writeSilence = [&](UINT32 n) -> HRESULT {
        BYTE* p = nullptr;
        HRESULT h = render->GetBuffer(n, &p);
        if (FAILED(h)) return h;
        return render->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
    };

    // Diagnostics (HKCU\Software\ClearMic\Diagnostics = 1): every 10 s, append counters to
    // %LOCALAPPDATA%\ClearMic\diag.log.
    struct DiagStats {
        int frames, drops, cushions, softDrops, softCushions, dfSlow, dfZero, flips;
        double dfMaxMs;
    } ds = {};
    DWORD diagFlag = 0, diagBytes = sizeof(diagFlag);
    const bool diag = RegGetValueW(HKEY_CURRENT_USER, L"Software\\ClearMic", L"Diagnostics", RRF_RT_REG_DWORD,
                                   nullptr, &diagFlag, &diagBytes) == ERROR_SUCCESS && diagFlag == 1;
    bool lastOpen = false;
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);

    float padAvg = float(kTargetQueued);
    auto writeFrame = [&](const float* src, bool quiet) -> HRESULT {
        UINT32 pad = 0;
        HRESULT h = renClient->GetCurrentPadding(&pad);
        if (FAILED(h)) return h;
        padAvg += 0.02f * (float(pad) - padAvg);   // ~0.5 s average
        const bool hardLow = pad < kHardLow;
        if ((hardLow || (quiet && padAvg < float(kSoftLow))) && pad + 2 * kFrame <= renBufFrames) {
            // Mic clock slower than the cable (or a scheduling hiccup): add 10 ms of silence.
            if (FAILED(h = writeSilence(kFrame))) return h;
            pad += kFrame;
            padAvg += float(kFrame);
            if (hardLow) ++ds.cushions; else ++ds.softCushions;
        }
        if (pad + kFrame > maxQueued) { ++ds.drops; return S_OK; }   // hard limit
        if (quiet && padAvg > float(kSoftHigh)) {                     // mic clock faster: drop a quiet frame
            padAvg -= float(kFrame);
            ++ds.softDrops;
            return S_OK;
        }
        BYTE* p = nullptr;
        if (FAILED(h = render->GetBuffer(kFrame, &p))) return h;
        memcpy(p, src, kFrame * sizeof(float));
        return render->ReleaseBuffer(kFrame, 0);
    };

    writeSilence(kTargetQueued);              // 20 ms initial cushion
    if (FAILED(hr = renClient->Start()) || FAILED(hr = capClient->Start())) {
        CloseHandle(capEvent);
        return fail(L"Start", hr);
    }

    std::wstring note = fellBack ? L"Running (selected mic missing, using default)" : L"Running";
    if (model_ == 1 && !df_) note += L". High-quality model unavailable (" + dfError_ + L"), using Standard.";
    SetStatus(EngineState::Running, note, inName, outName);

    float in[kFrame], out[kFrame], dfOut[kFrame], gated[kFrame], pcm[kFrame], d48[kFrame], enh[kFrame];
    UINT32 filled = 0;
    VoiceGate gate;
    VoiceEnhancer enhancer;
    Decimator3 dec;
    std::vector<float> d16;
    d16.reserve(kFrame);
    HANDLE waits[2] = {capEvent, wake_};

    while (true) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 500);
        if (w == WAIT_OBJECT_0 + 1) {
            if (quit_ || restart_) { hr = S_OK; break; }
            continue;
        }

        UINT32 packet = 0;
        while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const float* f = reinterpret_cast<const float*>(data);
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 i = 0; i < frames; ++i) {
                in[filled++] = silent ? 0.0f : f[i] * 32768.0f;   // RNNoise expects 16-bit scale
                if (filled == kFrame) {
                    filled = 0;
                    const float* result = in;
                    const int focus = focus_;
                    bool speechNow = false;   // user speech in this frame (drives the enhancer's auto level)
                    if (denoise_) {
                        // RNNoise always runs: it is cheap and supplies the voice-activity
                        // probability used by the voice-focus gate.
                        const float vad = rnnoise_process_frame(st, out, in);
                        speechNow = vad > 0.6f;
                        result = out;
                        if (model_ == 1 && df_) {
                            LARGE_INTEGER t0, t1;
                            QueryPerformanceCounter(&t0);
                            static_cast<DeepFilter*>(df_)->Process(in, dfOut);
                            QueryPerformanceCounter(&t1);
                            result = dfOut;
                            if (diag) {
                                const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / qpf.QuadPart;
                                if (ms > 9.0) ++ds.dfSlow;
                                if (ms > ds.dfMaxMs) ds.dfMaxMs = ms;
                                bool allZero = true;
                                for (UINT32 k = 0; k < kFrame && allZero; ++k) allZero = dfOut[k] == 0.0f;
                                if (allZero) ++ds.dfZero;   // plugin inserted silence (its underrun handling)
                            }
                        }
                        if (spkThread_) {
                            // Feed clean speech (16 kHz) to the speaker-recognition thread.
                            for (UINT32 k = 0; k < kFrame; ++k) d48[k] = result[k] * (1.0f / 32768.0f);
                            d16.clear();
                            dec.Process(d48, kFrame, d16);
                            PushSpeech(d16.data(), d16.size(), vad > 0.6f);
                        }
                        if (focus > 0) {
                            float vdb, gdb;
                            bool open;
                            gate.Process(result, vad, focus, manualDb_.load(), isUser_.load(), gated, vdb, gdb,
                                         open);
                            result = gated;
                            (void)vdb;
                            meterGate_.store(gdb);
                            meterOpen_.store(open);
                            if (open != lastOpen) ++ds.flips;
                            lastOpen = open;
                            speechNow = speechNow && open;   // don't level to voices the gate blocked
                        }
                    }
                    if (!denoise_ || focus == 0) gate.Reset();
                    double eIn = 0, eOut = 0;
                    float scale = volume_.load() * (1.0f / 32768.0f);
                    if (enhance_) {
                        for (UINT32 k = 0; k < kFrame; ++k) d48[k] = result[k] * (1.0f / 32768.0f);
                        enhancer.Process(d48, int(kFrame), speechNow);
                        for (UINT32 k = 0; k < kFrame; ++k) enh[k] = d48[k] * 32768.0f;
                        result = enh;
                    } else {
                        enhancer.Reset();
                    }
                    for (UINT32 k = 0; k < kFrame; ++k) {
                        eIn += double(in[k]) * in[k];
                        float v = result[k] * scale;
                        v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                        eOut += double(v) * v;
                        pcm[k] = v;
                    }
                    meterIn_.store(20.0f * log10f(float(sqrt(eIn / kFrame)) / 32768.0f + 1e-9f));
                    meterOut_.store(20.0f * log10f(float(sqrt(eOut / kFrame)) + 1e-9f));
                    // "Quiet" = below -50 dBFS (silence, or muted by the gate): safe to add/drop.
                    const bool quiet = eOut / kFrame < 1e-5;
                    HRESULT rh = writeFrame(pcm, quiet);
                    if (FAILED(rh)) hr = rh;
                    if (diag && ++ds.frames >= 1000) {
                        wchar_t base[MAX_PATH];
                        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
                        if (n > 0 && n < MAX_PATH) {
                            std::wstring path = std::wstring(base) + L"\\ClearMic\\diag.log";
                            char line[256];
                            SYSTEMTIME tm;
                            GetLocalTime(&tm);
                            int len = wsprintfA(line,
                                                "%02d:%02d:%02d frames=%d AUDIBLE drops=%d cushions=%d | quiet "
                                                "drops=%d cushions=%d | queue=%dms dfSlow=%d "
                                                "dfMax=%dms dfZero=%d gateFlips=%d match=%d%%\r\n",
                                                tm.wHour, tm.wMinute, tm.wSecond, ds.frames, ds.drops, ds.cushions,
                                                ds.softDrops, ds.softCushions, int(padAvg / 48),
                                                ds.dfSlow, int(ds.dfMaxMs), ds.dfZero, ds.flips,
                                                int(matchScore_.load() * 100));
                            HANDLE fh = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                                    OPEN_ALWAYS, 0, nullptr);
                            if (fh != INVALID_HANDLE_VALUE) {
                                DWORD put = 0;
                                WriteFile(fh, line, DWORD(len), &put, nullptr);
                                CloseHandle(fh);
                            }
                        }
                        ds = {};
                    }
                }
            }
            capture->ReleaseBuffer(frames);
            if (FAILED(hr)) break;
        }
        if (FAILED(hr)) {
            fail(hr == AUDCLNT_E_DEVICE_INVALIDATED ? L"Retrying" : L"Stream", hr);
            break;
        }
    }

    capClient->Stop();
    renClient->Stop();
    CloseHandle(capEvent);
    return hr;
}
