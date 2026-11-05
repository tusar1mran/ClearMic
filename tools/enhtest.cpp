// Checks the voice enhancer: EQ curve, limiter ceiling, and auto level on real speech.
// Usage: enhtest <16 kHz mono 16-bit wav>

#include <math.h>
#include <stdio.h>
#include <vector>

#include "../src/enhance.h"

static std::vector<float> ReadWavAs48k(const wchar_t* path) {
    std::vector<float> out;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return out;
    char hdr[44];
    if (fread(hdr, 1, 44, f) == 44) {
        short s;
        float prev = 0;
        while (fread(&s, 2, 1, f) == 1) {   // linear-interpolate 16k -> 48k
            const float v = s / 32768.0f;
            out.push_back(prev + (v - prev) / 3);
            out.push_back(prev + 2 * (v - prev) / 3);
            out.push_back(v);
            prev = v;
        }
    }
    fclose(f);
    return out;
}

static void Stats(const std::vector<float>& x, const char* label) {
    double sq = 0, pk = 0;
    for (float v : x) { sq += double(v) * v; pk = fabs(v) > pk ? fabs(v) : pk; }
    printf("  %-34s RMS %6.1f dBFS   peak %6.1f dBFS\n", label, 10 * log10(sq / x.size() + 1e-12),
           20 * log10(pk + 1e-12));
}

static std::vector<float> Run(std::vector<float> x, VoiceEnhancer& e) {
    for (size_t p = 0; p + 480 <= x.size(); p += 480) e.Process(&x[p], 480, true);
    return x;
}

int wmain(int argc, wchar_t** argv) {
    VoiceEnhancer eq;
    printf("EQ response:\n");
    for (double f : {40.0, 80.0, 150.0, 300.0, 1000.0, 2000.0, 3500.0, 6000.0, 10000.0, 15000.0})
        printf("  %6.0f Hz  %+5.1f dB\n", f, eq.EqResponseDb(f));

    printf("\nLimiter (1 kHz sine at 0 dBFS, 2 s):\n");
    std::vector<float> sine(96000);
    for (size_t i = 0; i < sine.size(); ++i) sine[i] = float(sin(2 * 3.14159265358979 * 1000 * i / 48000.0));
    VoiceEnhancer e1;
    std::vector<float> o1 = Run(sine, e1);
    Stats(o1, "output");

    if (argc > 1) {
        std::vector<float> speech = ReadWavAs48k(argv[1]);
        for (double gainDb : {-20.0, 0.0}) {
            std::vector<float> in = speech;
            const float g = float(pow(10.0, gainDb / 20));
            for (auto& v : in) v *= g;
            // Run twice through the same enhancer so the auto level has settled.
            VoiceEnhancer e;
            Run(in, e);
            std::vector<float> out = Run(in, e);
            printf("\nSpeech, input %+.0f dB (auto level settled at %+.1f dB):\n", gainDb, e.AutoGainDb());
            Stats(in, "input");
            Stats(out, "enhanced");
        }
    }
    return 0;
}
