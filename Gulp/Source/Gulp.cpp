#include "Gulp.h"
#include <optional>
#include <sstream>

using namespace juce;

namespace
{
// ============================ FROZEN — never rename/reorder semantics ============================
// Hosts store sessions and automation under these string IDs (ParameterID version hint 1). Never rename, remove,
// reorder, change a range or a choice list so that old values move. New parameters are APPENDED only, with hint 2+.
const char* const ids[] = { "trigger", "tempoSync", "rate", "freeHz", "curve", "slope",  "invert", "depth", "shift",
                            "loop",    "thresh",    "band", "split",  "output", "color", "tone",   "stereo" };
// =================================================================================================
enum { Trigger, TempoSync, Rate, FreeHz, Curve, Slope, Invert, Depth, Shift, Loop, Thresh, Band, Split, Output, Color, Tone, Stereo };
constexpr double pi = MathConstants<double>::pi;

AudioProcessorValueTreeState::ParameterLayout layout()
{
    // Every float snaps to its step: save -> reload is then bit-exact (the snapped value converts back to itself).
    auto trim = [](float v) { return String(v, std::abs(v - std::round(v)) < 0.05f ? 0 : 1); };
    auto sign = [](float v) { return String(v > 0 ? "+" : ""); };
    auto parse = [](const String& t) { return (float) (t.replace(CharPointer_UTF8("\xe2\x88\x92"), "-").retainCharacters("+-.0123456789").getDoubleValue() * (t.containsIgnoreCase("k") ? 1000 : 1)); };
    auto num = [&](int i, const char* name, float lo, float hi, float def, float step, bool log, const char* unit, std::function<String(float)> text) {
        const auto snap = [per = std::round(1 / step)](float s, float e, float v) { return jlimit(s, e, std::round(v * per) / per + 0.0f); };
        // The snap is part of the mapping: APVTS also reads its first value through the range's own convertFrom0to1.
        NormalisableRange<float> r(lo, hi, [=](float s, float e, float v) { return snap(s, e, log ? s * std::pow(e / s, v) : s + (e - s) * v); },
                                   [=](float s, float e, float v) { return log ? std::log(v / s) / std::log(e / s) : (v - s) / (e - s); }, snap);
        r.interval = log ? 0.0f : step;
        return std::make_unique<AudioParameterFloat>(ParameterID { ids[i], 1 }, name, r, def,
                                                     AudioParameterFloatAttributes().withLabel(unit).withStringFromValueFunction([text](float v, int) { return text(v); }).withValueFromStringFunction(parse));
    };
    auto onOff = [](int i, const char* name, bool def) { return std::make_unique<AudioParameterBool>(ParameterID { ids[i], 1 }, name, def); };
    auto choice = [](int i, const char* name, StringArray items, int def) { return std::make_unique<AudioParameterChoice>(ParameterID { ids[i], 1 }, name, items, def); };
    auto percent = [trim](float v) { return trim(v) + "%"; };
    return { choice(Trigger, "Trigger", { "sync", "midi", "audio" }, 0),
             onOff(TempoSync, "Tempo Sync", true),
             choice(Rate, "Rate", StringArray(gulp::rateNames, gulp::numRates), 8),
             num(FreeHz, "Free Rate", 0.1f, 20, 2, 0.01f, true, "Hz", [](float v) { return String(v, 2) + " Hz"; }),
             choice(Curve, "Curve", StringArray(gulp::curveNames, gulp::numCurves), 0),
             num(Slope, "Slope", -100, 100, 0, 0.1f, false, "%", [=](float v) { return sign(v) + trim(v) + "%"; }),
             onOff(Invert, "Invert", false),
             num(Depth, "Depth", 0, 100, 100, 0.1f, false, "%", percent),
             num(Shift, "Shift", -50, 50, 0, 0.1f, false, "%", [=](float v) { return exactlyEqual(v, 0.0f) ? String("0") : sign(v) + trim(v) + "%"; }),
             onOff(Loop, "Loop", false),
             num(Thresh, "Threshold", -60, 0, -24, 0.1f, false, "dB", [=](float v) { return trim(v) + " dB"; }),
             choice(Band, "Band", { "full", "lows", "highs" }, 0),
             num(Split, "Split", 20, 5120, 120, 0.1f, true, "Hz", [](float v) { return String(roundToInt(v)) + " Hz"; }),
             num(Output, "Output", -24, 12, 0, 0.1f, false, "dB", [=](float v) { return sign(v) + String(v, 1) + " dB"; }),
             choice(Color, "Color", StringArray(gulp::colorNames, gulp::numColors), 0),
             num(Tone, "Tone", 0, 100, 50, 0.1f, false, "%", percent),
             choice(Stereo, "Stereo", { "both", "mid", "side" }, 0) };
}

double release(double p, double attack, double hold, double r, double k) // linear attack, hold, release ^ k
{
    if (p < attack) return p / attack;
    p -= attack + hold;
    if (p < 0) return 1;
    return p < r ? std::pow(std::max(0.0, 1 - p / r), k) : 0.0;
}

// Zavalishin TPT state-variable filter: stable under per-sample cutoff changes.
struct Coef { double k = 2, a1 = 1, a2 = 0, a3 = 0; };
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

// The colour modes: what the duck amount a (0..1) does to the ducked band. All are linear in x for a given a (so mid
// and side can be taken after processing left and right), and level, bottom, top, hollow and grit are exact at a = 0.
struct Coefs { Coef cross, bottom, top, dark, thin, pinchHp, pinchLp, notch; double inc = 1, glide = 1; };
struct ColourState { Svf f, g; double held = 0, phase = 0, smooth = 0; };
double colour(int mode, ColourState& s, double x, double a, const Coefs& k)
{
    switch (mode)
    {
        case 0: return x * (1 - a);                          // level
        case 1: return x - a * s.f.run(x, k.bottom)[0];      // bottom: low shelf cut
        case 2: return x - a * (x - s.f.run(x, k.top)[0]);   // top: high shelf cut
        case 3: return s.f.run(x, k.dark)[0];                // dark: lowpass sweeping down
        case 4: return s.f.run(x, k.thin)[2];                // thin: highpass sweeping up
        case 5: return s.g.run(s.f.run(x, k.pinchHp)[2], k.pinchLp)[0]; // pinch: band closing on 1 kHz
        case 6: return x - a * k.notch.k * s.f.run(x, k.notch)[1];       // hollow: 1 kHz notch mixed in
        default:                                             // grit: sample & hold at a falling rate
            if ((s.phase += k.inc) >= 1) s.phase -= std::floor(s.phase), s.held = x;
            s.smooth += k.glide * (s.held - s.smooth);
            return s.held + a * (s.smooth - s.held);
    }
}

std::optional<double> number(const String& t) // the whole text is one finite number (C locale); else nothing
{
    std::istringstream in(t.toStdString());
    in.imbue(std::locale::classic());
    double v = 0;
    return in >> v && std::isfinite(v) && (in >> std::ws).eof() ? std::optional(jlimit(-1.0e6, 1.0e6, v)) : std::nullopt;
}
} // namespace

