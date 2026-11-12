#include "Crumb.h"
#include <optional>
#include <sstream>

using namespace juce;

namespace
{
// ============================ FROZEN — never rename/reorder semantics ============================
// Hosts store sessions and automation under these string IDs (ParameterID version hint 1). Never rename,
// remove, reorder or change a range so that old values move. New parameters are APPENDED only.
// Planned stereo independence (pending David): append `link` (default ON) and per-channel twins such as
// `rateR`, `bitsR`, ... after "bypass". Old states lack them, so they load at their defaults = linked,
// both channels identical. The state parser below already resets missing IDs to defaults.
const char* const ids[] = { "preamp", "bits",     "dcShift", "dither", "rate",   "jitter", "approxOn", "approxDev", "imagesOn",
                            "imagesShift", "fType", "fOrder", "cutoff", "reso", "mix",    "output", "bypass" };
// =================================================================================================
constexpr int numParams = 17;
enum { Preamp, Bits, DcShift, Dither, Rate, Jitter, ApproxOn, ApproxDev, ImagesOn, ImagesShift, FType, FOrder, Cutoff, Reso, Mix, Output, Bypass };
// Smoothed per-sample controls. Rate and cutoff are smoothed as log2(Hz); the last four mix the filter outputs.
enum { SPre, SRate, SDither, SJitter, SApprox, SDev, SImages, SShift, SCut, SReso, SOrder, SMix, SOut, SBypass, SDry, SLP, SBP, SHP, numSmoothed };
constexpr double pi = MathConstants<double>::pi;
constexpr int taps = 64, over = 64, ringMask = taps - 1; // minBLEP length (host samples) and table oversampling

AudioProcessorValueTreeState::ParameterLayout layout()
{
    // Every float snaps to 0.1 of its unit: save -> reload is then bit-exact on every platform (as in Snap).
    const auto snap = [](float s, float e, float v) { return jlimit(s, e, std::round(v * 10) / 10 + 0.0f); };
    // Display text as on the panel ("26.0k", "+12 dB", "35%"); typed text may use k for kHz.
    auto trim = [](float v) { return String(v, std::abs(v - std::round(v)) < 0.05f ? 0 : 1); };
    auto text = [trim](const char* unit) {
        return [unit = String(unit), trim](float v, int) {
            if (unit == "Hz")
                return v >= 1000 ? String(v / 1000, 1) + "k" : String(roundToInt(v)) + " Hz";
            if (unit.endsWith("dB")) // preamp "+12 dB", output "-3.5 dB"
                return (v > 0 ? "+" : "") + (unit == "dB" ? String(v, 1) : trim(v)) + " dB";
            return (unit == "+%" && v > 0 ? "+" : "") + trim(v) + (unit == "+%" ? "" : "%");
        };
    };
    auto parse = [](const String& t) { return (float) (t.replace(CharPointer_UTF8("\xe2\x88\x92"), "-").retainCharacters("+-.0123456789").getDoubleValue() * (t.containsIgnoreCase("k") ? 1000 : 1)); };
    auto num = [&](int i, const char* name, float lo, float hi, float def, const char* unit, bool log = false) {
        NormalisableRange<float> r(lo, hi, nullptr, nullptr, snap);
        if (log)
            r = NormalisableRange<float>(lo, hi, [](float s, float e, float v) { return s * std::pow(e / s, v); },
                                         [](float s, float e, float v) { return std::log(v / s) / std::log(e / s); }, snap);
        r.interval = log ? 0.0f : 0.1f;
        return std::make_unique<AudioParameterFloat>(ParameterID { ids[i], 1 }, name, r, def, AudioParameterFloatAttributes()
                                                                          .withLabel(String(unit).removeCharacters("+"))
                                                                          .withStringFromValueFunction(text(unit))
                                                                          .withValueFromStringFunction(parse));
    };
    auto onOff = [](int i, const char* name, bool def) { return std::make_unique<AudioParameterBool>(ParameterID { ids[i], 1 }, name, def); };
    auto choice = [](int i, const char* name, StringArray items, int def) {
        return std::make_unique<AudioParameterChoice>(ParameterID { ids[i], 1 }, name, items, def);
    };
    auto bitsText = AudioParameterIntAttributes().withStringFromValueFunction([](int v, int) { return v == 0 ? String("OFF") : String(v) + " bit"; });
    return { num(Preamp, "Preamp", 0, 48, 0, "+dB"),
             std::make_unique<AudioParameterInt>(ParameterID { ids[Bits], 1 }, "Resolution", 0, 24, 12, bitsText),
             onOff(DcShift, "DC Shift", false),
             num(Dither, "Dither", 0, 100, 0, "%"),
             num(Rate, "Rate", 44, 44100, 26040, "Hz", true),
             num(Jitter, "Jitter", 0, 100, 0, "%"),
             onOff(ApproxOn, "Approx Filter", true),
             num(ApproxDev, "Approx Deviation", -100, 100, 0, "+%"),
             onOff(ImagesOn, "Images Filter", false),
             num(ImagesShift, "Images Shift", 0, 100, 0, "%"),
             choice(FType, "Filter Type", { "OFF", "LP", "BP", "HP", "BR" }, 0),
             choice(FOrder, "Filter Order", { "Pre", "Post" }, 1),
             num(Cutoff, "Cutoff", 20, 20000, 20000, "Hz", true),
             num(Reso, "Reso / Bandwidth", 0, 100, 0, "%"),
             num(Mix, "Mix", 0, 100, 100, "%"),
             num(Output, "Output", -24, 12, 0, "dB"),
             onOff(Bypass, "Bypass", false) };
}

// Minimum-phase lowpass sampled `over` times per host sample over `taps` samples: its impulse (unit DC gain per host
// sample) and its integral, the minBLEP step (0 -> 1). Blackman-Harris windowed sinc whose stopband starts at the host
// Nyquist, made minimum phase through the real cepstrum (Oppenheim & Schafer). Being causal, it reconstructs the input
// between host samples (for the virtual ADC) and bandlimits the hold steps (for the DAC) with zero latency.
struct Kernel
{
    std::vector<double> impulse, step;
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
        return k;
    }();
    return table;
}

