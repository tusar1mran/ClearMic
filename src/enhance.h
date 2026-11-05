// Voice enhancer ("HD voice"): broadcast-style processing for speech.
//   high-pass 80 Hz -> mud cut 300 Hz (-2 dB) -> presence 3.5 kHz (+3 dB) -> air shelf 9 kHz (+2 dB)
//   -> compressor (3:1 above -24 dBFS) -> slow auto level (speech to about -20 dBFS) -> peak limiter (-1 dBFS)
// Works on 48 kHz mono float in [-1, 1]. It cannot add detail the microphone did not capture.
#pragma once

class Biquad {
public:
    static Biquad HighPass(double fs, double f, double q);
    static Biquad Peaking(double fs, double f, double q, double gainDb);
    static Biquad HighShelf(double fs, double f, double gainDb);
    float Process(float x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return float(y);
    }
    // Magnitude response in dB at frequency f (for tests).
    double ResponseDb(double fs, double f) const;
    void Reset() { z1 = z2 = 0; }

private:
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
};

class VoiceEnhancer {
public:
    VoiceEnhancer();
    void Reset();
    // speech: true when this frame contains the user's speech (drives the auto level).
    void Process(float* x, int n, bool speech);
    float AutoGainDb() const { return agcDb_; }
    // Combined EQ response in dB (for tests).
    double EqResponseDb(double f) const;

private:
    Biquad hp_, mud_, presence_, air_;
    float env_ = 0.0f;       // compressor envelope
    float agcDb_ = 0.0f;     // slow auto level
    float limGain_ = 1.0f;   // limiter gain
};
