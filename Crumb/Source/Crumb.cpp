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
    auto num = [&](int i, const char* name, float lo, float hi, float def, const char* unit, bool log = false) {
        NormalisableRange<float> r(lo, hi, nullptr, nullptr, snap);
        if (log)
            r = NormalisableRange<float>(lo, hi, [](float s, float e, float v) { return s * std::pow(e / s, v); },
                                         [](float s, float e, float v) { return std::log(v / s) / std::log(e / s); }, snap);
        r.interval = log ? 0.0f : 0.1f;
        return std::make_unique<AudioParameterFloat>(ParameterID { ids[i], 1 }, name, r, def, AudioParameterFloatAttributes().withLabel(unit));
    };
    auto onOff = [](int i, const char* name, bool def) { return std::make_unique<AudioParameterBool>(ParameterID { ids[i], 1 }, name, def); };
    auto choice = [](int i, const char* name, StringArray items, int def) {
        return std::make_unique<AudioParameterChoice>(ParameterID { ids[i], 1 }, name, items, def);
    };
    auto bitsText = AudioParameterIntAttributes().withStringFromValueFunction([](int v, int) { return v == 0 ? String("OFF") : String(v) + " bit"; });
    return { num(Preamp, "Preamp", 0, 48, 0, "dB"),
             std::make_unique<AudioParameterInt>(ParameterID { ids[Bits], 1 }, "Resolution", 0, 24, 12, bitsText),
             onOff(DcShift, "DC Shift", false),
             num(Dither, "Dither", 0, 100, 0, "%"),
             num(Rate, "Rate", 44, 44100, 26040, "Hz", true),
             num(Jitter, "Jitter", 0, 100, 0, "%"),
             onOff(ApproxOn, "Approx Filter", true),
             num(ApproxDev, "Approx Deviation", -100, 100, 0, "%"),
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
    for (int i = 0; i < n && channels > 0; ++i)
    {
        d.next(f);
        const double on = 1 - f.v[SBypass], mix = f.v[SMix] * on, gain = 1 + on * (f.v[SOut] - 1);
        for (int c = 0; c < channels; ++c) // non-finite input becomes silence; absurd levels are clipped
        {
            const double dry = std::isfinite(x[c][i]) ? jlimit(-1000.0f, 1000.0f, x[c][i]) : 0.0f;
            x[c][i] = (float) ((dry + mix * (d.ch[(size_t) c].process(dry, f) - dry)) * gain);
        }
    }
}

void CrumbProcessor::getStateInformation(MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty("stateVersion", stateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, dest);
}

// Reads the flat <CRUMB><PARAM id=".." value=".."/>..</CRUMB> text inside JUCE's binary envelope with a single
// non-recursive scan instead of JUCE's XML parser (which recurses per level, expands DTD entities and over-reads
// truncated character references). JUCE's writer escapes '<', '>' and '"' inside values, so every state it writes
// splits cleanly into tags and attributes. Unknown entries are ignored, values are clamped, missing or unparsable
// ones reset to default; a blob that is not one complete <CRUMB> element (or holds "<!") leaves the state alone.
void CrumbProcessor::setStateInformation(const void* data, int size)
{
    const auto* bytes = static_cast<const char*>(data);
    if (bytes == nullptr || size < 8 || ByteOrder::littleEndianInt(bytes) != 0x21324356 || ByteOrder::littleEndianInt(bytes + 4) > (uint32) size - 8)
        return;
    const std::string_view xml(bytes + 8, ByteOrder::littleEndianInt(bytes + 4)), space(" \t\r\n");
    std::optional<std::string_view> found[numParams];
    int depth = 0;
    bool complete = false;
    for (size_t open = xml.find('<'), close = 0; !complete && open != std::string_view::npos; open = xml.find('<', close))
    {
        if ((close = xml.find('>', open)) == std::string_view::npos)
            return;
        auto tag = xml.substr(open + 1, close - open - 1);
        if (tag.empty() || tag[0] == '!' || (tag[0] == '/' && depth == 0))
            return;
        if (tag[0] == '?' || tag[0] == '/') // the <?xml ?> declaration, or a closing tag
        {
            complete = tag[0] == '/' && --depth == 0;
            continue;
        }
        const bool empty = tag.back() == '/';
        tag.remove_suffix(empty ? 1 : 0);
        const auto name = tag.substr(0, tag.find_first_of(space));
        if (depth == 0 && name != "CRUMB")
            return;
        std::string_view id, value;
        if (depth == 1 && name == "PARAM")
            for (auto rest = tag.substr(name.size());;) // attributes: name="text"
            {
                const auto eq = rest.find("=\""), end = eq == std::string_view::npos ? eq : rest.find('"', eq + 2);
                if (end == std::string_view::npos)
                    break;
                auto key = rest.substr(0, eq);
                key.remove_prefix(std::min(key.size(), key.find_first_not_of(space)));
                const auto text = rest.substr(eq + 2, end - eq - 2);
                id = key == "id" ? text : id, value = key == "value" ? text : value;
                rest.remove_prefix(end + 1);
            }
        for (int i = 0; i < numParams; ++i)
            if (id == ids[i] && !found[i]) // the first entry wins
                found[i] = value;
        depth += empty ? 0 : 1;
        complete = depth == 0;
    }
    if (!complete)
        return;
    // Migration hook: stateVersion 1 is the only format so far. When a later version changes the meaning of stored
    // data, read the root's stateVersion here and translate `found` before it is applied (append-only IDs need nothing).
    for (int i = 0; i < numParams; ++i)
    {
        auto* p = apvts.getParameter(ids[i]);
        std::istringstream in(std::string(found[i].value_or(""))); // locale-independent, reports overflow
        in.imbue(std::locale::classic());
        double v = 0;
        const bool ok = found[i] && in >> v && (in >> std::ws).eof();
        p->setValueNotifyingHost(ok ? p->convertTo0to1(p->getNormalisableRange().snapToLegalValue((float) jlimit(-1.0e6, 1.0e6, v))) : p->getDefaultValue());
    }
}

AudioProcessorEditor* CrumbProcessor::createEditor() { return new GenericAudioProcessorEditor(*this); } // placeholder UI
AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CrumbProcessor(); }