// Zavalishin TPT state-variable filter (topology-preserving transform, trapezoidal integration).
struct Coef
{
    double k = 2, a1 = 1, a2 = 0, a3 = 0;
};
Coef svf(double g, double k)
{
    Coef c { k, 1 / (1 + g * (g + k)), 0, 0 };
    c.a2 = g * c.a1, c.a3 = g * c.a2;
    return c;
}
struct Svf
{
    double s1 = 0, s2 = 0;
    std::array<double, 3> run(double x, const Coef& c) // lowpass, bandpass, highpass
    {
        const double v3 = x - s2, v1 = c.a1 * s1 + c.a2 * v3, v2 = s2 + c.a2 * s1 + c.a3 * v3;
        s1 = 2 * v1 - s1, s2 = 2 * v2 - s2;
        return { v2, v1, x - c.k * v1 - v2 };
    }
};
// Butterworth damping 2cos((2i+1)pi/2N) of the cascaded sections: 8th-order approx filter, 4th-order images filter.
constexpr double approxK[] = { 1.9615705608, 1.6629392246, 1.1111404660, 0.3901806440 }, imagesK[] = { 1.8477590650, 0.7653668647 };

// ADC soft clip: identity up to the knee, then a tanh shoulder towards +-1. Its residual (clip - x) and the
// residual's antiderivative drive first-order antiderivative antialiasing (ADAA) without touching clean signals.
constexpr double knee = 0.5, soft = 1 - knee;
double clipResidual(double x)
{
    const double a = std::abs(x);
    return a <= knee ? 0 : std::copysign(knee + soft * std::tanh((a - knee) / soft), x) - x;
}
double clipIntegral(double x)
{
    const double a = std::abs(x), u = (a - knee) / soft; // log cosh(u) written so it cannot overflow
    return a <= knee ? 0 : knee * a + soft * soft * (u + std::log1p(std::exp(-2 * u)) - std::log(2.0)) - 0.5 * (a * a + knee * knee);
}

