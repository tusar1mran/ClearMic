// Voice enhancer. Filter formulas: RBJ "Audio EQ Cookbook".

#include "enhance.h"

#include <math.h>
#include <complex>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

constexpr float kCompThreshDb = -24.0f;
constexpr float kCompRatio = 3.0f;
constexpr float kCompKneeDb = 6.0f;
constexpr float kAgcTargetDb = -20.0f;   // speech level after processing
constexpr float kAgcMinDb = -6.0f, kAgcMaxDb = 12.0f;
constexpr float kLimitCeiling = 0.891f;  // -1 dBFS

float Coef(double seconds) { return float(exp(-1.0 / (seconds * kFs))); }
}

Biquad Biquad::HighPass(double fs, double f, double q) {
    const double w = 2 * kPi * f / fs, c = cos(w), alpha = sin(w) / (2 * q);
    const double a0 = 1 + alpha;
    Biquad b;
    b.b0 = (1 + c) / 2 / a0;
    b.b1 = -(1 + c) / a0;
    b.b2 = (1 + c) / 2 / a0;
    b.a1 = -2 * c / a0;
    b.a2 = (1 - alpha) / a0;
    return b;
}

Biquad Biquad::Peaking(double fs, double f, double q, double gainDb) {
    const double A = pow(10.0, gainDb / 40.0);
    const double w = 2 * kPi * f / fs, c = cos(w), alpha = sin(w) / (2 * q);
    const double a0 = 1 + alpha / A;
    Biquad b;
    b.b0 = (1 + alpha * A) / a0;
    b.b1 = -2 * c / a0;
    b.b2 = (1 - alpha * A) / a0;
    b.a1 = -2 * c / a0;
    b.a2 = (1 - alpha / A) / a0;
    return b;
}

Biquad Biquad::HighShelf(double fs, double f, double gainDb) {
    const double A = pow(10.0, gainDb / 40.0);
    const double w = 2 * kPi * f / fs, c = cos(w), s = sin(w);
    const double alpha = s / 2 * sqrt(2.0);   // shelf slope S = 1
    const double sq = 2 * sqrt(A) * alpha;
    const double a0 = (A + 1) - (A - 1) * c + sq;
    Biquad b;
    b.b0 = A * ((A + 1) + (A - 1) * c + sq) / a0;
    b.b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
    b.b2 = A * ((A + 1) + (A - 1) * c - sq) / a0;
    b.a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
    b.a2 = ((A + 1) - (A - 1) * c - sq) / a0;
    return b;
}

double Biquad::ResponseDb(double fs, double f) const {
    const std::complex<double> z1 = std::polar(1.0, -2 * kPi * f / fs);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    return 20 * log10(std::abs(h));
}

VoiceEnhancer::VoiceEnhancer()
    : hp_(Biquad::HighPass(kFs, 80, 0.707)),
      mud_(Biquad::Peaking(kFs, 300, 1.0, -2.0)),
      presence_(Biquad::Peaking(kFs, 3500, 0.8, 3.0)),
      air_(Biquad::HighShelf(kFs, 9000, 2.0)) {}

void VoiceEnhancer::Reset() {
    hp_.Reset();
    mud_.Reset();
    presence_.Reset();
    air_.Reset();
    env_ = 0;
    limGain_ = 1;
    // agcDb_ is kept so the level doesn't jump after a device restart.
}

double VoiceEnhancer::EqResponseDb(double f) const {
    return hp_.ResponseDb(kFs, f) + mud_.ResponseDb(kFs, f) + presence_.ResponseDb(kFs, f) +
           air_.ResponseDb(kFs, f);
}

void VoiceEnhancer::Process(float* x, int n, bool speech) {
    static const float att = Coef(0.005), rel = Coef(0.150), limRel = Coef(0.050);

    // 1) EQ + compressor
    double sumSq = 0;
    for (int i = 0; i < n; ++i) {
        float s = air_.Process(presence_.Process(mud_.Process(hp_.Process(x[i]))));
        const float lvl = fabsf(s);
        env_ = lvl > env_ ? att * env_ + (1 - att) * lvl : rel * env_ + (1 - rel) * lvl;
        const float envDb = 20.0f * log10f(env_ + 1e-9f);
        const float over = envDb - kCompThreshDb;
        float grDb = 0;   // soft knee
        if (over > kCompKneeDb / 2) grDb = over * (1 - 1 / kCompRatio);
        else if (over > -kCompKneeDb / 2) {
            const float k = over + kCompKneeDb / 2;
            grDb = (1 - 1 / kCompRatio) * k * k / (2 * kCompKneeDb);
        }
        s *= powf(10.0f, -grDb / 20.0f);
        x[i] = s;
        sumSq += double(s) * s;
    }

    // 2) Auto level: adjusts only while you speak, so silence/noise is never pumped up.
    const float prevDb = agcDb_;
    const float frameDb = 10.0f * log10f(float(sumSq / n) + 1e-12f);
    if (speech && frameDb > -55.0f) {
        agcDb_ += 0.01f * (kAgcTargetDb - (frameDb + agcDb_));   // ~1 s time constant
        agcDb_ = agcDb_ < kAgcMinDb ? kAgcMinDb : (agcDb_ > kAgcMaxDb ? kAgcMaxDb : agcDb_);
    }
    const float g0 = powf(10.0f, prevDb / 20.0f), g1 = powf(10.0f, agcDb_ / 20.0f);

    // 3) Gain ramp + peak limiter (instant attack, 50 ms release)
    for (int i = 0; i < n; ++i) {
        float s = x[i] * (g0 + (g1 - g0) * (i + 1) / n);
        limGain_ = limRel * limGain_ + (1 - limRel) * 1.0f;            // release towards unity
        if (fabsf(s) * limGain_ > kLimitCeiling) limGain_ = kLimitCeiling / fabsf(s);   // never exceed
        x[i] = s * limGain_;
    }
}
