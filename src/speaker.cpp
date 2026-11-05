// Speaker embeddings: Kaldi fbank front end + ONNX Runtime (C API, loaded at runtime).

#include "speaker.h"

#include <float.h>
#include <math.h>
#include <string.h>
#include <complex>

// MSVC SAL annotation used by the ONNX Runtime header; MinGW's specstrings.h lacks it.
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#include "onnxruntime_c_api.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Fbank

constexpr int kSr = 16000;
constexpr int kWin = 400;      // 25 ms
constexpr int kShift = 160;    // 10 ms
constexpr int kFft = 512;
constexpr int kBins = 80;

double Mel(double f) { return 1127.0 * log(1.0 + f / 700.0); }

struct FbankTables {
    float window[kWin];
    // Sparse mel weights: for each bin, first FFT index and weights.
    int first[kBins];
    std::vector<float> weights[kBins];
    std::complex<double> twiddle[kFft / 2];
    int bitrev[kFft];

    FbankTables() {
        for (int i = 0; i < kWin; ++i)
            window[i] = float(pow(0.5 - 0.5 * cos(2.0 * kPi * i / (kWin - 1)), 0.85));   // Povey

        const int numFftBins = kFft / 2;
        const double binWidth = double(kSr) / kFft;
        const double lowFreq = 20.0, highFreq = kSr / 2.0 - 400.0;   // high_freq = -400
        const double melLow = Mel(lowFreq), melHigh = Mel(highFreq);
        const double delta = (melHigh - melLow) / (kBins + 1);
        for (int b = 0; b < kBins; ++b) {
            const double left = melLow + b * delta, center = left + delta, right = center + delta;
            first[b] = -1;
            for (int i = 0; i < numFftBins; ++i) {
                const double mel = Mel(binWidth * i);
                if (mel > left && mel < right) {
                    const double w = mel <= center ? (mel - left) / (center - left) : (right - mel) / (right - center);
                    if (first[b] < 0) first[b] = i;
                    weights[b].resize(i - first[b] + 1, 0.0f);
                    weights[b][i - first[b]] = float(w);
                }
            }
            if (first[b] < 0) first[b] = 0;
        }

        for (int k = 0; k < kFft / 2; ++k) twiddle[k] = std::polar(1.0, -2.0 * kPi * k / kFft);
        int bits = 0;
        while ((1 << bits) < kFft) ++bits;
        for (int i = 0; i < kFft; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            bitrev[i] = r;
        }
    }

    void Fft(std::complex<double>* x) const {
        for (int i = 0; i < kFft; ++i)
            if (i < bitrev[i]) std::swap(x[i], x[bitrev[i]]);
        for (int len = 2; len <= kFft; len <<= 1) {
            const int half = len / 2, step = kFft / len;
            for (int s = 0; s < kFft; s += len)
                for (int k = 0; k < half; ++k) {
                    std::complex<double> t = twiddle[k * step] * x[s + k + half];
                    x[s + k + half] = x[s + k] - t;
                    x[s + k] += t;
                }
        }
    }
};

const FbankTables& Tables() {
    static FbankTables t;
    return t;
}

std::wstring Widen(const char* s) {
    std::wstring w;
    while (s && *s) w.push_back(wchar_t(static_cast<unsigned char>(*s++)));
    return w;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

} // namespace

std::vector<float> ComputeFbank(const float* x, size_t n, int& numFrames) {
    const FbankTables& t = Tables();
    numFrames = n == 0 ? 0 : int((n + kShift / 2) / kShift);   // snip_edges = false
    std::vector<float> feats(size_t(numFrames) * kBins);
    double frame[kWin];
    std::complex<double> spec[kFft];
    double power[kFft / 2 + 1];

    for (int f = 0; f < numFrames; ++f) {
        // Window centred on the frame, reflecting at the signal edges.
        const long long start = (long long)f * kShift + kShift / 2 - kWin / 2;
        double mean = 0;
        for (int i = 0; i < kWin; ++i) {
            long long s = start + i;
            while (s < 0 || s >= (long long)n) s = s < 0 ? -s - 1 : 2 * (long long)n - 1 - s;
            frame[i] = x[s];
            mean += frame[i];
        }
        mean /= kWin;
        for (int i = 0; i < kWin; ++i) frame[i] -= mean;                  // remove DC
        for (int i = kWin - 1; i > 0; --i) frame[i] -= 0.97 * frame[i - 1];   // pre-emphasis
        frame[0] -= 0.97 * frame[0];
        for (int i = 0; i < kFft; ++i) spec[i] = i < kWin ? frame[i] * t.window[i] : 0.0;
        t.Fft(spec);
        for (int k = 0; k <= kFft / 2; ++k) power[k] = std::norm(spec[k]);

        float* out = &feats[size_t(f) * kBins];
        for (int b = 0; b < kBins; ++b) {
            double e = 0;
            const auto& w = t.weights[b];
            for (size_t i = 0; i < w.size(); ++i) e += w[i] * power[t.first[b] + i];
            out[b] = float(log(e > FLT_EPSILON ? e : FLT_EPSILON));
        }
    }
    return feats;
}