double lerp(const double* table, double p)
{
    const int i = (int) p;
    return table[i] + (p - i) * (table[i + 1] - table[i]);
}

struct Frame
{
    double v[numSmoothed] {}, inc = 1, dcCoef = 0, jitCoef = 0, jitNorm = 0;
    Coef approx[4], images[2], filter;
    int bits = 0;
    bool tread = false, identity = false;
};

struct Channel
{
    Svf pre, post, approx[4], images[2];
    double adcPrev = 0, adcPrevD = 0, hist[taps] {}, ring[taps] {}, phase = 0, held = 0, dc = 0, jw = 0;
    const double *impulse = kernel().impulse.data(), *blep = kernel().step.data();
    int pos = 0; // shared by hist (newest sample) and ring (current output sample)
    uint32 seed = 1;

    double random() { return (double) ((seed = seed * 1664525u + 1013904223u) >> 8) * 0x1p-24; } // LCG, [0, 1)
    double filter(Svf& s, double x, const Frame& f)
    {
        const auto o = s.run(x, f.filter);
        return f.v[SDry] * x + f.v[SLP] * o[0] + f.v[SBP] * f.filter.k * o[1] + f.v[SHP] * o[2];
    }
    // Converter at one virtual sample instant: dither (<= 1/2 LSB) -> quantizer -> DAC with fixed vintage
    // ladder errors on the four MSBs. A slow tracker removes only the DC the converter adds (mid-riser idle offset).
    void hold(double v, const Frame& f)
    {
        double q = v;
        if (f.bits > 0)
        {
            static constexpr double ladder[] = { 0.6, -0.4, 0.3, -0.2 }; // weight errors of the MSBs (two's complement), in LSB
            const double lsb = std::ldexp(1.0, 1 - f.bits), half = std::ldexp(1.0, f.bits - 1);
            const double u = (v + f.v[SDither] * lsb * (random() - 0.5)) / lsb;
            const double k = jlimit(-half, half - 1, f.tread ? std::round(u) : std::floor(u)); // mid-tread / mid-riser
            q = (k + (f.tread ? 0 : 0.5)) * lsb;
            for (int j = 0, code = (int) k; j < jmin(4, f.bits); ++j) // code 0 = silence has no error
                q += ((code >> (f.bits - 1 - j)) & 1) * ladder[j] * lsb;
        }
        dc += f.dcCoef * (q - v - dc);
        held = q - dc;
    }
    double process(double x, const Frame& f)
    {
        // Preamp into the ADC clip (ADAA), filter[pre], approx (anti-alias) lowpass.
        const double a = jlimit(-64.0, 64.0, x * f.v[SPre]), aD = clipIntegral(a);
        double y = a + (std::abs(a - adcPrev) < 1e-6 ? clipResidual(0.5 * (a + adcPrev)) : (aD - adcPrevD) / (a - adcPrev));
        adcPrev = a, adcPrevD = aD;
        y = jlimit(-1.0, 1.0, y);
        y += (1 - f.v[SOrder]) * (filter(pre, y, f) - y);
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
            phase = 0, hold(y, f);
        else if ((phase += inc) >= 1)
        {
            phase -= 1;
            const double d = phase / inc, old = held; // the tick was d samples ago, d in [0, 1)
            double v = 0;
            for (int j = d > 0 ? 1 : 0; j < taps; ++j) // input at time n - d from samples n - j, j - d >= 0
                v += hist[(pos - j) & ringMask] * lerp(impulse, (j - d) * over);
            hold(v, f);
            for (int j = 0; j < taps; ++j)
                ring[(pos + j) & ringMask] += (held - old) * (lerp(blep, (j + d) * over) - 1);
        }
        y = held + ring[pos];
        ring[pos] = 0, pos = (pos + 1) & ringMask;

        // Images (reconstruction) lowpass, filter[post].
        lp = y;
        for (int k = 0; k < 2; ++k)
            lp = images[k].run(lp, f.images[k])[0];
        y += f.v[SImages] * (lp - y);
        return y + f.v[SOrder] * (filter(post, y, f) - y);
    }
};
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
    double fs = 48000;

    void targets(bool jump)
    {
        auto p = [this](int i) { return (double) raw[(size_t) i]->load(); };
        const int type = (int) p(FType);
        const double t[numSmoothed] = { Decibels::decibelsToGain(p(Preamp)), std::log2(p(Rate)), p(Dither) / 100, p(Jitter) / 100, p(ApproxOn),
                                        p(ApproxDev) / 100, p(ImagesOn), p(ImagesShift) / 100, std::log2(p(Cutoff)), p(Reso) / 100, p(FOrder),
                                        p(Mix) / 100, Decibels::decibelsToGain(p(Output)), p(Bypass), type == 0 ? 1.0 : 0.0,
                                        type == 1 || type == 4 ? 1.0 : 0.0, type == 2 ? 1.0 : 0.0, type == 3 || type == 4 ? 1.0 : 0.0 };
        for (int k = 0; k < numSmoothed; ++k)
            jump ? smooth[(size_t) k].setCurrentAndTargetValue(t[k]) : smooth[(size_t) k].setTargetValue(t[k]);
    }
    void next(Frame& f)
    {
        for (int k = 0; k < numSmoothed; ++k)
            f.v[k] = smooth[(size_t) k].getNextValue();
        const double fr = jmin(fs, std::exp2(f.v[SRate])), nyquist = 0.49 * fs;
        auto g = [&](double hz) { return std::tan(pi * jmin(nyquist, hz) / fs); };
        f.inc = fr / fs, f.identity = f.inc >= 1 - 1e-9 && f.v[SJitter] <= 0, f.dcCoef = jmin(1.0, 2 * pi * 5 / fr);
        const double ga = g(0.5 * fr * std::exp2(f.v[SDev])), gi = g(0.5 * fr * std::exp2(2 * f.v[SShift])); // Nyquist(fr) * 2^oct
        for (int k = 0; k < 4; ++k)
            f.approx[k] = svf(ga, approxK[k]);
        for (int k = 0; k < 2; ++k)
            f.images[k] = svf(gi, imagesK[k]);
        f.filter = svf(g(std::exp2(f.v[SCut])), 2 * std::pow(40.0, -f.v[SReso])); // Q 0.5 .. 20
    }
};

