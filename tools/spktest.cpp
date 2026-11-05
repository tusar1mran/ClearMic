// Speaker-model check: prints cosine similarity between test recordings.
// Same speaker should score clearly higher than different speakers.
// Usage: spktest <dir with onnxruntime.dll + speaker.onnx> <wav> [<wav> ...]

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <string>
#include <vector>

#include "../src/speaker.h"

static std::vector<float> ReadWav16k(const wchar_t* path) {
    std::vector<float> out;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return out;
    char hdr[44];
    if (fread(hdr, 1, 44, f) == 44) {
        short s;
        while (fread(&s, 2, 1, f) == 1) out.push_back(s / 32768.0f);
    }
    fclose(f);
    return out;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { fwprintf(stderr, L"usage: spktest <modeldir\\> <wav>...\n"); return 2; }
    SpeakerModel model;
    std::wstring err;
    if (!model.Load(argv[1], err)) { fwprintf(stderr, L"load failed: %ls\n", err.c_str()); return 1; }

    std::vector<std::vector<float>> embs;
    std::vector<std::wstring> names;
    for (int i = 2; i < argc; ++i) {
        std::vector<float> wav = ReadWav16k(argv[i]);
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        std::vector<float> e = model.Embed(wav.data(), wav.size());
        QueryPerformanceCounter(&t1);
        std::wstring n = argv[i];
        n = n.substr(n.find_last_of(L"\\/") + 1);
        wprintf(L"%-24ls %5.2f s audio, dim %zu, %6.1f ms\n", n.c_str(), wav.size() / 16000.0, e.size(),
                (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        embs.push_back(e);
        names.push_back(n.substr(0, 10));
    }

    // 1.5 s window timing (what the live gate will use)
    std::vector<float> w = ReadWav16k(argv[2]);
    if (w.size() > 24000) w.resize(24000);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 10; ++i) model.Embed(w.data(), w.size());
    QueryPerformanceCounter(&t1);
    wprintf(L"\n1.5 s window: %.1f ms per embedding\n\n", (t1.QuadPart - t0.QuadPart) * 100.0 / f.QuadPart);

    // Simulate ClearMic's enrollment (first two files) and live 1 s checks on every file.
    {
        std::vector<float> enroll;
        for (int i = 2; i < 4 && i < argc; ++i) {
            std::vector<float> w2 = ReadWav16k(argv[i]);
            enroll.insert(enroll.end(), w2.begin(), w2.end());
        }
        std::vector<std::vector<float>> chunks;
        for (size_t p = 0; p + 16000 <= enroll.size(); p += 8000) chunks.push_back(model.Embed(&enroll[p], 16000));
        std::vector<float> mean(chunks[0].size(), 0.0f);
        for (auto& c : chunks) for (size_t i = 0; i < c.size(); ++i) mean[i] += c[i];
        double nrm = 0;
        for (float v : mean) nrm += double(v) * v;
        for (auto& v : mean) v = float(v / sqrt(nrm));
        std::vector<float> self;
        for (auto& c : chunks) self.push_back(SpeakerModel::Cosine(c, mean));
        std::sort(self.begin(), self.end());
        float t = self[self.size() / 10] - 0.05f;
        t = t < 0.25f ? 0.25f : (t > 0.6f ? 0.6f : t);
        wprintf(L"Enrolled on first two files (%zu chunks). Threshold: %.3f\n", chunks.size(), t);
        for (int i = 2; i < argc; ++i) {
            std::vector<float> w2 = ReadWav16k(argv[i]);
            int acc = 0, tot = 0;
            float lo = 1, hi = -1;
            for (size_t p = 0; p + 16000 <= w2.size(); p += 8000) {
                float s = SpeakerModel::Cosine(model.Embed(&w2[p], 16000), mean);
                lo = s < lo ? s : lo;
                hi = s > hi ? s : hi;
                acc += s >= t;
                ++tot;
            }
            // Live rule: 1 s score >= t AND last-0.5 s score >= t - 0.1
            int acc2 = 0, acc3 = 0, tot2 = 0;
            for (size_t p = 0; p + 16000 <= w2.size(); p += 8000) {
                float s1 = SpeakerModel::Cosine(model.Embed(&w2[p], 16000), mean);
                float s2 = SpeakerModel::Cosine(model.Embed(&w2[p + 8000], 8000), mean);
                acc2 += (s1 >= t && s2 >= t - 0.15f); acc3 += (s1 >= t && s2 >= t - 0.2f);
                ++tot2;
            }
            std::wstring n = argv[i];
            n = n.substr(n.find_last_of(L"\\/") + 1);
            wprintf(L"  %-24ls 1s rule %2d/%2d (scores %.2f..%.2f) | 1s+0.5s rule %2d/%2d\n", n.c_str(), acc, tot,
                    lo, hi, acc2, tot2, acc3, tot2);
        }
        wprintf(L"\n");
    }

    wprintf(L"%-11ls", L"");
    for (auto& n : names) wprintf(L"%-11ls", n.c_str());
    wprintf(L"\n");
    for (size_t i = 0; i < embs.size(); ++i) {
        wprintf(L"%-11ls", names[i].c_str());
        for (size_t j = 0; j < embs.size(); ++j) wprintf(L"%-11.3f", SpeakerModel::Cosine(embs[i], embs[j]));
        wprintf(L"\n");
    }
    return 0;
}
