#include "Crumb.h"
#include <optional>
#include <sstream>

using namespace juce;

namespace
{
// ============================ FROZEN — never rename/reorder semantics ============================
// Hosts store sessions and automation under these string IDs (ParameterID version hint 1; autoGain, appended in
// 0.2.0: 2; loss to auto, appended in 0.3.0: 3). Never rename, remove, reorder or change a range so that old values move. New parameters are APPENDED only.
// Planned stereo independence (pending David): append `link` (default ON) and per-channel twins such as
// `rateR`, `bitsR`, ... after "autoGain". Old states lack them, so they load at their defaults = linked,
// both channels identical. The state parser below already resets missing IDs to defaults.
const char* const ids[] = { "preamp", "bits",     "dcShift", "dither", "rate",   "jitter", "approxOn", "approxDev", "imagesOn",
                            "imagesShift", "fType", "fOrder", "cutoff", "reso", "mix",    "output", "bypass", "autoGain",
                            "loss",   "speed",    "drop",    "fill",   "keep",   "ear",    "gate",     "stereo",    "lowCut",
                            "highCut", "slope",   "invert",  "auto" };
// =================================================================================================
enum { Preamp, Bits, DcShift, Dither, Rate, Jitter, ApproxOn, ApproxDev, ImagesOn, ImagesShift, FType, FOrder, Cutoff, Reso, Mix, Output, Bypass, AutoGain,
       Loss, Speed, Drop, Fill, Keep, Ear, Gate, Stereo, LowCut, HighCut, Slope, Invert, Auto, numParams };
constexpr int numSound = numParams - 1; // presets and A/B slots: every parameter but bypass, k -> ids[k < Bypass ? k : k + 1]
int sound(int k) { return k < Bypass ? k : k + 1; }
// Smoothed per-sample controls. Rate and cutoff are smoothed as log2(Hz); the last four mix the filter outputs.
enum { SPre, SRate, SDither, SJitter, SApprox, SDev, SImages, SShift, SCut, SReso, SOrder, SMix, SOut, SBypass, SDry, SLP, SBP, SHP, SClip, SAuto, SLoss, SDrop, SMono, SLow, SHigh, numSmoothed };
constexpr double pi = MathConstants<double>::pi;
constexpr int taps = 64, over = 64, ringMask = taps - 1; // minBLEP length (host samples) and table oversampling

AudioProcessorValueTreeState::ParameterLayout layout()
{
    // Every float snaps to 0.1 of its unit: save -> reload is then bit-exact on every platform (as in Snap).
    const auto snap = [](float s, float e, float v) { return jlimit(s, e, std::round(v * 10) / 10 + 0.0f); };
    // Display text as on the panel ("26.0 kHz", "+12.0 dB", "35%"); typed text may use k for kHz.
    auto trim = [](float v) { return String(v, std::abs(v - std::round(v)) < 0.05f ? 0 : 1); };
    auto text = [trim](const char* kind) {
        return [unit = String(kind), trim](float v, int) {
            if (unit == "Hz")
                return v >= 999.5f ? String(v / 1000, 1) + " kHz" : String(roundToInt(v)) + " Hz";
            if (unit == "gate")
                return v <= -96 ? String("off") : String(v, 1) + " dB";
            if (unit.endsWith("dB")) // preamp "+12.0 dB", output "-3.5 dB"
                return (v > 0 ? "+" : "") + String(v + 0.0f, 1) + " dB";
            return (unit == "+%" && v > 0 ? "+" : "") + trim(v) + "%";
        };
    };
    auto parse = [](const String& t) { return (float) (t.replace(CharPointer_UTF8("\xe2\x88\x92"), "-").retainCharacters("+-.0123456789").getDoubleValue() * (t.containsIgnoreCase("k") ? 1000 : 1)); };
    auto parseGate = [parse](const String& t) { return t.containsIgnoreCase("off") ? -96.0f : parse(t); };
    auto num = [&](int i, const char* name, float lo, float hi, float def, const char* unit, bool log = false) {
        NormalisableRange<float> r(lo, hi, nullptr, nullptr, snap);
        if (log)
            r = NormalisableRange<float>(lo, hi, [](float s, float e, float v) { return s * std::pow(e / s, v); },
                                         [](float s, float e, float v) { return std::log(v / s) / std::log(e / s); }, snap);
        r.interval = log ? 0.0f : 0.1f;
        const bool gate = String(unit) == "gate";
        return std::make_unique<AudioParameterFloat>(ParameterID { ids[i], i <= Bypass ? 1 : 3 }, name, r, def, AudioParameterFloatAttributes()
                                                                          .withLabel(gate ? String("dB") : String(unit).removeCharacters("+"))
                                                                          .withStringFromValueFunction(text(unit))
                                                                          .withValueFromStringFunction([gate, parse, parseGate](const String& t) { return gate ? parseGate(t) : parse(t); }));
    };
    auto onOff = [](int i, const char* name, bool def) { return std::make_unique<AudioParameterBool>(ParameterID { ids[i], i <= Bypass ? 1 : 3 }, name, def); };
    auto choice = [](int i, const char* name, StringArray items, int def) {
        return std::make_unique<AudioParameterChoice>(ParameterID { ids[i], i <= Bypass ? 1 : 3 }, name, items, def);
    };
    auto bitsText = AudioParameterIntAttributes().withStringFromValueFunction([](int v, int) { return v == 0 ? String("OFF") : String(v) + " bit"; });
    return { num(Preamp, "Drive", 0, 48, 0, "+dB"),
             std::make_unique<AudioParameterInt>(ParameterID { ids[Bits], 1 }, "Bits", 0, 24, 12, bitsText),
             onOff(DcShift, "DC Shift", false),
             num(Dither, "Dither", 0, 100, 0, "%"),
             num(Rate, "Rate", 44, 44100, 26040, "Hz", true),
             num(Jitter, "Jitter", 0, 100, 0, "%"),
             onOff(ApproxOn, "Anti-Alias On", true),
             num(ApproxDev, "Anti-Alias", -100, 100, 0, "+%"),
             onOff(ImagesOn, "Anti-Image On", false),
             num(ImagesShift, "Anti-Image", 0, 100, 0, "%"),
             choice(FType, "Filter Type", { "OFF", "LP", "BP", "HP", "BR" }, 0),
             choice(FOrder, "Filter Position", { "Pre", "Post" }, 1),
             num(Cutoff, "Cutoff", 20, 20000, 20000, "Hz", true),
             num(Reso, "Resonance", 0, 100, 0, "%"),
             num(Mix, "Mix", 0, 100, 100, "%"),
             num(Output, "Output", -24, 12, 0, "dB"),
             onOff(Bypass, "Bypass", false),
             std::make_unique<AudioParameterBool>(ParameterID { ids[AutoGain], 2 }, "Auto Gain", false),
             num(Loss, "Loss", 0, 100, 0, "%"),
             num(Speed, "Speed", 0, 100, 100, "%"),
             num(Drop, "Packet Loss", 0, 100, 0, "%"),
             choice(Fill, "Packets", { "drop", "repeat" }, 0),
             choice(Keep, "Delta", { "off", "on" }, 0),
             choice(Ear, "Weighting", { "perceptual", "flat" }, 0),
             num(Gate, "Gate", -96, 0, -96, "gate"),
             choice(Stereo, "Stereo Mode", { "stereo", "joint", "mono" }, 0),
             num(LowCut, "Low Cut", 20, 20000, 20, "Hz", true),
             num(HighCut, "High Cut", 20, 20000, 20000, "Hz", true),
             choice(Slope, "Filter Slope", { "6 dB", "12 dB", "18 dB", "24 dB", "36 dB", "48 dB", "72 dB", "96 dB" }, 3),
             onOff(Invert, "Filter Invert", false),
             num(Auto, "Auto Gain Amount", 0, 100, 100, "%") };
}

// Minimum-phase lowpass sampled `over` times per host sample over `taps` samples: its impulse (unit DC gain per host
// sample) and its integral, the minBLEP step (0 -> 1). Blackman-Harris windowed sinc whose stopband starts at the host
// Nyquist, made minimum phase through the real cepstrum (Oppenheim & Schafer). Being causal, it reconstructs the input
// between host samples (for the virtual ADC) and bandlimits the hold steps (for the DAC) with zero latency.
struct Kernel
{
    std::vector<double> impulse, step;
    double delay = 0; // host samples, the impulse's first moment
};
const Kernel& kernel()
{
    static const Kernel table = [] {
        constexpr int n = taps * over;
        constexpr double cutoff = 0.5 - 4.0 / taps; // window main-lobe half width is 4 / taps cycles per sample
        std::vector<std::complex<double>> x(16 * n);
        for (int i = 0; i <= n; ++i)
        {
            const double t = (double) i / over - taps / 2.0, w = 2 * pi * i / n;
            x[(size_t) i] = (0.35875 - 0.48829 * std::cos(w) + 0.14128 * std::cos(2 * w) - 0.01168 * std::cos(3 * w))
                            * (std::abs(t) < 1e-12 ? 2 * cutoff : std::sin(2 * pi * cutoff * t) / (pi * t));
        }
        crumb::fft(x, false);
        for (auto& c : x)
            c = std::log(std::max(std::abs(c), 1e-12));
        crumb::fft(x, true);
        for (size_t i = 1; i < x.size() / 2; ++i) // fold the cepstrum: anticausal part onto the causal one
            x[i] *= 2, x[x.size() - i] = 0;
        crumb::fft(x, false);
        for (auto& c : x)
            c = std::exp(c);
        crumb::fft(x, true);
        Kernel k { std::vector<double>(n + 2, 0.0), std::vector<double>(n + 2, 1.0) };
        double sum = 0, acc = 0;
        for (int i = 0; i <= n; ++i)
            sum += x[(size_t) i].real();
        for (int i = 0; i <= n; ++i) // trapezoidal integration
        {
            k.impulse[(size_t) i] = x[(size_t) i].real() * over / sum;
            k.step[(size_t) i] = (acc + 0.5 * x[(size_t) i].real()) / sum, acc += x[(size_t) i].real();
        }
        for (int i = 0; i <= n; ++i)
            k.delay += k.impulse[(size_t) i] / over * i / over;
        return k;
    }();
    return table;
}

// The TPT state-variable filter, its Butterworth dampings and the ADC soft clip are in Band.h and Crumb.h.
using crumb::Coef, crumb::svf, crumb::Svf, crumb::Band, crumb::approxK, crumb::imagesK, crumb::knee, crumb::soft, crumb::clip;
// The soft clip's antiderivative, for first-order ADAA.
double clipIntegral(double x)
{
    const double a = std::abs(x), u = (a - knee) / soft; // stable log cosh, continuous at the knee
    return a <= knee ? 0.5 * a * a : knee * a - 0.5 * knee * knee
        + soft * soft * (u + std::log1p(std::exp(-2 * u)) - std::log(2.0));
}

double lerp(const double* table, double p)
{
    const int i = (int) p;
    return table[i] + (p - i) * (table[i + 1] - table[i]);
}

struct Frame
{
    double v[numSmoothed] {}, inc = 1, dcCoef = 0, jitCoef = 0, jitNorm = 0, lsb = 0, half = 0, gLow = 0, gHigh = 0, lag = 0;
    Coef approx[4], images[2], filter;
    int bits = 0, ladderBits = 0;
    bool tread = false, identity = false;
};

struct BlepWeights { double h[taps], b[taps]; int h0 = 0; bool tick = false; };

struct Channel
{
    Svf pre, post, approx[4], images[2];
    double adcPrev = 0, adcPrevD = 0, hist[taps] {}, ring[taps] {}, phase = 0, held = 0, dc = 0, jw = 0, into = 0;
    const double *impulse = kernel().impulse.data(), *blep = kernel().step.data();
    int pos = 0; // shared by hist (newest sample) and ring (current output sample)
    uint32 seed = 1;