CrumbProcessor::CrumbProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo()).withOutput("Output", AudioChannelSet::stereo())),
      apvts(*this, nullptr, "CRUMB", layout()), dsp(std::make_unique<Dsp>())
{
    for (int i = 0; i < numParams; ++i)
        dsp->raw[(size_t) i] = apvts.getRawParameterValue(ids[i]);
}

CrumbProcessor::~CrumbProcessor() = default;

AudioProcessorParameter* CrumbProcessor::getBypassParameter() const { return apvts.getParameter(ids[Bypass]); }

bool CrumbProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet();
    return in == l.getMainOutputChannelSet() && (in == AudioChannelSet::mono() || in == AudioChannelSet::stereo());
}

void CrumbProcessor::prepareToPlay(double fs, int)
{
    auto& d = *dsp;
    d.fs = fs > 0 && fs < 1.0e7 ? fs : 48000.0; // a nonsense rate (NaN, inf, <= 0) must not reach the DSP
    sampleRate = d.fs;
    for (uint32 c = 0; c < 2; ++c) // fresh, reproducible state: an independent fixed seed per channel
    {
        d.ch[c] = Channel();
        d.ch[c].seed = 0x9e3779b9u * (c + 1);
    }
    for (auto& s : d.smooth)
        s.reset(d.fs, 0.02);
    d.targets(true);
}

void CrumbProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples(), channels = jmin(2, getTotalNumInputChannels(), buffer.getNumChannels());
    for (int c = channels; c < buffer.getNumChannels(); ++c)
        buffer.clear(c, 0, n);
    auto& d = *dsp;
    d.targets(false);
    Frame f;
    f.bits = (int) d.raw[Bits]->load(), f.tread = d.raw[DcShift]->load() > 0.5f;
    f.jitCoef = 1 - std::exp(-2 * pi * 30 / d.fs), f.jitNorm = 0.5 / std::sqrt(f.jitCoef / (3 * (2 - f.jitCoef))); // ~5 ms walk, +-2 sigma
    float* x[2] = { buffer.getWritePointer(0), buffer.getWritePointer(channels - 1) };
    const int w = scope.write.load(std::memory_order_relaxed);
    float peak[2] {};
    for (int i = 0; i < n && channels > 0; ++i)
    {
        d.next(f);
        const double on = 1 - f.v[SBypass], mix = f.v[SMix] * on, gain = 1 + on * (f.v[SOut] - 1);
        float in = 0, out = 0;
        for (int c = 0; c < channels; ++c) // non-finite input becomes silence; absurd levels are clipped
        {
            const double dry = std::isfinite(x[c][i]) ? jlimit(-1000.0f, 1000.0f, x[c][i]) : 0.0f;
            x[c][i] = (float) ((dry + mix * (d.ch[(size_t) c].process(dry, f) - dry)) * gain);
            in += (float) dry / (float) channels, out += x[c][i] / (float) channels, peak[c] = jmax(peak[c], std::abs(x[c][i]));
        }
        scope.push(w + i, in, out);
    }
    scope.write.store(w + n, std::memory_order_release);
    for (int c = 0; c < 2; ++c) // the editor takes the peak (exchange with 0); a race only loses one block's peak
        scope.peak[c].store(jmax(scope.peak[c].load(std::memory_order_relaxed), peak[jmin(c, channels - 1)]), std::memory_order_relaxed);
}

void CrumbProcessor::getStateInformation(MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty("stateVersion", stateVersion, nullptr);
    // Additive since 0.1.0 milestone 3 (older readers ignore them): preset, A/B slots ("preset;v0,..,v15" normalised), editor size.
    const ScopedLock sl(lock);
    state.setProperty("preset", preset.load(), nullptr);
    if (preset < 0 && userName.isNotEmpty())
        state.setProperty("presetName", userName, nullptr);
    state.setProperty("ab", ab, nullptr);
    for (int k = 0; k < 2; ++k)
        if (!slots[k].empty())
        {
            std::ostringstream t; // 9 significant digits: every float reads back exactly
            t.imbue(std::locale::classic()), t.precision(9), t << slotPreset[k] << ';';
            for (const float& v : slots[k])
                t << v << (&v == &slots[k].back() ? "" : ",");
            state.setProperty(k == 0 ? "slotA" : "slotB", String(t.str()), nullptr);
        }
    if (editorWidth > 0)
        state.setProperty("editorWidth", editorWidth.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, dest);
}

void CrumbProcessor::setStateInformation(const void* data, int size) { parse(data, size, false); }