// ---------------------------------------------------------------------------
// 48 kHz -> 16 kHz

Decimator3::Decimator3() {
    // 95-tap Blackman-windowed sinc low-pass, cutoff 7.2 kHz at 48 kHz.
    const int N = 95;
    const double fc = 7200.0 / 48000.0;
    taps_.resize(N);
    double sum = 0;
    for (int i = 0; i < N; ++i) {
        const double m = i - (N - 1) / 2.0;
        const double sinc = m == 0 ? 2 * fc : sin(2 * kPi * fc * m) / (kPi * m);
        const double w = 0.42 - 0.5 * cos(2 * kPi * i / (N - 1)) + 0.08 * cos(4 * kPi * i / (N - 1));
        taps_[i] = float(sinc * w);
        sum += taps_[i];
    }
    for (auto& v : taps_) v = float(v / sum);
}

void Decimator3::Process(const float* in, size_t n, std::vector<float>& out) {
    hist_.insert(hist_.end(), in, in + n);
    const size_t T = taps_.size();
    size_t pos = phase_;
    while (pos + T <= hist_.size()) {
        float acc = 0;
        for (size_t k = 0; k < T; ++k) acc += taps_[k] * hist_[pos + k];
        out.push_back(acc);
        pos += 3;
    }
    // pos < hist_.size() always holds here (T > 3), so the next output starts at index 0.
    hist_.erase(hist_.begin(), hist_.begin() + pos);
    phase_ = 0;
}

// ---------------------------------------------------------------------------
// ONNX Runtime

struct SpeakerModel::Impl {
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* opts = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* mem = nullptr;
    std::string inName, outName;
    bool normalizeSamples = true;
    bool globalMean = false;

    // True on success; frees the status object on failure.
    bool Quiet(OrtStatus* st) {
        if (!st) return true;
        api->ReleaseStatus(st);
        return false;
    }

    bool Ok(OrtStatus* st, std::wstring& err) {
        if (!st) return true;
        err = Widen(api->GetErrorMessage(st));
        api->ReleaseStatus(st);
        return false;
    }

    ~Impl() {
        if (!api) return;
        if (mem) api->ReleaseMemoryInfo(mem);
        if (session) api->ReleaseSession(session);
        if (opts) api->ReleaseSessionOptions(opts);
        if (env) api->ReleaseEnv(env);
    }
};