    double random() { return (double) ((seed = seed * 1664525u + 1013904223u) >> 8) * 0x1p-24; } // LCG, [0, 1)
    double filter(Svf& s, double x, const Frame& f)
    {
        const auto o = s.run(x, f.filter);
        return f.v[SDry] * x + f.v[SLP] * o[0] + f.v[SBP] * f.filter.k * o[1] + f.v[SHP] * o[2];
    }
    // Host-rate dither -> quantizer -> DAC with fixed vintage ladder errors on the four MSBs of the two's complement
    // code (code 0 = silence has none). At host rate, so the approx lowpass and the S&H bandlimit them. The tracked
    // converter-added DC (mid-riser idle offset, ladder errors) is removed by the fixed DAC coupling stage.
    double convert(double v, const Frame& f)
    {
        double q = v;
        if (f.bits > 0)
        {
            static constexpr double ladder[] = { 0.6, -0.4, 0.3, -0.2 }; // MSB weight errors in LSB
            const double lsb = f.lsb, half = f.half;
            const double u = (v + f.v[SDither] * lsb * (random() - 0.5)) / lsb;
            const double k = jlimit(-half, half - 1, f.tread ? std::round(u) : std::floor(u));
            q = (k + (f.tread ? 0 : 0.5)) * lsb;
            for (int j = 0; j < f.ladderBits; ++j)
                q += (((uint32) (int) k >> (f.bits - 1 - j)) & 1) * ladder[j] * lsb;
        }
        dc += f.dcCoef * (q - v - dc);
        return q;
    }
    void buildBlep(BlepWeights& w, double d) const
    {
        w.tick = true, w.h0 = d > 0 ? 1 : 0;
        for (int j = w.h0; j < taps; ++j)
            w.h[j] = lerp(impulse, (j - d) * over);
        for (int j = 0; j < taps; ++j)
            w.b[j] = lerp(blep, (j + d) * over) - 1;
    }
    double process(double x, const Frame& f, Band& band, int channel, const BlepWeights* reuse, BlepWeights* store)
    {
        // Preamp/ADC clip, filter[pre], dither, quantizer, approx lowpass. While the clip is engaged (preamp up or the
        // block above the knee) it is pure first-order ADAA; idle it is the plain clip, here the identity, so a clean
        // signal passes bit-exactly. SClip crossfades the two over 20 ms: no half-sample delay step at a switch.
        const double a = jlimit(-64.0, 64.0, x * f.v[SPre]), aD = clipIntegral(a), plain = clip(a);
        const double adaa = std::abs(a - adcPrev) < 1e-6 ? clip(0.5 * (a + adcPrev)) : (aD - adcPrevD) / (a - adcPrev);
        adcPrev = a, adcPrevD = aD;
        double y = plain + f.v[SClip] * (adaa - plain);
        y += (1 - f.v[SOrder]) * (filter(pre, y, f) - y);
        if (!band.idle()) y = band.run(channel, y);
        into = y;
        y = convert(y, f);
        double lp = y;
        for (int k = 0; k < 4; ++k)
            lp = approx[k].run(lp, f.approx[k])[0];
        y += f.v[SApprox] * (lp - y);

        // Sample & hold on a jittered virtual clock. The input is read at the exact tick time through the causal
        // reconstruction kernel, every step edge gets a minBLEP residual: only the emulated fold-down aliases.
        hist[pos] = y;
        jw += f.jitCoef * (2 * random() - 1 - jw);
        const double inc = jmin(1.0, f.inc * (1 + 0.03 * f.v[SJitter] * jlimit(-1.0, 1.0, jw * f.jitNorm)));
        if (f.identity) // a tick on every host sample: the S&H is the identity
            phase = 0, held = y;
        else if ((phase += inc) >= 1)
        {
            phase -= 1;
            const double d = phase / inc, old = held; // the tick was d samples ago, d in [0, 1)
            BlepWeights local;
            if (store != nullptr)
                buildBlep(*store, d);
            else if (reuse == nullptr || !reuse->tick)
                buildBlep(local, d);
            const auto& w = store != nullptr ? *store : reuse != nullptr && reuse->tick ? *reuse : local;
            double v = 0;
            for (int j = w.h0; j < taps; ++j) // input at time n - d from samples n - j, j - d >= 0
                v += hist[(pos - j + taps) & ringMask] * w.h[j];
            held = v;
            for (int j = 0; j < taps; ++j)
                ring[(pos + j) & ringMask] += (held - old) * w.b[j];
        }
        y = held + ring[pos];
        ring[pos] = 0, pos = (pos + 1) & ringMask;

        // Images lowpass -> fixed DAC coupling (converter DC removal) -> filter[post].
        lp = y;
        for (int k = 0; k < 2; ++k)
            lp = images[k].run(lp, f.images[k])[0];
        y += f.v[SImages] * (lp - y);
        y -= dc;
        return y + f.v[SOrder] * (filter(post, y, f) - y);
    }
};

std::unique_ptr<XmlElement> decode(std::string_view x) { return crumb::isFlatCrumbXml(x) ? XmlDocument::parse(String::fromUTF8(x.data(), (int) x.size())) : nullptr; }
std::optional<double> number(const String& t) // the whole text is one finite number (C locale); else nothing
{
    std::istringstream in(t.toStdString());
    in.imbue(std::locale::classic());
    double v = 0;
    return in >> v && std::isfinite(v) && (in >> std::ws).eof() ? std::optional(jlimit(-1.0e6, 1.0e6, v)) : std::nullopt;
}
} // namespace