const char* const gulp::curveNames[] = { "classic", "tight", "punch", "round", "long", "slam", "sub", "sub long", "trim", "suck", "wave", "double" };
const char* const gulp::rateNames[] = { "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2T", "1/2", "1/2D", "1/1", "2/1", "4/1" };
const char* const gulp::colorNames[] = { "level", "bottom", "top", "dark", "thin", "pinch", "hollow", "grit" };

double gulp::duck(int curve, double p, double s)
{
    switch (curve)
    {
        case 0: return release(p, .01, 0, .55 * s, 2.2);
        case 1: return release(p, .005, 0, .30 * s, 3.0);
        case 2: return release(p, .005, .08, .35 * s, 1.5);
        case 3: return release(p, .04, 0, .70 * s, 1.2);
        case 4: return release(p, .01, 0, .92 * s, 1.8);
        case 5: return p < .3 * s ? 1 : std::max(0.0, 1 - (p - .3 * s) / .06);
        case 6: return release(p, .02, .1, .80 * s, 1.0);
        case 7: return release(p, .02, .16, .82 * s, .8);
        case 8: return p < .22 * s ? 0 : std::min(1.0, (p - .22 * s) / .12);
        case 9: return 1 - std::pow(std::min(1.0, p / std::min(1.0, .95 * s)), 1.6);
        case 10: return .5 - .5 * std::cos(2 * pi * p);
        default: return release(std::fmod(2 * p, 1.0), .01, 0, .55 * s, 2.2);
    }
}