bool SpeakerModel::Installed() {
    const std::wstring d = ExeDir();
    return GetFileAttributesW((d + L"onnxruntime.dll").c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW((d + L"speaker.onnx").c_str()) != INVALID_FILE_ATTRIBUTES;
}

SpeakerModel::~SpeakerModel() { delete impl_; }

bool SpeakerModel::Load(const std::wstring& dirIn, std::wstring& err) {
    const std::wstring dir = dirIn.empty() ? ExeDir() : dirIn;
    Impl* m = new Impl;
    impl_ = m;

    // Full path only, so Windows never picks up a different onnxruntime.dll.
    m->dll = LoadLibraryExW((dir + L"onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m->dll) { err = L"onnxruntime.dll could not be loaded"; return false; }
    typedef const OrtApiBase*(ORT_API_CALL * GetApiBaseFn)(void);
    auto getBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(m->dll, "OrtGetApiBase"));
    if (!getBase) { err = L"onnxruntime.dll has no OrtGetApiBase"; return false; }
    m->api = getBase()->GetApi(ORT_API_VERSION);
    if (!m->api) { err = L"onnxruntime.dll is older than this build expects"; return false; }
    const OrtApi* a = m->api;

    if (!m->Ok(a->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "clearmic", &m->env), err)) return false;
    if (!m->Ok(a->CreateSessionOptions(&m->opts), err)) return false;
    m->Quiet(a->SetIntraOpNumThreads(m->opts, 1));    // one core: keep CPU use predictable
    m->Quiet(a->SetInterOpNumThreads(m->opts, 1));
    m->Quiet(a->SetSessionGraphOptimizationLevel(m->opts, ORT_ENABLE_ALL));
    if (!m->Ok(a->CreateSession(m->env, (dir + L"speaker.onnx").c_str(), m->opts, &m->session), err)) return false;
    if (!m->Ok(a->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m->mem), err)) return false;

    OrtAllocator* alloc = nullptr;
    if (!m->Ok(a->GetAllocatorWithDefaultOptions(&alloc), err)) return false;
    char* name = nullptr;
    if (!m->Ok(a->SessionGetInputName(m->session, 0, alloc, &name), err)) return false;
    m->inName = name;
    m->Quiet(a->AllocatorFree(alloc, name));
    if (!m->Ok(a->SessionGetOutputName(m->session, 0, alloc, &name), err)) return false;
    m->outName = name;
    m->Quiet(a->AllocatorFree(alloc, name));

    // Model metadata written by the sherpa-onnx export scripts.
    OrtModelMetadata* md = nullptr;
    if (m->Quiet(a->SessionGetModelMetadata(m->session, &md)) && md) {
        char* v = nullptr;
        if (m->Quiet(a->ModelMetadataLookupCustomMetadataMap(md, alloc, "normalize_samples", &v)) && v) {
            m->normalizeSamples = strcmp(v, "0") != 0;
            m->Quiet(a->AllocatorFree(alloc, v));
        }
        v = nullptr;
        if (m->Quiet(a->ModelMetadataLookupCustomMetadataMap(md, alloc, "feature_normalize_type", &v)) && v) {
            m->globalMean = strcmp(v, "global-mean") == 0;
            m->Quiet(a->AllocatorFree(alloc, v));
        }
        v = nullptr;
        if (m->Quiet(a->ModelMetadataLookupCustomMetadataMap(md, alloc, "sample_rate", &v)) && v) {
            const bool ok16k = strcmp(v, "16000") == 0;
            m->Quiet(a->AllocatorFree(alloc, v));
            if (!ok16k) { a->ReleaseModelMetadata(md); err = L"speaker model is not 16 kHz"; return false; }
        }
        a->ReleaseModelMetadata(md);
    }
    return true;
}

std::vector<float> SpeakerModel::Embed(const float* samples, size_t n) {
    std::vector<float> emb;
    Impl* m = impl_;
    if (!m || !m->session || n < kSr / 2) return emb;
    const OrtApi* a = m->api;

    // normalize_samples = 0 (WeSpeaker) means features are computed on 16-bit-scale samples.
    std::vector<float> scaled(samples, samples + n);
    if (!m->normalizeSamples)
        for (auto& v : scaled) v *= 32768.0f;
    int frames = 0;
    std::vector<float> feats = ComputeFbank(scaled.data(), n, frames);
    if (m->globalMean && frames > 0) {
        for (int b = 0; b < kBins; ++b) {
            double mean = 0;
            for (int f = 0; f < frames; ++f) mean += feats[size_t(f) * kBins + b];
            mean /= frames;
            for (int f = 0; f < frames; ++f) feats[size_t(f) * kBins + b] -= float(mean);
        }
    }

    const int64_t shape[3] = {1, frames, kBins};
    OrtValue* input = nullptr;
    OrtValue* output = nullptr;
    if (!m->Quiet(a->CreateTensorWithDataAsOrtValue(m->mem, feats.data(), feats.size() * sizeof(float), shape, 3,
                                                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input)))
        return emb;
    const char* inNames[1] = {m->inName.c_str()};
    const char* outNames[1] = {m->outName.c_str()};
    OrtStatus* st = a->Run(m->session, nullptr, inNames, &input, 1, outNames, 1, &output);
    a->ReleaseValue(input);
    if (st) { a->ReleaseStatus(st); return emb; }

    float* data = nullptr;
    OrtTensorTypeAndShapeInfo* info = nullptr;
    size_t count = 0;
    if (m->Quiet(a->GetTensorMutableData(output, reinterpret_cast<void**>(&data))) &&
        m->Quiet(a->GetTensorTypeAndShape(output, &info))) {
        if (!m->Quiet(a->GetTensorShapeElementCount(info, &count))) count = 0;
        a->ReleaseTensorTypeAndShapeInfo(info);
        emb.assign(data, data + count);
    }
    a->ReleaseValue(output);

    double norm = 0;
    for (float v : emb) norm += double(v) * v;
    norm = sqrt(norm);
    if (norm > 0)
        for (auto& v : emb) v = float(v / norm);
    return emb;
}

float SpeakerModel::Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d += double(a[i]) * b[i];
    return float(d);   // both are L2-normalised
}