void crumb::fft(std::vector<std::complex<double>>& x, bool inverse)
{
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i) // bit-reversal permutation
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        if (i < (j ^= bit))
            std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto a = x[i + k], b = x[i + k + len / 2] * std::polar(1.0, (inverse ? 2 : -2) * pi * (double) k / (double) len);
                x[i + k] = a + b, x[i + k + len / 2] = a - b;
            }
    if (inverse)
        for (auto& c : x)
            c /= (double) n;
}

struct CrumbProcessor::Dsp
{
    std::array<std::atomic<float>*, numParams> raw {};
    std::array<SmoothedValue<double>, numSmoothed> smooth;
    std::array<Channel, 2> ch;
    Band bandPre, bandPost;
    crumb::Codec codec;
    std::vector<float> dryRing[2], cutRing, preRing, heldRing; // the input, for the dry path beside the codec, where the drive clipped it, and the crush stage's in and out
    int ringMask2 = 0, warm = 0;
    uint32 ringAt = 0;
    double engaged = 0, engageStep = 0; // the codec's crossfade, 0 = off (the 0.2 path bit for bit)
    double gateEnv = 0, gateGain = 1, gateCoef = 0, gateAttack = 0, gateRelease = 0;
    double fs = 48000, clipOn = 0;
    double dcCoef = 0, jitCoef = 0, jitNorm = 0;
    std::array<double, 7> coefKeys {};
    Frame cached;
    bool coefValid = false;
    int clipHold = 0;
    int hot = 0; // samples left in the clip's engage hold: the ADAA path stays on across recurring knee crossings

    void bindSampleRate(double sr)
    {
        fs = sr > 0 && sr < 1.0e7 ? sr : 48000.0;
        dcCoef = jmin(1.0, 2 * pi * 5 / fs);
        jitCoef = 1 - std::exp(-2 * pi * 30 / fs), jitNorm = 0.5 / std::sqrt(jitCoef / (3 * (2 - jitCoef)));
        clipHold = (int) (0.25 * fs), coefValid = false;
        codec.prepare(jmin(fs, 400000.0)); // allocates: prepareToPlay only
        ringMask2 = 2 * codec.latency - 1;
        for (auto& r : dryRing) r.assign((size_t) (2 * codec.latency), 0.0f);
        for (auto* r : { &cutRing, &heldRing }) r->assign((size_t) (2 * codec.latency), 0.0f);
        preRing.assign((size_t) (4 * codec.latency), 0.0f);
        engageStep = 1 / (0.02 * fs);
        gateCoef = 1 - std::exp(-1 / (0.005 * fs)), gateAttack = 1 - std::exp(-1 / (0.001 * fs)), gateRelease = 1 - std::exp(-1 / (0.08 * fs));
    }
    double param(int i) const { return (double) raw[(size_t) i]->load(); }
    bool codecWanted() const { return param(Loss) > 0 || param(Drop) > 0; }
    int bandConfig(bool pre) const
    {
        return (param(FOrder) < 0.5) == pre ? crumb::bandConfig(param(LowCut), param(HighCut), (int) param(Slope), param(Invert) > 0.5) : 0;
    }
    void reset()
    {
        coefValid = false;
        for (uint32 c = 0; c < 2; ++c)
            ch[c] = Channel(), ch[c].seed = 0x9e3779b9u * (c + 1);
        bandPre.reset(fs, bandConfig(true)), bandPost.reset(fs, bandConfig(false));
        codec.reset();
        for (auto& r : dryRing) std::fill(r.begin(), r.end(), 0.0f);
        for (auto* r : { &cutRing, &preRing, &heldRing }) std::fill(r->begin(), r->end(), 0.0f);
        ringAt = 0, warm = 0, engaged = codecWanted() ? 1 : 0;
        gateEnv = 0, gateGain = 1;
        targets(true);
    }
    void targets(bool jump)
    {
        auto p = [this](int i) { return (double) raw[(size_t) i]->load(); };
        const int type = (int) p(FType);
        const double t[numSmoothed] = { Decibels::decibelsToGain(p(Preamp)), std::log2(p(Rate)), p(Dither) / 100, p(Jitter) / 100, p(ApproxOn),
                                        p(ApproxDev) / 100, p(ImagesOn), p(ImagesShift) / 100, std::log2(p(Cutoff)), p(Reso) / 100, p(FOrder),
                                        p(Mix) / 100, Decibels::decibelsToGain(p(Output)), p(Bypass), type == 0 ? 1.0 : 0.0,
                                        type == 1 || type == 4 ? 1.0 : 0.0, type == 2 ? 1.0 : 0.0, type == 3 || type == 4 ? 1.0 : 0.0, clipOn, p(AutoGain) * (p(Auto) / 100),
                                        p(Loss) / 100, p(Drop) / 100, p(Stereo) > 1.5 ? 1.0 : 0.0, std::log2(p(LowCut)), std::log2(p(HighCut)) };
        for (int k = 0; k < numSmoothed; ++k)
            jump ? smooth[(size_t) k].setCurrentAndTargetValue(t[k]) : smooth[(size_t) k].setTargetValue(t[k]);
    }
    void next(Frame& f)
    {
        for (int k = 0; k < numSmoothed; ++k)
            f.v[k] = smooth[(size_t) k].getNextValue();
        const std::array<double, 7> keys { f.v[SRate], f.v[SDev], f.v[SShift], f.v[SCut], f.v[SReso], f.v[SLow], f.v[SHigh] };
        if (!(coefValid && keys == coefKeys)) // exact == for every key; never reuse a neighbouring ramp value
        {
            const double fr = jmin(fs, std::exp2(f.v[SRate])), nyquist = 0.49 * fs;
            auto g = [&](double hz) { return std::tan(pi * jmin(nyquist, hz) / fs); };
            cached.inc = fr / fs;
            const double ga = g(0.5 * fr * std::exp2(f.v[SDev])), gi = g(0.5 * fr * std::exp2(2 * f.v[SShift])); // Nyquist(fr) * 2^oct
            for (int k = 0; k < 4; ++k)
                cached.approx[k] = svf(ga, approxK[k]);
            for (int k = 0; k < 2; ++k)
                cached.images[k] = svf(gi, imagesK[k]);
            cached.filter = svf(g(std::exp2(f.v[SCut])), 2 * std::pow(40.0, -f.v[SReso])); // Q 0.5 .. 20
            cached.lag = (approxK[0] + approxK[1] + approxK[2] + approxK[3]) / (pi * fr * std::exp2(f.v[SDev])); // the anti-alias filter's delay in seconds
            cached.gLow = g(std::exp2(f.v[SLow])), cached.gHigh = g(std::exp2(f.v[SHigh]));
            coefKeys = keys, coefValid = true;
        }
        f.inc = cached.inc, f.identity = f.inc >= 1 - 1e-9 && f.v[SJitter] <= 0, f.dcCoef = dcCoef;
        std::copy(std::begin(cached.approx), std::end(cached.approx), f.approx);
        std::copy(std::begin(cached.images), std::end(cached.images), f.images);
        f.filter = cached.filter, f.gLow = cached.gLow, f.gHigh = cached.gHigh, f.lag = f.v[SApprox] * cached.lag;
    }
};