// Reads the flat <CRUMB a=".."><PARAM id=".." value=".."/>..</CRUMB> text inside JUCE's binary envelope with a single
// non-recursive scan instead of JUCE's XML parser (which recurses per level, expands DTD entities and over-reads
// truncated character references). JUCE's writer escapes '<', '>' and '"' inside values, so every state it writes
// splits cleanly into tags and attributes. Unknown entries are ignored, values are clamped, missing or unparsable
// ones reset to default; a blob that is not one complete <CRUMB> element (or holds "<!") leaves the state alone.
// paramsOnly (user preset files) skips the root's preset, A/B and editor attributes.
bool CrumbProcessor::parse(const void* data, int size, bool paramsOnly)
{
    const auto* bytes = static_cast<const char*>(data);
    if (bytes == nullptr || size < 8 || ByteOrder::littleEndianInt(bytes) != 0x21324356 || ByteOrder::littleEndianInt(bytes + 4) > (uint32) size - 8)
        return false;
    const std::string_view xml(bytes + 8, ByteOrder::littleEndianInt(bytes + 4)), space(" \t\r\n");
    const char* const rootKeys[] = { "preset", "presetName", "ab", "slotA", "slotB", "editorWidth" };
    std::optional<std::string_view> found[numParams], root[6];
    int depth = 0;
    bool complete = false;
    for (size_t open = xml.find('<'), close = 0; !complete && open != std::string_view::npos; open = xml.find('<', close))
    {
        if ((close = xml.find('>', open)) == std::string_view::npos)
            return false;
        auto tag = xml.substr(open + 1, close - open - 1);
        if (tag.empty() || tag[0] == '!' || (tag[0] == '/' && depth == 0))
            return false;
        if (tag[0] == '?' || tag[0] == '/') // the <?xml ?> declaration, or a closing tag
        {
            complete = tag[0] == '/' && --depth == 0;
            continue;
        }
        const bool empty = tag.back() == '/';
        tag.remove_suffix(empty ? 1 : 0);
        const auto name = tag.substr(0, tag.find_first_of(space));
        if (depth == 0 && name != "CRUMB")
            return false;
        std::string_view id, value;
        if ((depth == 1 && name == "PARAM") || depth == 0)
            for (auto rest = tag.substr(name.size());;) // attributes: name="text"
            {
                const auto eq = rest.find("=\""), end = eq == std::string_view::npos ? eq : rest.find('"', eq + 2);
                if (end == std::string_view::npos)
                    break;
                auto key = rest.substr(0, eq);
                key.remove_prefix(std::min(key.size(), key.find_first_not_of(space)));
                const auto text = rest.substr(eq + 2, end - eq - 2);
                id = key == "id" ? text : id, value = key == "value" ? text : value;
                for (int k = 0; k < 6 && depth == 0; ++k)
                    if (key == rootKeys[k] && !root[k])
                        root[k] = text;
                rest.remove_prefix(end + 1);
            }
        for (int i = 0; i < numParams && depth == 1; ++i)
            if (id == ids[i] && !found[i]) // the first entry wins
                found[i] = value;
        depth += empty ? 0 : 1;
        complete = depth == 0;
    }
    if (!complete)
        return false;
    auto number = [](std::optional<std::string_view> t) -> std::optional<double> {
        std::istringstream in(std::string(t.value_or(""))); // locale-independent, reports overflow
        in.imbue(std::locale::classic());
        double v = 0;
        return t && in >> v && (in >> std::ws).eof() ? std::optional(jlimit(-1.0e6, 1.0e6, v)) : std::nullopt;
    };
    // Migration hook: stateVersion 1 is the only format so far. When a later version changes the meaning of stored
    // data, read the root's stateVersion here and translate `found` before it is applied (append-only IDs need nothing).
    for (int i = 0; i < numParams; ++i)
    {
        auto* p = apvts.getParameter(ids[i]);
        const auto v = number(found[i]);
        if (!(paramsOnly && i == Bypass))
            p->setValueNotifyingHost(v ? p->convertTo0to1(p->getNormalisableRange().snapToLegalValue((float) *v)) : p->getDefaultValue());
    }
    if (paramsOnly)
        return true;
    const ScopedLock sl(lock);
    preset = (int) jlimit(-1.0, 24.0, number(root[0]).value_or(-1)); // a state from before presets: "custom"
    userName = String(std::string(root[1].value_or(""))).retainCharacters("abcdefghijklmnopqrstuvwxyz0123456789 _-").substring(0, 40);
    ab = number(root[2]).value_or(0) > 0.5 ? 1 : 0;
    for (int k = 0; k < 2; ++k) // a slot counts only with a preset and 16 normalised values
    {
        const auto t = StringArray::fromTokens(String(std::string(root[3 + k].value_or(""))), ";,", "");
        slots[k].clear(), slotName[k] = {};
        slotPreset[k] = (int) jlimit(-1.0, 24.0, number(t[0].toStdString()).value_or(-1));
        for (int i = 1; i < t.size() && t.size() == 17; ++i)
            if (const auto v = number(t[i].toStdString()); v && *v >= 0 && *v <= 1)
                slots[k].push_back((float) *v);
        if (slots[k].size() != 16)
            slots[k].clear();
    }
    editorWidth = (int) jlimit(0.0, 4096.0, number(root[5]).value_or(0));
    return true;
}

