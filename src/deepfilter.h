// DeepFilterNet3 noise suppression, loaded at runtime from deepfilter.dll
// (the official DeepFilterNet LADSPA plugin build for Windows, v0.5.6).
#pragma once

#include <windows.h>
#include <string>

class DeepFilter {
public:
    DeepFilter() = default;
    ~DeepFilter();
    DeepFilter(const DeepFilter&) = delete;
    DeepFilter& operator=(const DeepFilter&) = delete;

    // True if deepfilter.dll sits next to ClearMic.exe.
    static bool Installed();

    // Loads the DLL and the model (takes a moment). On failure returns false and sets err.
    bool Init(std::wstring& err);

    // Processes one 10 ms frame (480 samples @ 48 kHz). Samples are in 16-bit scale
    // (same as RNNoise). Output is delayed by the plugin's internal buffering.
    void Process(const float* in, float* out);

    // Maximum noise attenuation in dB (100 = unlimited).
    void SetAttenuation(float db) { atten_ = db; }

private:
    HMODULE dll_ = nullptr;
    const void* desc_ = nullptr;
    void* handle_ = nullptr;
    float inBuf_[480] = {};
    float outBuf_[480] = {};
    // Control ports (values read by the plugin on every run()).
    // 50 dB instead of unlimited: frames the model judges as pure noise are turned down
    // by 50 dB rather than cut to exact zero, which avoids choppy gaps inside speech.
    float atten_ = 50.0f;
    float minThresh_ = -10.0f;
    float maxErbThresh_ = 30.0f;
    float maxDfThresh_ = 20.0f;
    float minBuffer_ = 0.0f;
    float pfBeta_ = 0.0f;
    float dummy_ = 0.0f;
};