CrumbProcessor::CrumbProcessor(File directory)
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo()).withOutput("Output", AudioChannelSet::stereo())),
      apvts(*this, nullptr, "CRUMB", layout()), presetDirectory(std::move(directory)), dsp(std::make_unique<Dsp>())
{
    for (int i = 0; i < numParams; ++i)
        dsp->raw[(size_t) i] = apvts.getRawParameterValue(ids[i]);
    for (int i : { Loss, Drop }) apvts.addParameterListener(ids[i], this);
}

CrumbProcessor::~CrumbProcessor()
{
    for (int i : { Loss, Drop }) apvts.removeParameterListener(ids[i], this);
    cancelPendingUpdate();
}

// The codec's latency is reported while it runs and zero while it does not, always from the message thread.
bool CrumbProcessor::codecOn() const { return dsp->codecWanted(); }
void CrumbProcessor::updateLatency()
{
    if (const int n = codecOn() ? dsp->codec.latency : 0; n != getLatencySamples()) setLatencySamples(n);
}
void CrumbProcessor::handleAsyncUpdate() { updateLatency(); }
void CrumbProcessor::parameterChanged(const String&, float)
{
    if ((codecOn() ? dsp->codec.latency : 0) == getLatencySamples()) return;
    if (auto* mm = MessageManager::getInstanceWithoutCreating(); mm != nullptr && mm->isThisTheMessageThread()) updateLatency();
    else triggerAsyncUpdate();
}

AudioProcessorParameter* CrumbProcessor::getBypassParameter() const { return apvts.getParameter(ids[Bypass]); }

bool CrumbProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet();
    return in == l.getMainOutputChannelSet() && (in == AudioChannelSet::mono() || in == AudioChannelSet::stereo());
}

void CrumbProcessor::prepareToPlay(double fs, int)
{
    auto& d = *dsp;
    d.bindSampleRate(fs); // a nonsense rate (NaN, inf, <= 0) must not reach the DSP
    sampleRate = d.fs;
    for (auto& s : d.smooth)
        s.reset(d.fs, 0.02);
    updateLatency();
    resetPending = true; // the first block initializes the DSP from the parameters it then sees, restored or not
}

void CrumbProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples(), channels = jmin(2, getTotalNumInputChannels(), buffer.getNumChannels());
    for (int c = channels; c < buffer.getNumChannels(); ++c)
        buffer.clear(c, 0, n);
    auto& d = *dsp;
    const bool driven = d.raw[Preamp]->load() > 0;
    if (resetPending.exchange(false, std::memory_order_acquire))
        d.hot = 0, d.clipOn = driven ? 1 : 0, d.reset(), scope.origin.store(scope.write.load(std::memory_order_relaxed), std::memory_order_relaxed);
    d.targets(false);
    Frame f;
    f.bits = (int) d.raw[Bits]->load(), f.tread = d.raw[DcShift]->load() > 0.5f;
    if (f.bits > 0)
        f.lsb = std::ldexp(1.0, 1 - f.bits), f.half = std::ldexp(1.0, f.bits - 1), f.ladderBits = jmin(4, f.bits);
    f.jitCoef = d.jitCoef, f.jitNorm = d.jitNorm; // ~5 ms walk, +-2 sigma
    float* x[2] = { channels > 0 ? buffer.getWritePointer(0) : nullptr, channels > 1 ? buffer.getWritePointer(1) : nullptr };
    const auto w = scope.write.load(std::memory_order_relaxed);
    const bool codecWanted = d.codecWanted();
    const int configPre = d.bandConfig(true), configPost = d.bandConfig(false), latency = d.codec.latency;
    const double gateDb = d.param(Gate), gateOpen = gateDb <= -96 ? -1 : std::pow(10.0, gateDb / 10);
    crumb::CodecSettings cs;
    cs.speed = d.param(Speed) / 100, cs.hold = d.param(Fill) > 0.5, cs.kept = d.param(Keep) > 0.5, cs.flat = d.param(Ear) > 0.5, cs.stereo = (int) d.param(Stereo);
    float peak = 0;
    for (int i = 0; i < n && channels > 0; ++i)
    {
        // Clip engagement, per sample (independent of the host's block size): preamp up, or the input passed the knee
        // within the last 250 ms (NaN counts as loud). SClip crossfades over 20 ms when that changes.
        bool loud = false;
        for (int c = 0; c < channels; ++c) loud = loud || !(std::abs(x[c][i]) <= knee);
        d.hot = loud ? d.clipHold : jmax(0, d.hot - 1);
        d.clipOn = driven || d.hot > 0 ? 1 : 0;
        d.smooth[SClip].setTargetValue(d.clipOn);
        d.next(f);
        d.bandPre.next(configPre, f.gLow, f.gHigh, f.v[SReso]), d.bandPost.next(configPost, f.gLow, f.gHigh, f.v[SReso]);
        const double on = 1 - f.v[SBypass], mix = f.v[SMix] * on, gain = 1 + on * (f.v[SOut] - 1);
        // Auto gain: the wet path gets the inverse of the smoothed preamp, crossfaded in over 20 ms. Off, wet = 1 exactly.
        const double wet = 1 + f.v[SAuto] * (1 / f.v[SPre] - 1);
        BlepWeights shared;
        const bool locked = channels == 2 && exactlyEqual(f.v[SJitter], 0.0) && exactlyEqual(d.ch[0].phase, d.ch[1].phase);
        double dry[2] {}, y[2] {}, loudest = 0;
        for (int c = 0; c < channels; ++c) // non-finite input becomes silence; absurd levels are clipped
            dry[c] = std::isfinite(x[c][i]) ? jlimit(-1000.0f, 1000.0f, x[c][i]) : 0.0f, loudest = jmax(loudest, dry[c] * dry[c]);
        const double mono = channels == 2 ? f.v[SMono] : 0, middle = 0.5 * (dry[0] + dry[1]);
        float cut = 0, pre = 0, held = 0; // what the drive's clip takes off this sample, the louder side's; the crush stage's in and out
        for (int c = 0; c < channels; ++c)
        {
            const double x0 = mono > 0 ? dry[c] + mono * (middle - dry[c]) : dry[c];
            if (const float t = crumb::clipped(x0 * f.v[SPre]); std::abs(t) > std::abs(cut)) cut = t;
            y[c] = d.ch[(size_t) c].process(x0, f, d.bandPre, c, locked && c == 1 ? &shared : nullptr, locked && c == 0 ? &shared : nullptr);
            pre += (float) d.ch[(size_t) c].into / (float) channels, held += (float) (d.ch[(size_t) c].held - d.ch[(size_t) c].dc) / (float) channels;
        }
        // Gate: the louder side's 5 ms level against the threshold; closed, the crushed signal is silent.
        d.gateEnv += d.gateCoef * (loudest - d.gateEnv);
        const double open = d.gateEnv >= gateOpen ? 1 : 0;
        d.gateGain += (open > d.gateGain ? d.gateAttack : d.gateRelease) * (open - d.gateGain);
        if (d.gateGain > 0.99999) d.gateGain = 1;
        if (d.gateGain < 1)
            for (int c = 0; c < channels; ++c) y[c] *= d.gateGain;
        // The codec. While it starts, the direct path plays until the codec has a full latency of audio.
        const auto at = (size_t) (d.ringAt & (uint32) d.ringMask2), late = (size_t) ((d.ringAt - (uint32) latency) & (uint32) d.ringMask2);
        ++d.ringAt;
        double coded[2] {}, gone[2] {};
        auto mark = crumb::Codec::clean;
        if (codecWanted || d.engaged > 0)
        {
            cs.loss = f.v[SLoss], cs.drop = f.v[SDrop], cs.jitter = f.v[SJitter], cs.autoGain = f.v[SAuto];
            mark = d.codec.process(y, coded, gone, channels, cs);
            d.warm = jmin(d.warm + 1, latency);
            d.engaged = jlimit(0.0, 1.0, d.engaged + (codecWanted && d.warm >= latency ? d.engageStep : codecWanted ? 0.0 : -d.engageStep));
        }
        else
            d.codec.idle(y, channels), d.warm = 0;
        const double e = d.engaged;
        if (d.gateGain < 0.5 || mix <= 0) cut = 0; // not heard
        d.cutRing[at] = cut;
        d.preRing[(size_t) ((d.ringAt - 1) & (uint32) (4 * latency - 1))] = pre * (float) d.gateGain, d.heldRing[at] = held * (float) d.gateGain;
        if (e > 0.5) cut = d.cutRing[late];
        // the steps stand late by the anti-alias filter's and the hold kernel's delay: what went in is shown as late
        const double lag = f.lag * d.fs + (f.identity ? 0 : kernel().delay);
        const auto crush = e > 0.5 ? late : at, into = (size_t) ((d.ringAt - 1 - (e > 0.5 ? (uint32) latency : 0u) - (uint32) jmin(2.0 * latency - 1, lag + 0.5)) & (uint32) (4 * latency - 1));
        for (int c = 0; c < channels; ++c)
        {
            auto& ring = d.dryRing[c];
            const double delayed = ring[late];
            ring[at] = (float) dry[c];
            if (e > 0)
                dry[c] += e * (delayed - dry[c]), y[c] += e * (coded[c] - y[c]);
        }
        float in = 0, out = 0, lost = 0, outputClip = 0;
        for (int c = 0; c < channels; ++c)
        {
            if (!d.bandPost.idle()) y[c] = d.bandPost.run(c, y[c]);
            x[c][i] = (float) ((dry[c] + mix * (y[c] * wet - dry[c])) * gain);
            in += (float) dry[c] / (float) channels, out += x[c][i] / (float) channels, peak = jmax(peak, std::abs(x[c][i]));
            if (const float t = std::abs(x[c][i]); t > 1 && 20 * std::log10(t) > std::abs(outputClip)) outputClip = std::copysign(20 * std::log10(t), x[c][i]);
            lost += (float) (e * gone[c] * wet) / (float) channels;
        }
        scope.push(w + (uint32_t) i, in, out, lost, e > 0.5 ? (uint8_t) mark : (uint8_t) 0, cut, outputClip, mix > 0 ? d.preRing[into] : in, mix > 0 ? d.heldRing[crush] : in);
    }
    scope.write.store(w + (uint32_t) n, std::memory_order_release);
    scope.loudest(peak);
}