// ================================ Factory presets (constexpr) ================================
// Values: preamp dB, bits (0 off), dcShift, dither %, rate Hz, jitter %, approxOn, approxDev %, imagesOn, imagesShift %,
// fType (0 off 1 lp 2 bp 3 hp 4 br), fOrder (0 pre 1 post), cutoff Hz, reso %, mix %, output dB.
// 1-12: classic 8/12/16-bit sampler flavours (generic names), 13-24: by use. Names are "bits·rate flavour".
constexpr crumb::Preset crumb::presets[25] = {
    { "init", { 0, 12, 0, 0, 26040, 0, 1, 0, 0, 0, 0, 1, 20000, 0, 100, 0 } },
    { "12\xc2\xb7" "26k dusty", { 4, 12, 0, 0, 26040, 4, 1, 0, 1, 10, 1, 1, 11000, 8, 100, -1 } },
    { "12\xc2\xb7" "40k warm", { 6, 12, 0, 0, 39062.5f, 2, 1, 0, 1, 0, 1, 1, 15000, 5, 100, -2 } },
    { "8\xc2\xb7" "22k grit", { 9, 8, 0, 0, 22050, 6, 1, 0, 0, 0, 0, 1, 20000, 0, 100, -4 } },
    { "16\xc2\xb7" "40k clean", { 0, 16, 0, 30, 40000, 0, 1, 0, 1, 20, 0, 1, 20000, 0, 100, 0 } },
    { "10\xc2\xb7" "dark", { 3, 10, 0, 0, 16000, 3, 1, -30, 1, 0, 1, 1, 5500, 10, 100, 0 } },
    { "12\xc2\xb7" "32k punch", { 12, 12, 0, 0, 31250, 2, 1, 0, 1, 15, 0, 1, 20000, 0, 100, -6 } },
    { "8\xc2\xb7" "11k crunch", { 6, 8, 0, 0, 11025, 5, 0, 0, 0, 0, 0, 1, 20000, 0, 100, -4 } },
    { "12\xc2\xb7" "26k tread", { 2, 12, 1, 0, 26040, 3, 1, 0, 1, 10, 1, 1, 12000, 0, 100, -1 } },
    { "8\xc2\xb7" "16k fold", { 4, 8, 0, 0, 16000, 4, 1, 40, 0, 0, 0, 1, 20000, 0, 100, -3 } },
    { "6\xc2\xb7" "8k toy", { 6, 6, 0, 0, 8000, 8, 0, 0, 0, 0, 0, 1, 20000, 0, 100, -6 } },
    { "14\xc2\xb7" "44k glue", { 5, 14, 0, 40, 44100, 1, 1, 0, 1, 30, 0, 1, 20000, 0, 100, -2 } },
    { "12\xc2\xb7" "20k thick", { 8, 12, 0, 0, 20000, 5, 1, -10, 1, 0, 1, 1, 9000, 15, 100, -3 } },
    { "drums \xc2\xb7 crunch bus", { 10, 10, 0, 0, 22050, 3, 1, 0, 1, 10, 0, 1, 20000, 0, 70, -3 } },
    { "drums \xc2\xb7 break dust", { 6, 12, 0, 0, 26040, 6, 1, -10, 1, 0, 1, 1, 9500, 10, 100, -2 } },
    { "bass \xc2\xb7 warm 12", { 8, 12, 0, 0, 26040, 0, 1, -20, 1, 0, 1, 1, 4000, 20, 100, -3 } },
    { "bass \xc2\xb7 sub grit", { 14, 8, 0, 0, 12000, 2, 1, 0, 1, 0, 1, 0, 2500, 25, 60, -4 } },
    { "keys \xc2\xb7 lo-fi", { 4, 10, 0, 30, 18000, 12, 1, -20, 1, 0, 1, 1, 6500, 5, 100, 0 } },
    { "vocal \xc2\xb7 radio", { 10, 8, 0, 20, 11025, 0, 1, 0, 1, 0, 2, 1, 1800, 40, 100, -2 } },
    { "vocal \xc2\xb7 phone", { 6, 8, 1, 0, 8000, 0, 1, 0, 1, 0, 2, 1, 1200, 55, 100, -1 } },
    { "hats \xc2\xb7 sizzle", { 0, 6, 0, 0, 30000, 0, 0, 0, 0, 0, 3, 1, 5000, 0, 50, -2 } },
    { "pad \xc2\xb7 drift", { 2, 12, 0, 20, 22050, 60, 1, -10, 1, 20, 1, 1, 8000, 0, 100, 0 } },
    { "fx \xc2\xb7 alias sweep", { 0, 16, 0, 0, 6000, 0, 1, 100, 0, 0, 0, 1, 20000, 0, 100, -3 } },
    { "fx \xc2\xb7 700 hz wreck", { 12, 4, 0, 0, 700, 30, 0, 0, 0, 0, 1, 1, 3000, 30, 100, -8 } },
    { "mix \xc2\xb7 parallel crush", { 12, 8, 0, 0, 16000, 4, 1, 0, 1, 0, 0, 1, 20000, 0, 35, 0 } },
};