struct GulpProcessor::Dsp
{
    std::array<std::atomic<float>*, gulp::numParams> raw {};
    double fs = 48000, slew = 1, envRelease = 0;
    SmoothedValue<double> depth, gain, split, tone, band[3], colours[gulp::numColors], mid, side;
    std::array<double, 1025> table {};
    std::array<float, 3> tableKey {}; // curve, slope, invert the table was built for
    Svf cross[2][4];                  // per channel: two lowpass, two highpass sections (LR4)
    ColourState state[2][3][gulp::numColors]; // per channel, band (full, lows, highs) and colour mode
    Coefs k;
    double cycle = 1, aim = 0, a = 0, hold = 0, env = 0, lo = 0, hi = 0, outEnv = 0, outLo = 0, outHi = 0;
    bool armed = false, scArmed = true;
    int holdoff = 0, bin = 0;
    unsigned tick = 0; // only its low bits are used: wraps, never overflows
    // Delayed triggers: one flag per sample, a ring as long as the longest delay (50 % of 4/1 at 20 bpm = 24 s), so
    // every hit fires at its own due sample however close together they come. Sized in prepare, never on the audio thread.
    std::vector<bool> due;
    size_t now = 0;

    void prepare(double sr)
    {
        fs = sr > 0 && sr < 1.0e7 ? sr : 48000.0;
        slew = 1 - std::exp(-1 / (0.00025 * fs)), envRelease = std::exp(-1 / (0.05 * fs));
        for (auto* s : { &depth, &gain, &split, &tone })
            s->reset(fs, 0.02);
        for (auto* s : { &band[0], &band[1], &band[2], &mid, &side })
            s->reset(fs, 0.01);
        for (auto& s : colours)
            s.reset(fs, 0.01);
        due.assign((size_t) std::ceil(24 * fs) + 2, false);
    }
    void reset()
    {
        cycle = 1, aim = a = hold = env = lo = hi = outEnv = outLo = outHi = 0, armed = false, scArmed = true, holdoff = bin = 0, tick = 0;
        std::fill(due.begin(), due.end(), false), now = 0;
        tableKey[0] = -1;
        for (auto& c : cross) for (auto& s : c) s = {};
        for (auto& c : state) for (auto& b : c) for (auto& s : b) s = {};
        targets(true);
    }
    // Band and colour switches crossfade over 10 ms; a path that fades in starts from silent filter state.
    void targets(bool jump)
    {
        auto p = [this](int i) { return (double) raw[(size_t) i]->load(); };
        const auto set = [jump](SmoothedValue<double>& s, double v) { jump ? s.setCurrentAndTargetValue(v) : s.setTargetValue(v); };
        for (int b = 0; b < 3; ++b)
        {
            if (b == (int) p(Band) && exactlyEqual(band[b].getCurrentValue(), 0.0))
                for (auto& c : state) for (auto& s : c[b]) s = {};
            set(band[b], b == (int) p(Band) ? 1 : 0);
        }
        for (int m = 0; m < gulp::numColors; ++m)
        {
            if (m == (int) p(Color) && exactlyEqual(colours[m].getCurrentValue(), 0.0))
                for (auto& c : state) for (auto& b : c) b[m] = {};
            set(colours[m], m == (int) p(Color) ? 1 : 0);
        }
        set(depth, p(Depth) / 100), set(gain, Decibels::decibelsToGain(p(Output))), set(split, std::log2(p(Split))), set(tone, p(Tone) / 100);
        set(mid, p(Stereo) < 1.5 ? 1 : 0), set(side, p(Stereo) < 0.5 || p(Stereo) > 1.5 ? 1 : 0);
    }
    void buildTable()
    {
        const std::array<float, 3> key { raw[Curve]->load(), raw[Slope]->load(), raw[Invert]->load() };
        if (key == tableKey)
            return;
        tableKey = key;
        for (size_t j = 0; j < table.size(); ++j)
        {
            const double d = gulp::duck((int) key[0], (double) j / 1024, std::exp2(key[1] / 100.0));
            table[j] = key[2] > 0.5f ? 1 - d : d;
        }
    }
    void update(double splitLog2, double t) // every 8 samples: crossover and colour coefficients from the duck amount a
    {
        const auto g = [this](double hz) { return std::tan(pi * jmin(0.45 * fs, hz) / fs); };
        const double q = 2 * std::pow(12.0, -t); // tone as resonance: Q 0.5 .. 6
        k.cross = svf(g(std::exp2(splitLog2)), std::sqrt(2.0));
        k.bottom = svf(g(40 * std::pow(10.0, t)), std::sqrt(2.0));
        k.top = svf(g(1500 * std::pow(8.0, t)), std::sqrt(2.0));
        k.dark = svf(g(20000 * std::pow(0.0075, a)), q);
        k.thin = svf(g(20 * std::pow(150.0, a)), q);
        k.pinchHp = svf(g(20 * std::pow(50.0, a)), q), k.pinchLp = svf(g(20000 * std::pow(0.05, a)), q);
        k.notch = svf(g(1000), q * (0.2 + 1.8 * a));
        k.inc = std::pow(1000 / fs, a);
        k.glide = 1 - std::exp(-2 * pi * jmin(0.45 * fs, 20000 * std::pow(0.05, t)) / fs);
    }
};