// Writers (presets, A/B, restore) serialize on txLock, which nothing else takes. While one publishes, `busy` makes
// saves return the state committed before it. stateLock only guards that switch and the user preset name, and is
// never held across a host callback or I/O: any thread can always save, and the editor can always read.
struct CrumbProcessor::Transaction
{
    CrumbProcessor& p;
    const ScopedLock writer;
    const bool outer; // a writer nested on the same thread (from a listener) joins the open transaction
    explicit Transaction(CrumbProcessor& owner) : p(owner), writer(p.txLock), outer(!p.busy)
    {
        if (const ScopedLock sl(p.stateLock); outer)
            p.serialize(p.committed), p.busy = true;
    }
    ~Transaction() { const ScopedLock sl(p.stateLock); p.busy = p.busy && !outer; }
};

void CrumbProcessor::getStateInformation(MemoryBlock& dest) { const ScopedLock sl(stateLock); busy ? (void) (dest = committed) : serialize(dest); }

void CrumbProcessor::serialize(MemoryBlock& dest) // under stateLock
{
    auto state = apvts.copyState();
    state.setProperty("stateVersion", stateVersion, nullptr);
    // Additive since 0.1.0 milestone 3 (older readers ignore them): preset, A/B slots ("preset;v0,..,v16" normalised,
    // in sound() order; 0.1.x wrote 16 values, 0.2.x 17), editor size.
    state.setProperty("preset", preset.load(), nullptr);
    if (preset < 0 && userName.isNotEmpty())
        state.setProperty("presetName", userName, nullptr);
    state.setProperty("ab", ab.load(), nullptr);
    for (int k = 0; k < 2; ++k)
        if (!slots[k].empty())
        {
            std::ostringstream t; // 9 significant digits: every float reads back exactly
            t.imbue(std::locale::classic()), t.precision(9), t << slotPreset[k] << ';';
            for (const float& v : slots[k])
                t << v << (&v == &slots[k].back() ? "" : ",");
            state.setProperty(k == 0 ? "slotA" : "slotB", String(t.str()), nullptr);
            state.setProperty(k == 0 ? "slotNameA" : "slotNameB", slotName[k], nullptr);
        }
    if (viz > 0)
        state.setProperty("viz", viz.load(), nullptr);
    if (editorWidth > 0)
        state.setProperty("editorWidth", editorWidth.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, dest);
}

bool crumb::isFlatCrumbXml(std::string_view xml)
{
    if (xml.size() > 1 << 20 || !CharPointer_UTF8::isValidString(xml.data(), (int) xml.size()))
        return false;
    size_t pos = 0;
    auto skip = [&] { while (pos < xml.size() && std::string_view(" \t\r\n").find(xml[pos]) != std::string_view::npos) ++pos; };
    auto name = [&] {
        const size_t start = pos;
        auto first = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':'; };
        if (pos < xml.size() && first(xml[pos]))
            while (++pos < xml.size() && (first(xml[pos]) || (xml[pos] >= '0' && xml[pos] <= '9') || xml[pos] == '-' || xml[pos] == '.')) {}
        return xml.substr(start, pos - start);
    };
    std::string_view stack[2];
    int depth = 0;
    bool complete = false, declaration = false;
    skip();
    while (pos < xml.size() && !complete)
    {
        if (xml[pos++] != '<' || pos == xml.size()) return false;
        const bool decl = xml[pos] == '?', closing = xml[pos] == '/';
        pos += decl || closing ? 1 : 0;
        const auto tag = name();
        if (tag.empty() || (decl && (declaration || depth != 0 || tag != "xml"))) return false;
        if (closing)
        {
            skip();
            if (depth == 0 || stack[depth - 1] != tag || pos == xml.size() || xml[pos++] != '>') return false;
            complete = --depth == 0;
        }
        else
        {
            std::vector<std::string_view> keys;
            for (;;)
            {
                const auto before = pos;
                skip();
                if (pos == xml.size()) return false;
                if (xml[pos] == '/' || xml[pos] == '>' || (decl && xml[pos] == '?')) break;
                if (pos == before) return false; // attributes require separating whitespace
                const auto key = name();
                if (key.empty() || std::find(keys.begin(), keys.end(), key) != keys.end()) return false;
                keys.push_back(key);
                skip();
                if (pos == xml.size() || xml[pos++] != '=') return false;
                skip();
                if (pos == xml.size() || (xml[pos] != '\'' && xml[pos] != '"')) return false;
                const char quote = xml[pos++];
                while (pos < xml.size() && xml[pos] != quote)
                {
                    const auto c = (unsigned char) xml[pos++];
                    if (c == '<' || (c < 32 && c != 9 && c != 10 && c != 13)) return false;
                    if (c != '&') continue;
                    const auto end = xml.find(';', pos);
                    if (end == std::string_view::npos) return false;
                    const auto ref = xml.substr(pos, end - pos);
                    if (ref != "amp" && ref != "lt" && ref != "gt" && ref != "quot" && ref != "apos")
                    {
                        if (ref.empty() || ref[0] != '#') return false; // before substr: "&;" has nothing to skip
                        const bool hex = ref.size() > 1 && ref[1] == 'x';
                        const auto digits = ref.substr(hex ? 2 : 1);
                        if (digits.empty() || digits.size() > 7
                            || digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") != std::string_view::npos)
                            return false;
                        const auto code = std::stoul(std::string(digits), nullptr, hex ? 16 : 10);
                        if (!(code == 9 || code == 10 || code == 13 || (code >= 32 && code <= 0xd7ff) || (code >= 0xe000 && code <= 0xfffd) || (code >= 0x10000 && code <= 0x10ffff))) return false;
                    }
                    pos = end + 1;
                }
                if (pos++ == xml.size()) return false;
            }
            const bool empty = xml[pos] == '/';
            if ((decl || empty) && xml[pos++] != (decl ? '?' : '/')) return false;
            if (pos == xml.size() || xml[pos++] != '>') return false;
            if (decl) declaration = true;
            else
            {
                if (depth == 2 || (depth == 0 && tag != "CRUMB")) return false;
                stack[depth] = tag;
                depth += empty ? 0 : 1;
                complete = depth == 0;
            }
        }
        skip();
    }
    return complete && pos == xml.size();
}

