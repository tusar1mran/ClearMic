// Speaker recognition ("is this the enrolled user?") using a WeSpeaker CAM++
// embedding model run through ONNX Runtime (loaded at runtime from
// onnxruntime.dll next to ClearMic.exe).
#pragma once

#include <windows.h>
#include <string>
#include <vector>

class SpeakerModel {
public:
    SpeakerModel() = default;
    ~SpeakerModel();
    SpeakerModel(const SpeakerModel&) = delete;
    SpeakerModel& operator=(const SpeakerModel&) = delete;

    // True if onnxruntime.dll and the model file sit next to ClearMic.exe.
    static bool Installed();

    // dir: folder holding onnxruntime.dll and speaker.onnx (empty = exe folder).
    bool Load(const std::wstring& dir, std::wstring& err);

    // Embedding of 16 kHz mono audio in [-1, 1]. L2-normalised. Empty on failure.
    // Needs at least ~0.5 s of audio.
    std::vector<float> Embed(const float* samples16k, size_t n);

    static float Cosine(const std::vector<float>& a, const std::vector<float>& b);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// Kaldi-compatible 80-bin log-mel filterbank (16 kHz, 25 ms / 10 ms, Povey window,
// pre-emphasis 0.97, DC removal, no dither, snip_edges = false), matching the
// feature settings sherpa-onnx uses for these models. Input in 16-bit scale.
// Returns frames x 80, row-major.
std::vector<float> ComputeFbank(const float* samples, size_t n, int& numFrames);

// Streaming 48 kHz -> 16 kHz decimator (low-pass FIR then keep every 3rd sample).
class Decimator3 {
public:
    Decimator3();
    // Appends floor((carry + n) / 3) samples to out.
    void Process(const float* in, size_t n, std::vector<float>& out);

private:
    std::vector<float> taps_;
    std::vector<float> hist_;
    size_t phase_ = 0;
};