GulpProcessor::GulpProcessor(File directory)
    : AudioProcessor(BusesProperties()
                         .withInput("Input", AudioChannelSet::stereo())
                         .withOutput("Output", AudioChannelSet::stereo())
                         .withInput("Sidechain", AudioChannelSet::stereo(), false)),
      apvts(*this, nullptr, "GULP", layout()), presetDirectory(std::move(directory)), dsp(std::make_unique<Dsp>())
{
    for (int i = 0; i < gulp::numParams; ++i)
        dsp->raw[(size_t) i] = apvts.getRawParameterValue(ids[i]);
    dsp->prepare(48000); // usable even if a host processes before prepareToPlay
}

GulpProcessor::~GulpProcessor() = default;

bool GulpProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), sc = l.inputBuses.size() > 1 ? l.inputBuses[1] : AudioChannelSet::disabled();
    return in == l.getMainOutputChannelSet() && (in == AudioChannelSet::mono() || in == AudioChannelSet::stereo())
        && (sc.isDisabled() || sc == AudioChannelSet::mono() || sc == AudioChannelSet::stereo());
}

void GulpProcessor::prepareToPlay(double fs, int)
{
    dsp->prepare(fs);
    resetPending = true; // the first block initializes the DSP from the parameters it then sees, restored or not
}

void GulpProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    auto& d = *dsp;
    if (resetPending.exchange(false))
        d.reset();
    d.targets(false), d.buildTable();
    auto p = [&d](int i) { return d.raw[(size_t) i]->load(); };
    const int n = buffer.getNumSamples(), channels = jmin(2, getMainBusNumInputChannels(), getMainBusNumOutputChannels());
    const auto sc = getBusBuffer(buffer, true, 1);
    const int trigger = (int) p(Trigger), rate = (int) p(Rate), scChannels = jmin(2, sc.getNumChannels());
    const bool loop = p(Loop) > 0.5f, tempo = p(TempoSync) > 0.5f;

    // Sync: the phase comes from the host's ppq at every block start and advances per sample, so tempo changes and
    // loop jumps land on the grid. Stopped or without a usable position it freewheels at the host tempo (120 if the
    // host reports none, or a non-finite or non-positive one). A non-finite ppq counts as no position.
    double bpm = 120, ppq = 0;
    bool playing = false;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            if (const auto b = pos->getBpm(); b && std::isfinite(*b) && *b > 0)
                bpm = jlimit(20.0, 999.0, *b);
            if (const auto q = pos->getPpqPosition(); q && std::isfinite(*q) && pos->getIsPlaying())
                playing = true, ppq = *q;
        }
    const double quarters = 4.0 * gulp::rateNum[rate] / gulp::rateDen[rate];
    // bpm 20..999, freeHz 0.1..20 and fs sanitized in prepare: inc is always finite and positive.
    const double inc = (tempo ? bpm / 60 / quarters : (double) p(FreeHz)) / d.fs, shift = p(Shift) / 100.0;
    if (trigger == 0) // the cycle stays in [0, 1) whatever the ppq (fmod is exact; ppq / quarters could overflow)
        d.cycle = playing && tempo ? std::fmod(ppq, quarters) / quarters : d.cycle, d.cycle -= std::floor(d.cycle);
    const size_t ring = d.due.size(); // positive shift delays a trigger
    const size_t delay = trigger == 0 ? 0 : (size_t) jmin((long) ring - 1, std::lround(jmax(0.0, shift) / inc));
    double scPeak = d.env, outPeak = d.outEnv; // the meters: the followers' peaks over the block
    const double threshold = Decibels::decibelsToGain((double) p(Thresh)), rearm = threshold * Decibels::decibelsToGain(-3.0);
    auto event = midi.cbegin();
    double phase = 0;
    for (int i = 0; i < n; ++i)
    {
        // Triggers: a note-on at its sample, or the sidechain's peak follower crossing the threshold (re-armed 3 dB
        // below it, 30 ms hold-off). Either restarts the cycle `delay` samples later.
        double s = 0;
        for (int c = 0; c < scChannels; ++c)
            s += sc.getSample(c, i);
        s = std::isfinite(s) ? jlimit(-1000.0, 1000.0, s / jmax(1, scChannels)) : 0.0;
        d.env = jmax(std::abs(s), d.env * d.envRelease);
        bool hit = false;
        for (; event != midi.cend() && (*event).samplePosition <= i; ++event)
            hit = hit || (trigger == 1 && (*event).numBytes == 3 && ((*event).data[0] & 0xf0) == 0x90 && (*event).data[2] > 0);
        d.holdoff = jmax(0, d.holdoff - 1);
        if (d.scArmed && d.env > threshold && d.holdoff == 0)
            hit = hit || trigger == 2, d.scArmed = false, d.holdoff = (int) (0.03 * d.fs);
        else if (d.env < rearm)
            d.scArmed = true;
        if (hit)
            d.due[(d.now + delay) % ring] = true;
        const bool fire = d.due[d.now];
        d.due[d.now] = false, d.now = (d.now + 1) % ring;
        if (fire && trigger != 0) // a retrigger starts its attack from where the duck is: no up-then-down step
            d.cycle = 0, d.armed = true, d.hold = jmax(d.aim, d.a);
        bool idle = false;
        if (trigger == 0)
            phase = d.cycle - shift, phase -= std::floor(phase), d.cycle += inc;
        else // before the first trigger, and after a one shot has run (loop off): full level until the next trigger
        {
            phase = !d.armed ? 1 : loop ? d.cycle - std::floor(d.cycle) : jmin(d.cycle, 1.0);
            idle = !d.armed || (!loop && phase >= 1);
            d.cycle = loop ? phase + inc : jmin(d.cycle + inc, 1.0);
        }

        const double x = phase * 1024;
        const int j = jmin(1023, (int) x);
        const double depth = d.depth.getNextValue();
        const double aim = idle ? 0.0 : depth * (d.table[(size_t) j] + (x - j) * (d.table[(size_t) j + 1] - d.table[(size_t) j]));
        if (idle || (d.hold > 0 && (aim >= d.hold || d.table[(size_t) j + 1] <= d.table[(size_t) j])))
            d.hold = 0;
        d.aim = jmax(aim, d.hold);
        // 0.25 ms: inaudible on attacks, removes steps from curve swaps. Lands exactly (the one-pole alone stalls an ulp short).
        d.a = std::abs(d.aim - d.a) < 1e-9 ? d.aim : d.a + (d.aim - d.a) * d.slew;

        const double splitLog2 = d.split.getNextValue(), tone = d.tone.getNextValue(), out = d.gain.getNextValue(), wMid = d.mid.getNextValue(), wSide = d.side.getNextValue();
        double wBand[3], wColour[gulp::numColors];
        for (int k = 0; k < 3; ++k) wBand[k] = d.band[k].getNextValue();
        for (int m = 0; m < gulp::numColors; ++m) wColour[m] = d.colours[m].getNextValue();
        if ((d.tick++ & 7) == 0)
            d.update(splitLog2, tone);

        // Per channel: LR4 split, the colour on the ducked band (full = no filters in the path), then mid/side as a
        // weighted sum of the left and right changes, so both = left/right, and each is exact when nothing changes.
        double in[2] {}, change[2] {};
        for (int c = 0; c < channels; ++c)
        {
            const float v = buffer.getSample(c, i);
            in[c] = std::isfinite(v) ? jlimit(-1000.0f, 1000.0f, v) : 0.0f;
            auto& f = d.cross[c];
            const double low = f[1].run(f[0].run(in[c], d.k.cross)[0], d.k.cross)[0], high = f[3].run(f[2].run(in[c], d.k.cross)[2], d.k.cross)[2];
            double y = 0;
            for (int k = 0; k < 3; ++k)
                if (wBand[k] > 0)
                {
                    double sum = 0;
                    for (int m = 0; m < gulp::numColors; ++m)
                        if (wColour[m] > 0)
                            sum += wColour[m] * colour(m, d.state[c][k][m], k == 0 ? in[c] : k == 1 ? low : high, d.a, d.k);
                    y += wBand[k] * ((k == 0 ? 0 : k == 1 ? high : low) + sum);
                }
            change[c] = y - in[c];
        }
        if (channels == 1)
            change[1] = change[0];
        const double m = 0.5 * wMid * (change[0] + change[1]), sd = 0.5 * wSide * (change[0] - change[1]);
        double o = 0, peak = 0;
        for (int c = 0; c < channels; ++c)
        {
            const double y = (in[c] + m + (c == 0 ? sd : -sd)) * out;
            buffer.setSample(c, i, (float) y);
            o += y / channels, peak = jmax(peak, std::abs(y));
        }

        // Telemetry: the followers (instant attack, 50 ms release) for the meters; the sidechain and the output folded
        // into the cycle for the kick ghost and the output scope.
        d.outEnv = jmax(peak, d.outEnv * d.envRelease);
        scPeak = jmax(scPeak, d.env), outPeak = jmax(outPeak, d.outEnv);
        const int b = jmin(gulp::Feed::bins - 1, (int) (phase * gulp::Feed::bins));
        if (b != d.bin)
        {
            const auto k = (size_t) d.bin;
            feed.lo[k].store((float) d.lo, std::memory_order_relaxed), feed.hi[k].store((float) d.hi, std::memory_order_relaxed);
            feed.outLo[k].store((float) d.outLo, std::memory_order_relaxed), feed.outHi[k].store((float) d.outHi, std::memory_order_relaxed);
            d.bin = b, d.lo = d.hi = s, d.outLo = d.outHi = o;
        }
        else
            d.lo = jmin(d.lo, s), d.hi = jmax(d.hi, s), d.outLo = jmin(d.outLo, o), d.outHi = jmax(d.outHi, o);
    }
    for (int c = channels; c < getTotalNumOutputChannels(); ++c)
        buffer.clear(c, 0, n);
    feed.phase.store((float) phase, std::memory_order_relaxed), feed.duck.store((float) d.a, std::memory_order_relaxed), feed.bpm.store((float) bpm, std::memory_order_relaxed);
    feed.scLevel.store((float) scPeak, std::memory_order_relaxed), feed.outLevel.store((float) outPeak, std::memory_order_relaxed);
}