void CrumbProcessor::applyParams(const XmlElement& state, bool bypass)
{
    for (int i = 0; i < numParams; ++i)
    {
        if (i == Bypass && !bypass)
            continue;
        auto* p = apvts.getParameter(ids[i]);
        const XmlElement* c = nullptr; // the first PARAM with this id; other tags are unknown entries, ignored
        for (auto* e : state.getChildWithTagNameIterator("PARAM"))
            if (c == nullptr && e->getStringAttribute("id") == ids[i])
                c = e;
        const auto v = number(c != nullptr ? c->getStringAttribute("value") : String());
        // stateVersion 1 migration: sessions and user presets from 0.1.x have no autoGain and load with it off, as they sounded.
        p->setValueNotifyingHost(v ? p->convertTo0to1(p->getNormalisableRange().snapToLegalValue((float) *v)) : c == nullptr && i == AutoGain ? 0.0f : p->getDefaultValue());
    }
}

void CrumbProcessor::setIdentity(int index, const String& name)
{
    const ScopedLock sl(stateLock);
    preset = index, userName = name;
}

// Malformed documents never mutate state. A restore does not reset the DSP: playback continues with 20 ms smoothing.
void CrumbProcessor::setStateInformation(const void* data, int size)
{
    const auto* bytes = static_cast<const char*>(data);
    if (bytes == nullptr || size < 8 || ByteOrder::littleEndianInt(bytes) != 0x21324356 || ByteOrder::littleEndianInt(bytes + 4) > (uint32) size - 8)
        return;
    const auto tree = decode({ bytes + 8, ByteOrder::littleEndianInt(bytes + 4) });
    if (tree == nullptr)
        return;
    const Transaction transaction(*this);
    // Migration: stateVersion 1 (0.1.x) lacks autoGain, in the parameters and in the A/B slots: both load it off. Version 2
    // (0.2.x) and 1 lack 0.3.0's parameters: all load neutral (the codec and the band off, auto 100 % of the old switch), so
    // the sound is the old one; its filter type and cutoff keep working as parameters. Version 3 (0.3.0) holds every parameter
    // 4 (0.4.0) does: 0.4.0 took auto gain off the panel and turned its default off, but a session that had it on keeps it on
    // and plays as it did, until a factory preset or randomize (a new sound) turns it off. 0.5.0 (still 4) did the same to gate,
    // DC shift, the filter's position and its invert: off the panel, registered, and run unseen as a session stored them.
    applyParams(*tree, true);
    for (int k = 0; k < 2; ++k) // a slot counts only with a preset and all the normalised values of its version
    {
        const auto t = StringArray::fromTokens(tree->getStringAttribute(k == 0 ? "slotA" : "slotB"), ";,", "");
        slots[k].clear(), slotName[k] = tree->getStringAttribute(k == 0 ? "slotNameA" : "slotNameB");
        slotPreset[k] = (int) jlimit(-1.0, crumb::numPresets - 1.0, number(t[0]).value_or(-1));
        for (int i = 1; i < t.size() && (t.size() == 17 || t.size() == 18 || t.size() == numSound + 1); ++i)
            if (const auto v = number(t[i]); v && *v >= 0 && *v <= 1)
                slots[k].push_back((float) *v);
        if (t.size() == 17 && slots[k].size() == 16)
            slots[k].push_back(0); // 0.1.x: auto gain off
        if ((t.size() == 17 || t.size() == 18) && slots[k].size() == 17)
            for (int i = 17; i < numSound; ++i)
                slots[k].push_back(apvts.getParameter(ids[sound(i)])->getDefaultValue()); // 0.2.x: 0.3.0's neutral
        if (slots[k].size() != (size_t) numSound)
            slots[k].clear();
    }
    ab = number(tree->getStringAttribute("ab")).value_or(0) > 0.5 ? 1 : 0;
    viz = (int) jlimit(0.0, (double) (crumb::numViz - 1), number(tree->getStringAttribute("viz")).value_or(0));
    editorWidth = (int) jlimit(0.0, 4096.0, number(tree->getStringAttribute("editorWidth")).value_or(0));
    setIdentity((int) jlimit(-1.0, crumb::numPresets - 1.0, number(tree->getStringAttribute("preset")).value_or(-1)), tree->getStringAttribute("presetName")); // none: "custom"
}