void CrumbProcessor::setAll(const float* normalised)
{
    for (int i = 0; i < Bypass; ++i)
        if (auto* p = apvts.getParameter(ids[i]); std::abs(p->getValue() - normalised[i]) > 1e-7f)
            p->beginChangeGesture(), p->setValueNotifyingHost(normalised[i]), p->endChangeGesture();
}

void CrumbProcessor::setCurrentProgram(int index)
{
    if (!isPositiveAndBelow(index, 25))
        return;
    float n[16];
    for (int i = 0; i < Bypass; ++i)
        n[i] = apvts.getParameter(ids[i])->convertTo0to1(crumb::presets[index].v[i]);
    setAll(n);
    preset = index;
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
}

const String CrumbProcessor::getProgramName(int index) { return isPositiveAndBelow(index, 25) ? String::fromUTF8(crumb::presets[index].name) : String(); }

File CrumbProcessor::presetFolder() { return File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("daveydidit/crumb/Presets"); }

String CrumbProcessor::presetName()
{
    const ScopedLock sl(lock);
    return preset >= 0 ? getProgramName(preset) : userName.isEmpty() ? String("custom") : userName;
}

void CrumbProcessor::stepPreset(int delta)
{
    auto files = presetFolder().findChildFiles(File::findFiles, false, "*.xml");
    files.sort();
    int at = preset;
    for (int k = 0; k < files.size() && at < 0; ++k)
        at = files[k].getFileNameWithoutExtension() == presetName() ? 25 + k : at;
    at = (jmax(0, at) + delta + 25 + files.size()) % (25 + files.size()); // an unknown position steps from "init"
    if (at < 25)
        return setCurrentProgram(at);
    MemoryOutputStream m; // the file is plain XML: give it the state envelope and read it like a state
    const auto text = files[at - 25].loadFileAsString();
    m.writeInt(0x21324356), m.writeInt((int) text.getNumBytesAsUTF8()), m << text;
    if (parse(m.getData(), (int) m.getDataSize(), true))
    {
        const ScopedLock sl(lock);
        preset = -1, userName = files[at - 25].getFileNameWithoutExtension();
    }
}

File CrumbProcessor::saveUserPreset()
{
    const auto folder = presetFolder();
    folder.createDirectory();
    File file;
    for (int k = 1; k < 1000 && (file == File() || file.exists()); ++k)
        file = folder.getChildFile("user " + String(k).paddedLeft('0', 2) + ".xml");
    auto xml = apvts.copyState().createXml();
    if (xml == nullptr || !xml->writeTo(file))
        return {};
    const ScopedLock sl(lock);
    preset = -1, userName = file.getFileNameWithoutExtension();
    return file;
}

void CrumbProcessor::selectAb(int slot)
{
    const ScopedLock sl(lock);
    if (slot == ab || !isPositiveAndBelow(slot, 2))
        return;
    auto& leaving = slots[ab];
    leaving.resize(16);
    for (int i = 0; i < Bypass; ++i)
        leaving[(size_t) i] = apvts.getParameter(ids[i])->getValue();
    slotPreset[ab] = preset, slotName[ab] = userName, ab = slot;
    if (slots[slot].empty()) // never visited: it starts as a copy of the other slot
        return;
    const auto entering = slots[slot]; // a copy: a listener may save, which writes the slots
    setAll(entering.data());
    preset = slotPreset[slot], userName = slotName[slot];
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CrumbProcessor(); }