// ================================ State, presets, A/B ================================
void GulpProcessor::getStateInformation(MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty("stateVersion", stateVersion, nullptr);
    {
        const ScopedLock sl(lock);
        state.setProperty("preset", preset.load(), nullptr);
        if (preset < 0 && userName.isNotEmpty())
            state.setProperty("presetName", userName, nullptr);
        state.setProperty("ab", ab.load(), nullptr);
        for (int k = 0; k < 2; ++k)
            if (!slots[k].empty()) // "preset;v0,..,v16", normalised, 9 significant digits: every float reads back exactly
            {
                std::ostringstream t;
                t.imbue(std::locale::classic()), t.precision(9), t << slotPreset[k] << ';';
                for (const float& v : slots[k])
                    t << v << (&v == &slots[k].back() ? "" : ",");
                state.setProperty(k == 0 ? "slotA" : "slotB", String(t.str()), nullptr);
                state.setProperty(k == 0 ? "slotNameA" : "slotNameB", slotName[k], nullptr);
            }
    }
    if (editorWidth > 0)
        state.setProperty("editorWidth", editorWidth.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, dest);
}

void GulpProcessor::applyParams(const XmlElement& state)
{
    for (int i = 0; i < gulp::numParams; ++i)
    {
        auto* p = apvts.getParameter(ids[i]);
        const XmlElement* e = nullptr; // the first PARAM with this id
        for (auto* c : state.getChildWithTagNameIterator("PARAM"))
            e = e == nullptr && c->getStringAttribute("id") == ids[i] ? c : e;
        const auto v = number(e != nullptr ? e->getStringAttribute("value") : String());
        p->setValueNotifyingHost(v ? p->convertTo0to1(p->getNormalisableRange().snapToLegalValue((float) *v)) : p->getDefaultValue());
    }
}