// ================================ Factory presets (constexpr) ================================
// Values: preamp dB, bits (0 off), dcShift, dither %, rate Hz, jitter %, approxOn, approxDev %, imagesOn, imagesShift %,
// fType (0 off 1 lp 2 bp 3 hp 4 br), fOrder (0 pre 1 post), cutoff Hz, reso %, mix %, output dB.
// 1-12: classic 8/12/16-bit sampler flavours (generic names), 13-24: by use. Names are "bits·rate flavour".
// Then 0.3.0's: loss, speed, drop, fill (0 gap 1 hold), keep (0 lost 1 kept), ear (0 ear 1 flat), gate dB (-96 off), stereo
// (0 stereo 1 joint 2 mono), low cut Hz, high cut Hz, slope (0 6 .. 3 24 .. 7 96 dB), invert; 0..24 leave them neutral.
// DC shift, the gate, the filter's position (post) and invert left the panel in 0.5.0: every preset has them at rest. Until
// 0.4.0 ten had DC shift on, three a gate (bad line, phone call, dial tone) and one the filter before (bass, sub grit).
// Auto gain is off in every preset (0.4.0). Until 0.3.0 it was on by default, so each preset that it turned down or up has the
// same change baked in: exactly, drive's share (mix and output; at mix m and drive p the wet share is m / p of 1 - m + m / p),
// and the codec's as a fixed output gain, what it averaged over the demo loop and the tests'
// (it follows the music, so it is near, not the same). Output ends at -24: dial tone and hard crush
// are 2.0 and 4.9 dB louder than they were.
constexpr crumb::Preset crumb::presets[numPresets] = {
    { "init", { 0, 12, 0, 0, 26040, 0, 1, 0, 0, 0, 0, 1, 20000, 0, 100, 0 } },
    { "12\xc2\xb7" "26k dusty", { 4, 12, 0, 0, 26040, 4, 1, 0, 1, 10, 1, 1, 11000, 8, 100, -5 } },
    { "12\xc2\xb7" "40k warm", { 6, 12, 0, 0, 39062.5f, 2, 1, 0, 1, 0, 1, 1, 15000, 5, 100, -8 } },
    { "8\xc2\xb7" "22k grit", { 9, 8, 0, 0, 22050, 6, 1, 0, 0, 0, 0, 1, 20000, 0, 100, -13 } },
    { "16\xc2\xb7" "40k clean", { 0, 16, 0, 30, 40000, 0, 1, 0, 1, 20, 0, 1, 20000, 0, 100, 0 } },
    { "10\xc2\xb7" "dark", { 3, 10, 0, 0, 16000, 3, 1, -30, 1, 0, 1, 1, 5500, 10, 100, -3 } },
    { "12\xc2\xb7" "32k punch", { 12, 12, 0, 0, 31250, 2, 1, 0, 1, 15, 0, 1, 20000, 0, 100, -18 } },
    { "8\xc2\xb7" "11k crunch", { 6, 8, 0, 0, 11025, 5, 0, 0, 0, 0, 0, 1, 20000, 0, 100, -10 } },
    { "12\xc2\xb7" "26k tread", { 2, 12, 0, 0, 26040, 3, 1, 0, 1, 10, 1, 1, 12000, 0, 100, -3 } },
    { "8\xc2\xb7" "16k fold", { 4, 8, 0, 0, 16000, 4, 1, 40, 0, 0, 0, 1, 20000, 0, 100, -7 } },
    { "6\xc2\xb7" "8k toy", { 6, 6, 0, 0, 8000, 8, 0, 0, 0, 0, 0, 1, 20000, 0, 100, -12 } },
    { "14\xc2\xb7" "44k glue", { 5, 14, 0, 40, 44100, 1, 1, 0, 1, 30, 0, 1, 20000, 0, 100, -7 } },
    { "12\xc2\xb7" "20k thick", { 8, 12, 0, 0, 20000, 5, 1, -10, 1, 0, 1, 1, 9000, 15, 100, -11 } },
    { "drums \xc2\xb7 crunch bus", { 10, 10, 0, 0, 22050, 3, 1, 0, 1, 10, 0, 1, 20000, 0, 42.5f, -8.7f } },
    { "drums \xc2\xb7 break dust", { 6, 12, 0, 0, 26040, 6, 1, -10, 1, 0, 1, 1, 9500, 10, 100, -8 } },
    { "bass \xc2\xb7 warm 12", { 8, 12, 0, 0, 26040, 0, 1, -20, 1, 0, 1, 1, 4000, 20, 100, -11 } },
    { "bass \xc2\xb7 sub grit", { 14, 8, 0, 0, 12000, 2, 1, 0, 1, 0, 1, 1, 2500, 25, 23, -9.7f } },
    { "keys \xc2\xb7 lo-fi", { 4, 10, 0, 30, 18000, 12, 1, -20, 1, 0, 1, 1, 6500, 5, 100, -4 } },
    { "vocal \xc2\xb7 radio", { 10, 8, 0, 20, 11025, 0, 1, 0, 1, 0, 2, 1, 1800, 40, 100, -12 } },
    { "vocal \xc2\xb7 phone", { 6, 8, 0, 0, 8000, 0, 1, 0, 1, 0, 2, 1, 1200, 55, 100, -7 } },
    { "hats \xc2\xb7 sizzle", { 0, 6, 0, 0, 30000, 0, 0, 0, 0, 0, 3, 1, 5000, 0, 50, -2 } },
    { "pad \xc2\xb7 drift", { 2, 12, 0, 20, 22050, 60, 1, -10, 1, 20, 1, 1, 8000, 0, 100, -2 } },
    { "fx \xc2\xb7 alias sweep", { 0, 16, 0, 0, 6000, 0, 1, 100, 0, 0, 0, 1, 20000, 0, 100, -3 } },
    { "fx \xc2\xb7 700 hz wreck", { 12, 4, 0, 0, 700, 30, 0, 0, 0, 0, 1, 1, 3000, 30, 100, -20 } },
    { "mix \xc2\xb7 parallel crush", { 12, 8, 0, 0, 16000, 4, 1, 0, 1, 0, 0, 1, 20000, 0, 11.9f, -2.6f } },
    // 0.3.0, from a clean stream to a crushed one
    { "256k stream", { 0, 16, 0, 0, 44100, 0, 1, 0, 0, 0, 0, 1, 20000, 0, 100, 0 }, { 4, 25, 0, 0, 0, 0, -96, 0, 20, 19000, 4, 0 } },
    { "128k stream", { 0, 16, 0, 0, 44100, 0, 1, 0, 0, 0, 0, 1, 20000, 0, 100, 0 }, { 10, 25, 0, 0, 0, 0, -96, 1, 20, 16000, 5, 0 } },
    { "64k stream", { 0, 16, 0, 0, 32000, 0, 1, 0, 0, 0, 0, 1, 20000, 0, 100, 0 }, { 28, 30, 0, 0, 0, 0, -96, 1, 20, 12500, 5, 0 } },
    { "56k stream", { 3, 16, 0, 0, 22000, 4, 1, 0, 0, 0, 0, 1, 20000, 0, 100, 2.6f }, { 70, 40, 6, 1, 0, 0, -96, 0, 160, 6300, 3, 0 } },
    { "32k swirl", { 0, 16, 0, 0, 22050, 0, 1, 0, 1, 0, 0, 1, 20000, 0, 100, 0.3f }, { 55, 12, 0, 0, 0, 0, -96, 1, 20, 9000, 3, 0 } },
    { "slow smear", { 0, 0, 0, 0, 44100, 0, 1, 0, 0, 0, 0, 1, 20000, 0, 100, 0.5f }, { 62, 0, 0, 0, 0, 0, -96, 0, 40, 14000, 1, 0 } },
    { "fast garble", { 4, 14, 0, 0, 26040, 12, 1, 0, 1, 10, 0, 1, 20000, 0, 100, 9.4f }, { 78, 100, 0, 0, 0, 0, -96, 0, 90, 9000, 1, 0 } },
    { "bad line", { 2, 13, 0, 0, 16000, 0, 1, 0, 1, 0, 0, 1, 20000, 10, 100, -1.8f }, { 35, 50, 30, 0, 0, 0, -96, 2, 200, 3800, 5, 0 } },
    { "stuck buffer", { 0, 16, 0, 0, 44100, 0, 1, 0, 0, 0, 0, 1, 20000, 0, 100, 0 }, { 12, 12, 45, 1, 0, 0, -96, 1, 20, 20000, 3, 0 } },
    { "phone call", { 6, 12, 0, 0, 8000, 0, 1, 0, 1, 0, 0, 1, 20000, 15, 100, -1 }, { 45, 62, 4, 1, 0, 0, -96, 2, 400, 3150, 5, 0 } },
    { "only the lost", { 0, 16, 0, 0, 44100, 0, 1, 0, 0, 0, 0, 1, 20000, 0, 100, 9.9f }, { 38, 25, 0, 0, 1, 0, -96, 0, 20, 20000, 3, 0 } },
    { "flat codec", { 3, 14, 0, 0, 32000, 0, 1, 0, 1, 0, 0, 1, 20000, 0, 100, -3.8f }, { 66, 50, 0, 0, 0, 1, -96, 0, 20, 20000, 3, 0 } },
    { "8 bit cast", { 8, 8, 0, 20, 22050, 5, 1, 0, 1, 0, 0, 1, 20000, 0, 100, -9.3f }, { 40, 38, 0, 0, 0, 0, -96, 1, 70, 8000, 3, 0 } },
    { "torn tape", { 6, 10, 0, 0, 18000, 45, 1, -10, 1, 0, 0, 1, 20000, 0, 100, -7.9f }, { 30, 12, 22, 0, 0, 0, -96, 0, 60, 7000, 1, 0 } },
    { "dial tone", { 18, 5, 0, 0, 6000, 8, 0, 0, 0, 0, 0, 1, 20000, 35, 100, -24 }, { 82, 75, 10, 1, 0, 1, -96, 2, 300, 3400, 4, 0 } },
    { "hard crush", { 24, 3, 0, 0, 4000, 20, 0, 0, 0, 0, 0, 1, 20000, 0, 100, -24 }, { 60, 88, 8, 1, 0, 1, -96, 0, 60, 12000, 1, 0 } },
};
constexpr int crumb::presetOrder[numPresets] = { 0, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
const std::vector<crumb::PresetGroup>& crumb::presetGroups()
{
    static const std::vector<PresetGroup> groups {
        { "clean", { 0, 4, 11, 25, 26 } },
        { "sampler", { 1, 2, 5, 6, 8, 12 } },
        { "crunch", { 3, 7, 9, 10, 37, 40 } },
        { "stream", { 27, 28, 29, 30, 31, 36, 35 } },
        { "phone", { 34, 32, 39, 18, 19 } },
        { "glitch", { 33, 38, 22, 23 } },
        { "instrument", { 13, 14, 20, 15, 16, 17, 21, 24 } },
    };
    return groups;
}

void CrumbProcessor::setAll(const float* normalised)
{
    for (int k = 0; k < numSound; ++k)
        if (auto* p = apvts.getParameter(ids[sound(k)]); std::abs(p->getValue() - normalised[k]) > 1e-7f)
            p->beginChangeGesture(), p->setValueNotifyingHost(normalised[k]), p->endChangeGesture();
}

void CrumbProcessor::setCurrentProgram(int index)
{
    if (!isPositiveAndBelow(index, crumb::numPresets))
        return;
    const auto& v = crumb::presets[index].v;
    const auto& more = crumb::presets[index].x;
    float n[numSound];
    for (int k = 0; k < numSound; ++k)
    {
        const int i = sound(k);
        auto* p = apvts.getParameter(ids[i]);
        n[k] = i == AutoGain ? 0.0f : i == Auto ? p->getDefaultValue() : p->convertTo0to1(i < Bypass ? v[i] : more[i - Loss]);
    }
    {
        const Transaction transaction(*this);
        setAll(n);
        setIdentity(index, {});
    }
    updateHostDisplay(ChangeDetails().withProgramChanged(true)); // published: a host query here sees the new state
}

const String CrumbProcessor::getProgramName(int index) { return isPositiveAndBelow(index, crumb::numPresets) ? String::fromUTF8(crumb::presets[index].name) : String(); }

File CrumbProcessor::presetFolder() const { return presetDirectory != File() ? presetDirectory : File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("daveydidit/crumb/Presets"); }

String CrumbProcessor::presetName() const
{
    const ScopedLock sl(stateLock);
    return preset >= 0 ? String::fromUTF8(crumb::presets[preset].name) : userName.isEmpty() ? String("custom") : userName;
}

StringArray CrumbProcessor::userPresets() const
{
    auto files = presetFolder().findChildFiles(File::findFiles, false, "*.xml");
    files.sort();
    StringArray names;
    for (const auto& f : files) names.add(f.getFileNameWithoutExtension());
    return names;
}

void CrumbProcessor::loadUserPreset(const String& name) // a user file: parameters, no bypass
{
    if (const auto xml = decode(presetFolder().getChildFile(name + ".xml").loadFileAsString().toStdString()))
    {
        const Transaction transaction(*this);
        applyParams(*xml, false);
        setIdentity(-1, name);
    }
}

void CrumbProcessor::stepPreset(int delta)
{
    const auto files = userPresets();
    const int n = crumb::numPresets + files.size();
    int at = 0; // an unknown position steps from "init"
    if (preset >= 0)
        at = (int) (std::find(crumb::presetOrder, crumb::presetOrder + crumb::numPresets, preset.load()) - crumb::presetOrder);
    else if (const int k = files.indexOf(presetName()); k >= 0)
        at = crumb::numPresets + k;
    at = (at + delta + n) % n;
    if (at < crumb::numPresets) setCurrentProgram(crumb::presetOrder[at]);
    else loadUserPreset(files[at - crumb::numPresets]);
}

// A new sound: everything on the panel but mix and output; what left the panel (auto gain, gate, DC shift, invert, the
// filter's position) is at rest (without auto gain the drive reaches 9 dB, half what 0.3.0's did). The order of the draws is part of the site's port (silt's randomize, juce::Random).
void CrumbProcessor::randomize(int64_t seed)
{
    Random r(seed);
    r.nextInt();
    float v[numParams];
    for (int i = 0; i < numParams; ++i) v[i] = apvts.getParameter(ids[i])->convertFrom0to1(apvts.getParameter(ids[i])->getDefaultValue());
    const auto log = [&](float lo, float hi) { return lo * std::pow(hi / lo, r.nextFloat()); };
    v[Preamp] = r.nextFloat() < 0.5f ? 0.0f : r.nextFloat() * 9;
    v[Bits] = r.nextFloat() < 0.2f ? 0.0f : (float) (3 + r.nextInt(14));
    v[Rate] = r.nextFloat() < 0.2f ? 44100.0f : log(2500, 44100);
    v[Jitter] = r.nextFloat() < 0.6f ? 0.0f : r.nextFloat() * 50;
    v[ImagesOn] = (float) r.nextInt(2);
    v[Loss] = r.nextFloat() < 0.2f ? 0.0f : r.nextFloat() * 90;
    v[Speed] = r.nextFloat() * 100;
    v[Drop] = r.nextFloat() < 0.6f ? 0.0f : r.nextFloat() * 35;
    v[Fill] = (float) r.nextInt(2);
    v[Ear] = r.nextFloat() < 0.75f ? 0.0f : 1.0f;
    v[Stereo] = (float) r.nextInt(2);
    v[LowCut] = r.nextFloat() < 0.5f ? 20.0f : log(40, 400);
    v[HighCut] = r.nextFloat() < 0.5f ? 20000.0f : log(2500, 16000);
    v[Slope] = (float) (1 + r.nextInt(5));
    for (int i : { Mix, Output }) v[i] = apvts.getParameter(ids[i])->convertFrom0to1(apvts.getParameter(ids[i])->getValue());
    float n[numSound];
    for (int k = 0; k < numSound; ++k) n[k] = apvts.getParameter(ids[sound(k)])->convertTo0to1(v[sound(k)]);
    const Transaction transaction(*this);
    setAll(n);
    setIdentity(-1, "random");
}

// A 0.2 session's filter as the panel shows it: lp = high cut, hp = low cut, bp = both, br = both, inverted.
CrumbProcessor::BandView CrumbProcessor::band() const
{
    const auto p = [this](int i) { return apvts.getRawParameterValue(ids[i])->load(); };
    const int type = (int) p(FType);
    if (type == 0) return { p(LowCut), p(HighCut), p(Invert) > 0.5f, false };
    return { type == 1 ? 20.0f : p(Cutoff), type == 3 ? 20000.0f : p(Cutoff), type == 4, true };
}
void CrumbProcessor::adoptBand()
{
    const auto b = band();
    if (!b.legacy) return;
    const Transaction transaction(*this);
    const std::pair<int, float> to[] { { LowCut, b.low }, { HighCut, b.high }, { Invert, b.invert ? 1.0f : 0.0f }, { Slope, 1.0f }, { FType, 0.0f } };
    for (auto [i, v] : to)
    {
        auto* p = apvts.getParameter(ids[i]);
        p->beginChangeGesture(), p->setValueNotifyingHost(p->convertTo0to1(v)), p->endChangeGesture();
    }
}
File CrumbProcessor::saveUserPreset()
{
    const ScopedLock writer(txLock);
    const auto folder = presetFolder();
    folder.createDirectory();
    File file;
    for (int k = 1; k < 1000 && (file == File() || file.exists()); ++k)
        file = folder.getChildFile("user " + String(k).paddedLeft('0', 2) + ".xml");
    if (file.exists())
        return {};
    auto xml = apvts.copyState().createXml();
    if (xml == nullptr || !xml->writeTo(file))
        return {};
    setIdentity(-1, file.getFileNameWithoutExtension());
    return file;
}

void CrumbProcessor::selectAb(int slot)
{
    const Transaction transaction(*this);
    const int from = ab;
    if (slot == from || !isPositiveAndBelow(slot, 2))
        return;
    slots[from].resize(numSound);
    for (int k = 0; k < numSound; ++k)
        slots[from][(size_t) k] = apvts.getParameter(ids[sound(k)])->getValue();
    slotPreset[from] = preset, slotName[from] = userName;
    if (!slots[slot].empty()) // never visited: it starts as a copy of the other slot
        setAll(slots[slot].data()), setIdentity(slotPreset[slot], slotName[slot]);
    ab = slot;
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CrumbProcessor(); }
