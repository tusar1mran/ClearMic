// DeepFilterNet3 via its LADSPA plugin DLL.
//
// LADSPA is a small C plugin ABI. The plugin runs the model on its own worker
// thread and adds one 10 ms frame of buffering. Port layout of the mono plugin
// (DeepFilterNet ladspa/src/lib.rs, v0.5.6): Audio In, Audio Out, then control
// ports matched below by name.

#include "deepfilter.h"

#include <string.h>

namespace {

// LADSPA 1.1 ABI. On Windows `unsigned long` is 32-bit in both C and Rust (c_ulong).
typedef float LADSPA_Data;
typedef void* LADSPA_Handle;
typedef int LADSPA_PortDescriptor;

struct LADSPA_PortRangeHint {
    int HintDescriptor;
    LADSPA_Data LowerBound;
    LADSPA_Data UpperBound;
};

struct LADSPA_Descriptor {
    unsigned long UniqueID;
    const char* Label;
    int Properties;
    const char* Name;
    const char* Maker;
    const char* Copyright;
    unsigned long PortCount;
    const LADSPA_PortDescriptor* PortDescriptors;
    const char* const* PortNames;
    const LADSPA_PortRangeHint* PortRangeHints;
    void* ImplementationData;
    LADSPA_Handle (*instantiate)(const LADSPA_Descriptor*, unsigned long sampleRate);
    void (*connect_port)(LADSPA_Handle, unsigned long port, LADSPA_Data* data);
    void (*activate)(LADSPA_Handle);
    void (*run)(LADSPA_Handle, unsigned long sampleCount);
    void (*run_adding)(LADSPA_Handle, unsigned long sampleCount);
    void (*set_run_adding_gain)(LADSPA_Handle, LADSPA_Data gain);
    void (*deactivate)(LADSPA_Handle);
    void (*cleanup)(LADSPA_Handle);
};

typedef const LADSPA_Descriptor* (*DescriptorFn)(unsigned long index);

constexpr unsigned long kMonoId = 7843795;   // ID_MONO in the plugin source
constexpr int kPortInput = 0x1, kPortOutput = 0x2, kPortControl = 0x4, kPortAudio = 0x8;
constexpr int kFrame = 480;

std::wstring DllPath() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return p.substr(0, slash + 1) + L"deepfilter.dll";
}

} // namespace

bool DeepFilter::Installed() {
    return GetFileAttributesW(DllPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

DeepFilter::~DeepFilter() {
    // The plugin's worker thread has no stop signal, so the DLL is intentionally
    // left loaded; the process exit cleans it up.
    const LADSPA_Descriptor* d = static_cast<const LADSPA_Descriptor*>(desc_);
    if (d && handle_) {
        if (d->deactivate) d->deactivate(handle_);
    }
}

bool DeepFilter::Init(std::wstring& err) {
    // Full path only: never let Windows search other folders for this DLL.
    dll_ = LoadLibraryExW(DllPath().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll_) { err = L"deepfilter.dll not found or could not be loaded"; return false; }

    auto fn = reinterpret_cast<DescriptorFn>(GetProcAddress(dll_, "ladspa_descriptor"));
    if (!fn) { err = L"deepfilter.dll is not a LADSPA plugin"; return false; }

    const LADSPA_Descriptor* d = nullptr;
    for (unsigned long i = 0; i < 4; ++i) {
        const LADSPA_Descriptor* c = fn(i);
        if (!c) break;
        if (c->UniqueID == kMonoId) { d = c; break; }
    }
    if (!d || !d->instantiate || !d->connect_port || !d->run) {
        err = L"deepfilter.dll has no mono DeepFilterNet plugin";
        return false;
    }

    handle_ = d->instantiate(d, 48000);   // loads the model
    if (!handle_) { err = L"DeepFilterNet failed to start"; return false; }

    bool haveIn = false, haveOut = false;
    for (unsigned long p = 0; p < d->PortCount; ++p) {
        const int pd = d->PortDescriptors[p];
        const char* name = d->PortNames[p] ? d->PortNames[p] : "";
        float* target = &dummy_;
        if ((pd & kPortAudio) && (pd & kPortInput)) { target = inBuf_; haveIn = true; }
        else if ((pd & kPortAudio) && (pd & kPortOutput)) { target = outBuf_; haveOut = true; }
        else if (pd & kPortControl) {
            if (!strcmp(name, "Attenuation Limit (dB)")) target = &atten_;
            else if (!strcmp(name, "Min processing threshold (dB)")) target = &minThresh_;
            else if (!strcmp(name, "Max ERB processing threshold (dB)")) target = &maxErbThresh_;
            else if (!strcmp(name, "Max DF processing threshold (dB)")) target = &maxDfThresh_;
            else if (!strcmp(name, "Min Processing Buffer (frames)")) target = &minBuffer_;
            else if (!strcmp(name, "Post Filter Beta")) target = &pfBeta_;
        }
        d->connect_port(handle_, p, target);
    }
    if (!haveIn || !haveOut) { err = L"deepfilter.dll has unexpected ports"; return false; }

    if (d->activate) d->activate(handle_);
    desc_ = d;
    return true;
}

void DeepFilter::Process(const float* in, float* out) {
    const LADSPA_Descriptor* d = static_cast<const LADSPA_Descriptor*>(desc_);
    for (int i = 0; i < kFrame; ++i) inBuf_[i] = in[i] * (1.0f / 32768.0f);
    d->run(handle_, kFrame);
    for (int i = 0; i < kFrame; ++i) out[i] = outBuf_[i] * 32768.0f;
}