// stateVersion 1 is the first format. A newer one loads the parameters this version knows; each future bump gets an
// explicit migration here and a saved blob in Tests/. Malformed documents change nothing; JUCE's parser accepts a
// truncated one, so the text must also end with the root's closing tag.
void GulpProcessor::setStateInformation(const void* data, int size)
{
    const auto* bytes = static_cast<const char*>(data);
    if (bytes == nullptr || size < 8 || ByteOrder::littleEndianInt(bytes) != 0x21324356 || ByteOrder::littleEndianInt(bytes + 4) > (uint32) size - 8)
        return;
    const auto text = String::fromUTF8(bytes + 8, (int) ByteOrder::littleEndianInt(bytes + 4)).trimEnd();
    const auto tree = text.endsWith("</GULP>") || (text.endsWith("/>") && !text.fromFirstOccurrenceOf("<GULP", false, false).containsChar('<')) ? parseXML(text) : nullptr;
    if (tree == nullptr || !tree->hasTagName("GULP"))
        return;
    applyParams(*tree);
    const ScopedLock sl(lock);
    for (int k = 0; k < 2; ++k) // a slot counts only with a preset and 17 normalised values
    {
        const auto t = StringArray::fromTokens(tree->getStringAttribute(k == 0 ? "slotA" : "slotB"), ";,", "");
        slots[k].clear(), slotName[k] = tree->getStringAttribute(k == 0 ? "slotNameA" : "slotNameB");
        slotPreset[k] = (int) jlimit(-1.0, gulp::numPresets - 1.0, number(t[0]).value_or(-1));
        for (int i = 1; i < t.size() && t.size() == gulp::numParams + 1; ++i)
            if (const auto v = number(t[i]); v && *v >= 0 && *v <= 1)
                slots[k].push_back((float) *v);
        if (slots[k].size() != (size_t) gulp::numParams)
            slots[k].clear();
    }
    ab = number(tree->getStringAttribute("ab")).value_or(0) > 0.5 ? 1 : 0;
    editorWidth = (int) jlimit(0.0, 4096.0, number(tree->getStringAttribute("editorWidth")).value_or(0));
    preset = (int) jlimit(-1.0, gulp::numPresets - 1.0, number(tree->getStringAttribute("preset")).value_or(-1)), userName = tree->getStringAttribute("presetName");
}

// Values in ID order: trigger (sync midi audio), tempoSync, rate (index), freeHz, curve (index), slope %, invert, depth %,
// shift %, loop, thresh dB, band (full lows highs), split Hz, output dB, color (index), tone %, stereo (both mid side).
const gulp::Preset gulp::presets[] = {
    { "init", { 0, 1, 8, 2, 0, 0, 0, 100, 0, 0, -24, 0, 120, 0, 0, 50, 0 } },
    { "techno \xc2\xb7 tight eighths", { 0, 1, 5, 2, 1, 0, 0, 90, 0, 0, -24, 0, 120, 0, 0, 50, 0 } },
    { "sub \xc2\xb7 long breath", { 0, 1, 11, 2, 7, 20, 0, 100, 0, 0, -24, 1, 90, 0, 0, 50, 0 } },
    { "house \xc2\xb7 classic pump", { 0, 1, 8, 2, 0, 0, 0, 100, 0, 0, -24, 1, 120, 0, 0, 50, 0 } },
    { "kick \xc2\xb7 midi punch", { 1, 1, 8, 2, 2, 0, 0, 85, 0, 0, -24, 0, 120, 0, 0, 50, 0 } },
    { "kick \xc2\xb7 audio follow", { 2, 1, 8, 2, 0, -20, 0, 80, 0, 0, -24, 1, 150, 0, 0, 50, 0 } },
    { "pad \xc2\xb7 wave swell", { 0, 1, 11, 2, 10, 0, 0, 70, 0, 1, -24, 0, 120, 0, 0, 50, 0 } },
    { "bus \xc2\xb7 dark duck", { 0, 1, 8, 2, 3, 0, 0, 100, 0, 0, -24, 0, 120, 0, 3, 40, 0 } },
    { "wide \xc2\xb7 side breathe", { 0, 1, 8, 2, 4, 0, 0, 80, 0, 0, -24, 0, 120, 0, 0, 50, 2 } },
    { "fx \xc2\xb7 grit gate", { 0, 1, 2, 2, 5, 0, 0, 100, 0, 0, -24, 0, 120, -2, 7, 60, 0 } },
};

void GulpProcessor::setAll(const float* normalised)
{
    for (int k = 0; k < gulp::numParams; ++k)
        if (auto* p = apvts.getParameter(ids[k]); std::abs(p->getValue() - normalised[k]) > 1e-7f)
            p->beginChangeGesture(), p->setValueNotifyingHost(normalised[k]), p->endChangeGesture();
}

void GulpProcessor::setIdentity(int index, const String& name)
{
    const ScopedLock sl(lock);
    preset = index, userName = name;
}

void GulpProcessor::setCurrentProgram(int index)
{
    if (!isPositiveAndBelow(index, gulp::numPresets))
        return;
    float v[gulp::numParams];
    for (int k = 0; k < gulp::numParams; ++k)
        v[k] = apvts.getParameter(ids[k])->convertTo0to1(gulp::presets[index].v[k]);
    setAll(v), setIdentity(index, {});
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
}

const String GulpProcessor::getProgramName(int index) { return isPositiveAndBelow(index, gulp::numPresets) ? String::fromUTF8(gulp::presets[index].name) : String(); }

File GulpProcessor::presetFolder() const { return presetDirectory != File() ? presetDirectory : File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("daveydidit/gulp/Presets"); }

String GulpProcessor::presetName() const
{
    const ScopedLock sl(lock);
    return preset >= 0 ? String::fromUTF8(gulp::presets[preset].name) : userName.isEmpty() ? String("custom") : userName;
}

void GulpProcessor::stepPreset(int delta)
{
    auto files = presetFolder().findChildFiles(File::findFiles, false, "*.xml");
    files.sort();
    const auto current = presetName();
    const int count = gulp::numPresets + files.size();
    int at = preset;
    for (int k = 0; k < files.size() && at < 0; ++k)
        at = files[k].getFileNameWithoutExtension() == current ? gulp::numPresets + k : at;
    at = (jmax(0, at) + delta + count) % count; // an unknown position steps from "init"
    if (at < gulp::numPresets)
        return setCurrentProgram(at);
    if (const auto xml = parseXML(files[at - gulp::numPresets]); xml != nullptr && xml->hasTagName("GULP"))
        applyParams(*xml), setIdentity(-1, files[at - gulp::numPresets].getFileNameWithoutExtension());
}

File GulpProcessor::saveUserPreset()
{
    const auto folder = presetFolder();
    folder.createDirectory();
    File file;
    for (int k = 1; k < 1000 && (file == File() || file.exists()); ++k)
        file = folder.getChildFile("user " + String(k).paddedLeft('0', 2) + ".xml");
    const auto xml = apvts.copyState().createXml();
    if (file.exists() || xml == nullptr || !xml->writeTo(file))
        return {};
    setIdentity(-1, file.getFileNameWithoutExtension());
    return file;
}

void GulpProcessor::selectAb(int slot)
{
    std::vector<float> to;
    int toPreset = 0;
    String toName;
    {
        const ScopedLock sl(lock);
        const int from = ab;
        if (slot == from || !isPositiveAndBelow(slot, 2))
            return;
        slots[from].resize(gulp::numParams);
        for (int k = 0; k < gulp::numParams; ++k)
            slots[from][(size_t) k] = apvts.getParameter(ids[k])->getValue();
        slotPreset[from] = preset, slotName[from] = userName;
        to = slots[slot], toPreset = slotPreset[slot], toName = slotName[slot], ab = slot;
    }
    if (!to.empty()) // never visited: it starts as a copy of the other slot
        setAll(to.data()), setIdentity(toPreset, toName);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new GulpProcessor(); }
