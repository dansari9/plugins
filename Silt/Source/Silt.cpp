#include "Silt.h"
#include "SiltReverb.h"
#include <optional>
#include <sstream>

using namespace juce;

namespace
{
// ============================ FROZEN — never rename/reorder semantics ============================
// Hosts store sessions and automation under these string IDs (ParameterID version hint 1). Never rename, remove,
// reorder, change a range or a choice list so that old values move. New parameters are APPENDED only, with hint 2+.
// verbSize and verbMix belong to the removable reverb (SILT_REVERB): without it they stay here, inert.
const char* const ids[] = { "mix",   "output",    "size",  "grid",     "density", "position", "spray",   "sprayDir", "slope",
                            "scan",  "pitch",     "fine",  "pitchRand", "snap",   "pitchMode", "reverse", "window",  "skew",
                            "trigger", "sync",    "rate",  "division", "jitter",  "thresh",   "source",  "width",    "pan",
                            "freeze", "feedback", "tone",  "verbSize", "verbMix", "blur",     "drift",   "bloom",    "glint" };
// =================================================================================================
enum { Mix, Output, Size, GridMode, Density, Position, Spray, SprayDir, Slope, Scan, Pitch, Fine, PitchRand, Snap, PitchMode, Reverse,
       Window, Skew, Trigger, Sync, Rate, Division, Jitter, Thresh, Source, Width, Pan, Freeze, Feedback, Tone, VerbSize, VerbMix,
       Blur, Drift, Bloom, Glint };
static_assert(std::size(ids) == silt::numParams);
constexpr double pi = MathConstants<double>::pi;

// The knob travel of the skewed ranges: lo + (hi - lo) n^p, or lo (hi / lo)^(n^p). size puts 100 ms at the middle.
struct Travel
{
    double lo, hi, p;
    bool log;
    double from(double n) const { const double t = std::pow(jlimit(0.0, 1.0, n), p); return log ? lo * std::pow(hi / lo, t) : lo + (hi - lo) * t; }
    double to(double v) const { v = jlimit(lo, hi, v); return std::pow(log ? std::log(v / lo) / std::log(hi / lo) : (v - lo) / (hi - lo), 1 / p); }
};
constexpr Travel travels[] { { 0.5, 4000, 0.762, true }, { 0, 8000, 3, false }, { 0, 2000, 3, false }, { 0, 24, 2, false } }; // size, position, spray, pitchRand
enum { SizeTravel, BackTravel, SprayTravel, RandTravel };

// grid note / lock: the size and position knobs pick from a list by their travel. size: notes or grain spacings;
// position: 0 then notes, or clock periods.
int pick(double travel, int count) { return jlimit(0, count - 1, roundToInt(travel * (count - 1))); }
String ratioText(double r)
{
    for (int d : { 1, 2, 3, 4 })
        if (std::abs(r * d - std::round(r * d)) < 1e-9) return "x" + String(roundToInt(r * d)) + (d > 1 ? "/" + String(d) : String());
    return "x" + String(r, 2);
}
String gridText(int grid, bool size, double travel)
{
    if (grid == 1)
    {
        const int k = pick(travel, size ? silt::numNotes : silt::numNotes + 1) - (size ? 0 : 1);
        return k < 0 ? String("now") : String(silt::noteNames[k]);
    }
    const int k = pick(travel, silt::numRatios);
    return ratioText(size ? silt::sizeRatio[k] : silt::backRatio[k]);
}
String msText(double ms) { return ms >= 1000 ? String(ms / 1000, 2) + " s" : String(ms, ms < 10 ? 2 : ms < 100 ? 1 : 0) + " ms"; }

AudioProcessorValueTreeState::ParameterLayout layout(const SiltProcessor& self)
{
    // Every float snaps to its step: save -> reload is then bit-exact (the snapped value converts back to itself).
    auto trim = [](float v) { return String(v, std::abs(v - std::round(v)) < 0.05f ? 0 : 1); };
    auto sign = [](float v) { return String(v > 0 ? "+" : ""); };
    auto parse = [](const String& t) { return (float) (t.replace(CharPointer_UTF8("\xe2\x88\x92"), "-").retainCharacters("+-.0123456789").getDoubleValue() * (t.containsIgnoreCase("k") ? 1000 : 1)); };
    using Text = std::function<String(float)>;
    using Parse = std::function<float(const String&)>;
    auto num = [&](int i, const char* name, float lo, float hi, float def, float step, std::optional<Travel> travel, const char* unit, Text text, Parse from = {}) {
        const auto snap = [per = std::round(1 / step)](float s, float e, float v) { return jlimit(s, e, std::round(v * per) / per + 0.0f); };
        // The snap is part of the mapping: APVTS also reads its first value through the range's own convertFrom0to1.
        NormalisableRange<float> r(lo, hi, [=](float s, float e, float v) { return snap(s, e, travel ? (float) travel->from(v) : s + (e - s) * v); },
                                   [=](float s, float e, float v) { return travel ? (float) travel->to(v) : (v - s) / (e - s); }, snap);
        r.interval = travel ? 0.0f : step;
        return std::make_unique<AudioParameterFloat>(ParameterID { ids[i], 1 }, name, r, def,
                                                     AudioParameterFloatAttributes().withLabel(unit).withStringFromValueFunction([text](float v, int) { return text(v); }).withValueFromStringFunction(from ? from : Parse(parse)));
    };
    auto onOff = [](int i, const char* name, bool def) { return std::make_unique<AudioParameterBool>(ParameterID { ids[i], 1 }, name, def); };
    auto choice = [](int i, const char* name, StringArray items, int def) { return std::make_unique<AudioParameterChoice>(ParameterID { ids[i], 1 }, name, items, def); };
    auto percent = [trim](float v) { return trim(v) + "%"; };
    auto signedPercent = [=](float v) { return sign(v) + trim(v) + "%"; };
    // size and position read ms, or a note / ratio in grid note / lock (the value keeps its place on the travel).
    const auto timed = [&self, parse](int t, bool size) {
        const auto grid = [&self] { return roundToInt(self.apvts.getRawParameterValue(ids[GridMode])->load()); };
        Text text = [=](float v) { return grid() == 0 ? msText(v) : gridText(grid(), size, travels[t].to(v)); };
        Parse from = [=](const String& s) {
            const int count = grid() == 0 ? 0 : grid() == 1 ? silt::numNotes + (size ? 0 : 1) : silt::numRatios;
            for (int k = 0; k < count; ++k)
                if (const double n = (double) k / (count - 1); gridText(grid(), size, n) == s.trim()) return (float) travels[t].from(n);
            return parse(s) * (s.endsWithIgnoreCase(" s") || s.endsWithIgnoreCase("sec") ? 1000.0f : 1.0f);
        };
        return std::pair { text, from };
    };
    const auto [sizeText, sizeFrom] = timed(SizeTravel, true);
    const auto [backText, backFrom] = timed(BackTravel, false);
    return { num(Mix, "Mix", 0, 100, 100, 0.1f, {}, "%", percent),
             num(Output, "Output", -24, 12, 0, 0.1f, {}, "dB", [=](float v) { return sign(v) + String(v, 1) + " dB"; }),
             num(Size, "Size", 0.5f, 4000, 220, 0.01f, travels[SizeTravel], "ms", sizeText, sizeFrom),
             choice(GridMode, "Grid", { "free", "note", "lock" }, 0),
             num(Density, "Density", 1, 16, 3, 0.01f, Travel { 1, 16, 1, true }, "x", [](float v) { return String(v, v < 10 ? 2 : 1) + "x"; }),
             num(Position, "Position", 0, 8000, 180, 0.1f, travels[BackTravel], "ms", backText, backFrom),
             num(Spray, "Spray", 0, 2000, 140, 0.1f, travels[SprayTravel], "ms", [](float v) { return msText(v); }),
             choice(SprayDir, "Spray Direction", { "both", "back", "ahead" }, 1),
             num(Slope, "Spray Slope", 0, 100, 40, 0.1f, {}, "%", percent),
             num(Scan, "Scan", -100, 100, 0, 0.1f, {}, "%", signedPercent),
             num(Pitch, "Pitch", -48, 48, 0, 0.01f, {}, "st", [](float v) { return (v > 0 ? "+" : "") + String(v, std::abs(v - std::round(v)) < 0.005f ? 0 : 2) + " st"; }),
             num(Fine, "Fine", -100, 100, 0, 0.1f, {}, "ct", [=](float v) { return sign(v) + String(roundToInt(v)) + " ct"; }),
             num(PitchRand, "Pitch Random", 0, 24, 0.15f, 0.01f, travels[RandTravel], "st", [](float v) { return String(v, v < 10 ? 2 : 1) + " st"; }),
             choice(Snap, "Pitch Snap", { "off", "semi", "5th", "oct" }, 0),
             choice(PitchMode, "Pitch Mode", { "speed", "pitch" }, 1),
             num(Reverse, "Reverse", 0, 100, 15, 0.1f, {}, "%", percent),
             choice(Window, "Window", StringArray(silt::windowNames, silt::numWindows), 10),
             num(Skew, "Skew", 0, 100, 50, 0.1f, {}, "%", percent),
             choice(Trigger, "Trigger", { "clock", "input", "side", "clk+in" }, 0),
             onOff(Sync, "Sync", false),
             num(Rate, "Rate", 0.25f, 50, 5, 0.01f, Travel { 0.25, 50, 1, true }, "Hz", [](float v) { return String(v, v < 10 ? 2 : 1) + " Hz"; }),
             choice(Division, "Division", StringArray(silt::noteNames, silt::numNotes), 7),
             num(Jitter, "Jitter", 0, 100, 50, 0.1f, {}, "%", percent),
             num(Thresh, "Threshold", -60, 0, -30, 0.1f, {}, "dB", [=](float v) { return trim(v) + " dB"; }),
             choice(Source, "Source", { "input", "side" }, 0),
             num(Width, "Width", 0, 100, 70, 0.1f, {}, "%", percent),
             num(Pan, "Pan Random", 0, 100, 40, 0.1f, {}, "%", percent),
             onOff(Freeze, "Freeze", false),
             num(Feedback, "Feedback", 0, 100, 25, 0.1f, {}, "%", percent),
             num(Tone, "Tone", -100, 100, -20, 0.1f, {}, "%", signedPercent),
             num(VerbSize, "Space", 0, 100, 60, 0.1f, {}, "%", percent), // SILT_REVERB
             num(VerbMix, "Reverb", 0, 100, 35, 0.1f, {}, "%", percent), // SILT_REVERB
             num(Blur, "Blur", 0, 100, 0, 0.1f, {}, "%", percent),
             num(Drift, "Drift", 0, 100, 0, 0.1f, {}, "%", percent),
             num(Bloom, "Bloom", 0, 100, 0, 0.1f, {}, "%", percent),
             num(Glint, "Glint", 0, 100, 0, 0.1f, {}, "%", percent) };
}

std::optional<double> number(const String& t) // the whole text is one finite number (C locale); else nothing
{
    std::istringstream in(t.toStdString());
    in.imbue(std::locale::classic());
    double v = 0;
    return in >> v && std::isfinite(v) && (in >> std::ws).eof() ? std::optional(jlimit(-1.0e6, 1.0e6, v)) : std::nullopt;
}

double release(double p, double hold, double k) { return p < hold ? 1 : std::pow(std::max(0.0, 1 - (p - hold) / (1 - hold)), k); } // gulp's, normalised

// ================================ The capture buffer ================================
// A ring of the input per channel plus four mipmaps (each a halfband-filtered half of the one above), so grains
// pitched up read a band-limited copy: pitch r in (2^k, 2^(k+1)] crossfades levels k and k + 1 by log2 r - k, and
// every read is a 4-point Hermite. Level k's sample j sits at time 2^k j - delay[k] (the filters' linear phase).
// That replaces oversampling: no latency, and only the reads that need it pay for it.
constexpr int levels = 5;
constexpr int delay[levels] { 0, 7, 21, 49, 105 };
constexpr float halfband[4] { 0.298482f, -0.058813f, 0.010946f, -0.000665f }; // taps 1, 3, 5, 7 (centre 0.5), Blackman sinc
constexpr int margin = 192; // samples a read keeps from the write head: the filters' delay and the interpolation

float hermite(const std::vector<float>& d, size_t mask, double pos)
{
    const double whole = std::floor(pos);
    const auto i = (int64) whole;
    const float f = (float) (pos - whole), a = d[(size_t) (i - 1) & mask], b = d[(size_t) i & mask], c = d[(size_t) (i + 1) & mask], e = d[(size_t) (i + 2) & mask];
    return b + f * (0.5f * (c - a) + f * (a - 2.5f * b + 2 * c - 0.5f * e + f * (0.5f * (e - a) + 1.5f * (b - c))));
}

struct Capture
{
    std::vector<float> data[levels][2];
    size_t mask[levels] {};
    int64 head = 0;        // frames written
    float gain = 1;        // the write crossfade: 1 live, 0 frozen (writing stops)
    float peak = 0;        // the editor's waveform: the peak of the slice being written
    int count = 0, slice = 1;
    double history = 384000;
    std::array<std::atomic<float>, silt::Feed::bins>* wave = nullptr;
    std::atomic<int>* at = nullptr;

    void prepare(size_t size, int sliceLength, double frames)
    {
        for (int k = 0; k < levels; ++k)
            for (auto& c : data[k]) c.assign(size >> k, 0.0f);
        for (int k = 0; k < levels; ++k) mask[k] = (size >> k) - 1;
        slice = jmax(1, sliceLength), history = frames;
    }
    void reset()
    {
        for (auto& level : data) for (auto& c : level) std::fill(c.begin(), c.end(), 0.0f);
        head = 0, gain = 1, peak = 0, count = 0;
    }
    // One frame, mixed into what is there by the write gain, then each mipmap sample it completes.
    void write(float l, float r)
    {
        auto& d = data[0];
        const size_t i = (size_t) head & mask[0];
        d[0][i] += gain * (l - d[0][i]), d[1][i] += gain * (r - d[1][i]);
        for (int k = 1, j = (int) (head & 0x7fffffff); k < levels && j % 2 == 0; ++k, j /= 2)
            for (int c = 0; c < 2; ++c)
            {
                const auto& x = data[k - 1][c];
                const auto at_ = [&](int o) { return x[(size_t) (j - o) & mask[k - 1]]; };
                data[k][c][(size_t) (j / 2) & mask[k]] = 0.5f * at_(7) + halfband[0] * (at_(6) + at_(8)) + halfband[1] * (at_(4) + at_(10))
                                                       + halfband[2] * (at_(2) + at_(12)) + halfband[3] * (at_(0) + at_(14));
            }
        ++head;
        peak = jmax(peak, std::abs(d[0][i]), std::abs(d[1][i]));
        if (++count >= slice)
        {
            const int k = at->load(std::memory_order_relaxed);
            (*wave)[(size_t) k].store(peak, std::memory_order_relaxed), at->store((k + 1) % silt::Feed::bins, std::memory_order_relaxed);
            peak = 0, count = 0;
        }
    }
    // Both channels at time t (frames, fractional) from level k, crossfaded by b towards level k + 1.
    void readAt(double t, int k, float b, float& l, float& r) const
    {
        const double j = (t + delay[k]) / (1 << k);
        l = hermite(data[k][0], mask[k], j), r = hermite(data[k][1], mask[k], j);
        if (b > 0)
        {
            const double j2 = (t + delay[k + 1]) / (2 << k);
            l += b * (hermite(data[k + 1][0], mask[k + 1], j2) - l), r += b * (hermite(data[k + 1][1], mask[k + 1], j2) - r);
        }
    }
    void read(double t, int k, float b, float& l, float& r) const
    {
        // Very long synced / reverse / frozen grains loop within recent history. A short splice avoids a step
        // when a reader crosses either edge; ordinary reads away from the edges are unchanged.
        constexpr double fade = 128;
        const double lo = (double) head - history + margin, hi = (double) head - margin, period = hi - lo - fade;
        if (t < lo + fade || t >= hi) t -= period * std::floor((t - lo - fade) / period);
        readAt(t, k, b, l, r);
        if (t > hi - fade)
        {
            float l2, r2;
            readAt(t - period, k, b, l2, r2);
            const float mix = (float) ((t - hi + fade) / fade);
            l += mix * (l2 - l), r += mix * (r2 - r);
        }
    }

};

// ================================ Grains ================================
struct Grain
{
    int wait = 0, left = 0;    // samples until it sounds, samples it still sounds (0 = a free voice)
    double phase = 0, inc = 0;  // through the window
    double pos = 0, step = 1;  // speed: the read position and its step (+-r); pitch: the anchor, moving +-1
    float tap = 0, tapStep = 0, span = 0; // pitch: the first tap's distance behind the anchor, its step, the tap window
    float gl = 0, gr = 0, blend = 0, pitch = 0;
    float peak = 0.5f, attack = 1, decay = 1;
    int shape = 10;
    int level = 0, source = 0;
    bool shifted = false;      // pitch mode: two crossfaded taps keep the timeline while the pitch moves
};

// Transients: a peak follower that jumps 6 dB over its own slow (30 ms) copy above the threshold; re-armed once the
// fast one falls back near the slow one, 50 ms apart at least. A steady tone never fires; each note's attack does.
struct Onset
{
    float fast = 0, slow = 0;
    int hold = 0;
    bool armed = true;
    bool step(float x, float threshold, float fastRelease, float slowAttack, float slowRelease, int holdoff)
    {
        fast = jmax(std::abs(x), fast * fastRelease);
        slow += (fast > slow ? slowAttack : slowRelease) * (fast - slow);
        hold = jmax(0, hold - 1);
        if (armed && hold == 0 && fast > threshold && fast > 2 * slow)
            return armed = false, hold = holdoff, true;
        armed = armed || fast < 1.25f * slow;
        return false;
    }
};

struct OnePole // TPT one-pole: lowpass and highpass of the same state
{
    float s = 0;
    float low(float x, float g) { const float v = (x - s) * g, y = v + s; s = y + v; return y; }
};
float coefficient(double hz, double fs) { const double g = std::tan(pi * jmin(hz, 0.45 * fs) / fs); return (float) (g / (1 + g)); }
} // namespace

const char* const silt::windowNames[] = { "drop", "needle", "knock", "lean", "fade", "slab", "mesa", "dome", "gate", "bulge", "hann", "twin" };
const char* const silt::noteNames[] = { "1/64", "1/32T", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2T", "1/2", "1/2D", "1 bar", "2 bars" };

double silt::window(int shape, double x, double skew)
{
    const double k = 0.02 + 0.96 * skew, p = x < k ? 1 - x / k : (x - k) / (1 - k);
    switch (shape)
    {
        case 0: return release(p, 0, 2.2);
        case 1: return release(p, 0, 3.0);
        case 2: return release(p, 0.19, 1.5);
        case 3: return release(p, 0, 1.2);
        case 4: return release(p, 0, 1.8);
        case 5: return release(p, 0.83, 1.0);
        case 6: return release(p, 0.11, 1.0);
        case 7: return release(p, 0.16, 0.8);
        case 8: return release(p, 0.65, 0.5);
        case 9: return 1 - std::pow(p, 1.6);
        case 10: return 0.5 + 0.5 * std::cos(pi * p);
        default: return p < 0.5 ? 0.5 + 0.5 * std::pow(1 - 2 * p, 2.2) : 0.5 * std::pow(2 - 2 * p, 2.2);
    }
}

// ================================ The engine ================================
// Read parameters once per host block. Events, capture, active voices and feedback advance sample by sample;
// the fixed 32-sample feedback delay is independent of host block size. All storage is prepared ahead of time.
struct SiltProcessor::Engine
{
    static constexpr int K = 32;
    std::array<std::atomic<float>*, silt::numParams> raw {};
    float v[silt::numParams] {}; // this block's parameters
    double fs = 48000, tempo = 120;
    Capture capture[2];          // the input, the sidechain
    std::array<Grain, silt::maxGrains> grains {};
    int voiceCount = 0;
    std::array<float, 1025> env[silt::numWindows] {}, taps {}; // immutable windows; sin^2 pitch taps
    Onset onsets[2];
    float fastRelease = 0, slowAttack = 0, slowRelease = 0, freezeStep = 0, ccGlide = 0;
    int holdoff = 0;
    // The clock: grains (stream phase) since the last tick; sync while playing: the last whole grain on the host's grid.
    double phase = 0, lastTick = -1;
    bool wasPlaying = false;
    int stream = 0;
    double scanned = 0;          // the scan's extra distance back (samples)
    uint32 seed = 1;
    // MIDI: held notes (last-note priority) and the fixed controller map's smoothed values, -1 until received.
    std::array<int, 128> held {};
    std::array<float, 128> velocities {};
    int heldCount = 0;
    float velocity = 1;
    float cc[6] {}, ccTarget[6] {};
    // Output side
    
    float fbRing[2][K] {};
    int64 now = 0;               // samples since reset
    double overlap = 0;          // smoothed sum of the windows sounding
    float overlapAttack = 0, overlapRelease = 0;
    OnePole lows[2], highs[2], dc[2];
    std::vector<float> diffusion[4];
    size_t diffusionAt[4] {};
    float level = 0, inputLevel = 0, levelGlide = 0, inputAttack = 0, inputRelease = 0;
    SmoothedValue<float> mix, gain, verb, width, feedback, lowpass, highpass, power;
    SiltReverb reverb; // SILT_REVERB

    void prepare(double sr, silt::Feed& feed)
    {
        fs = sr > 0 && sr < 1.0e7 ? sr : 48000.0;
        const auto size = (size_t) nextPowerOfTwo((int) ((silt::maxBackSeconds + 2.5) * fs));
        for (int c = 0; c < 2; ++c)
        {
            capture[c].wave = &feed.wave[c], capture[c].at = &feed.at[c];
            capture[c].prepare(size, (int) (silt::maxBackSeconds * fs / silt::Feed::bins), silt::maxBackSeconds * fs);
        }
        for (int k = 0; k < 4; ++k) diffusion[k].assign((size_t) (fs * (k < 2 ? 0.0047 : 0.0071)) + 1, 0.0f);
        reverb.prepare(fs); // SILT_REVERB
        fastRelease = (float) std::exp(-1 / (0.015 * fs)), slowAttack = (float) (1 - std::exp(-1 / (0.03 * fs)));
        slowRelease = (float) (1 - std::exp(-1 / (0.3 * fs))), holdoff = (int) (0.05 * fs);
        freezeStep = (float) (1 / (0.02 * fs)), ccGlide = (float) (1 - std::exp(-1 / (0.03 * fs)));
        overlapAttack = (float) (1 - std::exp(-1 / (0.002 * fs))), overlapRelease = (float) (1 - std::exp(-1 / (0.08 * fs)));
        levelGlide = (float) (1 - std::exp(-1 / (0.1 * fs)));
        inputAttack = (float) (1 - std::exp(-1 / (0.01 * fs))), inputRelease = (float) (1 - std::exp(-1 / (2.0 * fs)));
        for (auto* s : { &mix, &gain, &verb, &width, &feedback, &lowpass, &highpass, &power }) s->reset(fs, 0.02);
        for (size_t j = 0; j < taps.size(); ++j)
        {
            taps[j] = (float) std::pow(std::sin(pi * (double) j / 1024), 2);
            for (int shape = 0; shape < silt::numWindows; ++shape) env[shape][j] = (float) silt::window(shape, (double) j / 1024, 0.5);
        }
    }
    void reset(silt::Feed& feed)
    {
        for (auto& c : capture) c.reset();
        voiceCount = 0, grains = {}, onsets[0] = onsets[1] = {}, phase = 0, lastTick = -1, wasPlaying = false, stream = 0, scanned = 0, seed = 0x5117u;
        heldCount = 0, velocity = 1, now = 0, overlap = 0, level = inputLevel = 0;
        std::fill(std::begin(cc), std::end(cc), -1.0f), std::fill(std::begin(ccTarget), std::end(ccTarget), -1.0f);
        for (auto& r : fbRing) std::fill(std::begin(r), std::end(r), 0.0f);
        for (auto& d : diffusion) std::fill(d.begin(), d.end(), 0.0f);
        std::fill(std::begin(diffusionAt), std::end(diffusionAt), (size_t) 0);
        lows[0] = lows[1] = highs[0] = highs[1] = dc[0] = dc[1] = {};
        reverb.reset(); // SILT_REVERB
        for (int c = 0; c < 2; ++c)
        {
            for (auto& w : feed.wave[c]) w.store(0, std::memory_order_relaxed);
            feed.at[c].store(0, std::memory_order_relaxed);
        }
        feed.fired.store(0, std::memory_order_relaxed);
        feed.lastFire.store(-1, std::memory_order_relaxed), feed.lastPitch.store(0, std::memory_order_relaxed), feed.lastLength.store(0, std::memory_order_relaxed);
        read();
        width.setCurrentAndTargetValue(v[Width] / 100);
        feedback.setCurrentAndTargetValue(jmin(1.0f, v[Feedback] / 100 + 0.35f * macro(Bloom) + 0.2f * macro(Glint)) * 0.85f);
        const float tone = jlimit(-1.0f, 1.0f, v[Tone] / 100);
        lowpass.setCurrentAndTargetValue(coefficient(20000 * std::pow(0.04, jmax(0.0f, -tone)), fs));
        highpass.setCurrentAndTargetValue(coefficient(20 * std::pow(100.0, jmax(0.0f, tone)), fs));
        power.setCurrentAndTargetValue((float) (1 - 0.5 * jlimit(0.0, 1.0, separation())));
        mix.setCurrentAndTargetValue(v[Mix] / 100), gain.setCurrentAndTargetValue(Decibels::decibelsToGain(v[Output])), verb.setCurrentAndTargetValue(0);
    }
    void read()
    {
        for (size_t i = 0; i < raw.size(); ++i) v[i] = raw[i]->load(std::memory_order_relaxed);
    }
    float envelope(const Grain& g) const
    {
        return lookup(env[g.shape], g.phase < g.peak ? g.phase * g.attack : 0.5f + (g.phase - g.peak) * g.decay);
    }

    float random() { seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5; return (float) (seed >> 8) * 0x1p-24f; } // 0..1
    float macro(int i) const { return v[i] / 100; }
    float control(int k) const { return jmax(0.0f, cc[k]); } // a controller's value, 0 until received
    static float lookup(const std::array<float, 1025>& t, float x)
    {
        const float at = jlimit(0.0f, 1024.0f, x * 1024);
        const int j = jmin(1023, (int) at);
        return t[(size_t) j] + (at - (float) j) * (t[(size_t) j + 1] - t[(size_t) j]);
    }

    double separation() const
    {
        return (v[Spray] + 300 * macro(Blur)) / jmax(1.0f, v[Size]) + v[PitchRand]
             + std::abs(v[Fine]) / 100 + v[Reverse] / 100 + std::abs(v[Scan] / 100.0 - 0.6 * macro(Drift));
    }
    double position(double bpm) const
    {
        const double n = travels[BackTravel].to(v[Position]);
        if (v[GridMode] < 0.5f) return v[Position] * fs / 1000;
        if (v[GridMode] > 1.5f) return silt::backRatio[pick(n, silt::numRatios)] * period(bpm);
        const int k = pick(n, silt::numNotes + 1) - 1;
        return k < 0 ? 0.0 : 240.0 * silt::noteNum[k] / silt::noteDen[k] / bpm * fs;
    }

    // The clock in samples per tick (the host tempo or 120 in sync; else the free rate) and grains per tick.
    double period(double bpm) const { const int d = roundToInt(v[Division]); return v[Sync] > 0.5f ? 240.0 * silt::noteNum[d] / silt::noteDen[d] / bpm * fs : fs / v[Rate]; }
    double density() const { return jlimit(1.0, 64.0, v[Density] * (1 + 2.0 * macro(Blur)) * (1 + 3.0 * control(1))); }

    // Starts one grain after `wait` samples; alternating streams have independent pan and a small size difference.
    void spawn(int wait, double bpm, bool frozen, int side)
    {
        if (voiceCount == silt::maxGrains) return; // all voices busy: skip this one, never steal (no clicks)
        auto* g = &grains[(size_t) voiceCount++];
        const double spacing = period(bpm) / density(), stereo = v[Width] / 100;
        const double sizeTravel = travels[SizeTravel].to(v[Size]);
        const int grid = roundToInt(v[GridMode]);
        const auto note = [&](int k) { return 240.0 * silt::noteNum[k] / silt::noteDen[k] / bpm * fs; };
        double size = grid == 0 ? v[Size] * fs / 1000 : grid == 1 ? note(pick(sizeTravel, silt::numNotes)) : silt::sizeRatio[pick(sizeTravel, silt::numRatios)] * spacing;
        size *= (1 + 2.0 * macro(Blur)) * (cc[4] < 0 ? 1.0 : std::exp2(4.0 * cc[4] - 2)) * (side == 1 ? 1 + 0.04 * stereo : 1.0);
        size = jlimit(0.0005 * fs, grid == 0 ? 4 * fs : jmax(4 * fs, note(silt::numNotes - 1)), size);
        // Pitch: base + random, snapped (after the randomness), then fine, the held note and glint's octaves.
        double st = v[Pitch] + (2 * random() - 1) * (v[PitchRand] + 0.4 * macro(Drift) + 12 * control(3));
        const int snap = roundToInt(v[Snap]);
        if (snap == 1) st = std::round(st);
        else if (snap > 1)
        {
            const double octave = 12 * std::floor(st / 12), in = st - octave;
            st = octave + (snap == 3 ? 12 * std::round(in / 12) : in < 3.5 ? 0 : in < 9.5 ? 7 : 12);
        }
        st += v[Fine] / 100 + (heldCount > 0 ? held[(size_t) heldCount - 1] - 60 : 0) + (random() < 0.6f * macro(Glint) ? 12 : 0);
        st = jlimit(-48.0, 48.0, st);
        const double r = std::exp2(st / 12);
        const bool reverse = random() * 100 < v[Reverse], shifted = v[PitchMode] > 0.5f;
        const double dir = reverse ? -1 : 1;
        // The read: how long it sounds and how much source it covers.
        const double dur = jmin(2.0e9, shifted ? size : size / r), span = shifted ? jmin(0.04 * fs, size) : 0;
        // How far back it starts: position (+ the scan) + spray, kept inside what the buffer holds for the whole grain
        // (live, the head moves on while it plays; frozen, it stands still). a: room needed ahead, b: behind.
        const double maxBack = silt::maxBackSeconds * fs;
        double back = position(bpm);
        const double spray = (v[Spray] + 300 * macro(Blur) + 1500 * control(0)) * fs / 1000;
        double u = random();
        u = std::pow(u, 1 + 4 * v[Slope] / 100.0); // slope: small distances more likely
        const int towards = roundToInt(v[SprayDir]);
        back += spray * (towards == 1 ? u : towards == 2 ? -u : (random() < 0.5f ? u : -u)) + scanned;
        const double extent = shifted ? size : dur * r;
        double a = 0, b = 0;
        if (frozen) a = extent + (shifted && reverse ? span : 0), b = shifted && !reverse ? span : 0;
        else if (shifted) a = reverse ? size + span : 0, b = reverse ? size : span;
        else if (reverse) a = extent, b = dur;
        else a = jmax(0.0, dur * (r - 1)), b = jmax(0.0, dur * (1 - r));
        // Fit the whole segment when possible; longer notes retain their duration and use the capture's splice.
        back = a + b < maxBack - 2 * margin ? jlimit(margin + a, maxBack - margin - b, back) : jlimit((double) margin, maxBack - margin, back);
        const double start = (double) capture[source()].head + (frozen ? 0 : wait) - back;
        g->wait = wait, g->left = jmax(2, (int) dur), g->phase = 0, g->inc = 1.0 / g->left;
        g->shape = roundToInt(v[Window]), g->peak = 0.02f + 0.96f * v[Skew] / 100;
        g->attack = 0.5f / g->peak, g->decay = 0.5f / (1 - g->peak);
        g->shifted = shifted && std::abs(r - 1) > 1e-8, g->source = source(), g->pitch = (float) st;
        g->step = shifted ? dir : dir * r;
        g->pos = shifted ? (reverse ? start + size : start) : (reverse ? start + dur * r : start);
        g->span = (float) jmax(2.0, 2 * std::round(span / 2)), g->tap = 0, g->tapStep = (float) (1 - r);
        g->level = r <= 1 ? 0 : jmin(levels - 1, (int) std::floor(std::log2(r)));
        g->blend = r <= 1 || g->level == levels - 1 ? 0.0f : (float) (std::log2(r) - g->level);
        // Pan: the stream's side of the width plus a random spread; equal power, unity in the middle.
        const double pan = jlimit(-1.0, 1.0, (side == 0 ? -0.6 : 0.6) * stereo + (2 * random() - 1) * jmin(1.0, v[Pan] / 100.0 + 0.6 * macro(Drift)));
        const double angle = (pan + 1) * pi / 4;
        g->gl = (float) (velocity * std::cos(angle) * std::sqrt(2.0)), g->gr = (float) (velocity * std::sin(angle) * std::sqrt(2.0));
    }
    int source() const { return roundToInt(v[Source]); }
    void burst(int at, double bpm, bool frozen) // a MIDI note or a transient: density grains over one tick
    {
        const double spacing = period(bpm) / density();
        const int count = jmin(16, jmax(1, roundToInt(density())));
        for (int k = 0; k < count; ++k)
            spawn(at + (int) (k * spacing + (k > 0 ? random() * v[Jitter] / 100 * spacing : 0)), bpm, frozen, stream ^= 1);
    }
    void midi(const MidiMessage& m, int at, double bpm, bool frozen)
    {
        if (m.isAllNotesOff() || m.isAllSoundOff())
            heldCount = 0, velocity = 1;
        else if (m.isNoteOn())
        {
            const auto end = std::remove(held.begin(), held.begin() + heldCount, m.getNoteNumber());
            heldCount = (int) (end - held.begin());
            held[(size_t) heldCount++] = m.getNoteNumber();
            velocities[(size_t) m.getNoteNumber()] = velocity = m.getFloatVelocity();
            burst(at, bpm, frozen);
        }
        else if (m.isNoteOff())
        {
            const auto end = std::remove(held.begin(), held.begin() + heldCount, m.getNoteNumber());
            heldCount = (int) (end - held.begin());
            velocity = heldCount > 0 ? velocities[(size_t) held[(size_t) heldCount - 1]] : 1;
        }
        else if (m.isController())
        {
            // Fixed offsets: CC1 spray, CC2 density, CC64 freeze, CC71 pitch random, CC74 size, CC91 reverb.
            constexpr int map[] { 1, 2, 64, 71, 74, 91 };
            for (int k = 0; k < 6; ++k)
                if (map[k] == m.getControllerNumber())
                {
                    if (cc[k] < 0) cc[k] = k == 4 ? 0.5f : 0.0f;
                    ccTarget[k] = (float) m.getControllerValue() / 127;
                }
            if (m.getControllerNumber() == 121)
                std::fill(std::begin(cc), std::end(cc), -1.0f), std::fill(std::begin(ccTarget), std::end(ccTarget), -1.0f);
        }
    }

    void process(SiltProcessor& p, AudioBuffer<float>& buffer, MidiBuffer& midiBuffer)
    {
        read();
        auto& feed = p.feed;
        const int n = buffer.getNumSamples(), ins = jmin(2, p.getMainBusNumInputChannels()), outs = jmin(2, p.getMainBusNumOutputChannels());
        const auto sc = p.getBusBuffer(buffer, true, 1);
        const int scs = jmin(2, sc.getNumChannels());
        // The host's tempo (120 without one) and, while it plays, its position for the synced clock.
        double bpm = 120, ppq = 0;
        bool playing = false;
        if (auto* head = p.getPlayHead())
            if (const auto pos = head->getPosition())
            {
                if (const auto b = pos->getBpm(); b && std::isfinite(*b) && *b > 0) bpm = jlimit(20.0, 999.0, *b);
                if (const auto q = pos->getPpqPosition(); q && std::isfinite(*q) && pos->getIsPlaying()) playing = true, ppq = *q;
            }
        tempo = bpm;
        const bool synced = v[Sync] > 0.5f && playing;
        if (synced)
        {
            const double quarters = 4.0 * silt::noteNum[roundToInt(v[Division])] / silt::noteDen[roundToInt(v[Division])] / density();
            const double target = ppq / quarters;
            if (!wasPlaying || std::abs(target - phase) > 1e-5) lastTick = std::ceil(target - 1e-9) - 1;
            phase = target;
        }
        else if (wasPlaying) phase -= std::floor(phase);
        wasPlaying = synced;
        const int trigger = roundToInt(v[Trigger]);
        const float threshold = Decibels::decibelsToGain(v[Thresh]);
        mix.setTargetValue(v[Mix] / 100), gain.setTargetValue(Decibels::decibelsToGain(v[Output]));
        const double scan = jlimit(-1.0, 1.0, v[Scan] / 100.0 - 0.6 * macro(Drift));
        feedback.setTargetValue(jmin(1.0f, v[Feedback] / 100 + 0.35f * macro(Bloom) + 0.2f * macro(Glint)) * 0.85f);
        width.setTargetValue(v[Width] / 100);
        const float tone = jlimit(-1.0f, 1.0f, v[Tone] / 100);
        lowpass.setTargetValue(coefficient(20000 * std::pow(0.04, jmax(0.0f, -tone)), fs));
        highpass.setTargetValue(coefficient(20 * std::pow(100.0, jmax(0.0f, tone)), fs));
        const float dcCoef = coefficient(20, fs);
        // Similar grains add in amplitude; scattered/pitched grains add mostly in power. Interpolate the overlap
        // law using their source separation, preserving level without analysing or limiting the output.
        const double apart = separation();
        const double sizeMs = jmax(1.0f, v[Size]);
        auto event = midiBuffer.cbegin();
        for (int t = 0; t < n; ++t)
        {
            bool frozen = v[Freeze] > 0.5f || cc[2] >= 0.5f;
            for (; event != midiBuffer.cend() && (*event).samplePosition <= t; ++event)
                if ((*event).numBytes <= 3) midi((*event).getMessage(), 0, bpm, frozen); // ignore SysEx without allocating
            for (int k = 0; k < 6; ++k)
                if (ccTarget[k] >= 0) cc[k] += ccGlide * (ccTarget[k] - cc[k]);
            frozen = v[Freeze] > 0.5f || cc[2] >= 0.5f;
            float x[2], side[2] {}, dry[2];
            for (int c = 0; c < 2; ++c)
            {
                dry[c] = buffer.getSample(jmin(c, ins - 1), t);
                x[c] = std::isfinite(dry[c]) ? jlimit(-8.0f, 8.0f, dry[c]) : 0.0f;
                const float z = scs > 0 ? sc.getSample(jmin(c, scs - 1), t) : 0.0f;
                side[c] = std::isfinite(z) ? jlimit(-8.0f, 8.0f, z) : 0.0f;
            }
            const bool hitIn = onsets[0].step(jmax(std::abs(x[0]), std::abs(x[1])), threshold, fastRelease, slowAttack, slowRelease, holdoff);
            const bool hitSide = onsets[1].step(jmax(std::abs(side[0]), std::abs(side[1])), threshold, fastRelease, slowAttack, slowRelease, holdoff);
            const float input = 0.5f * (std::abs(x[0]) + std::abs(x[1]));
            inputLevel += (input > inputLevel ? inputAttack : inputRelease) * (input - inputLevel);
            for (int c = 0; c < 2; ++c)
            {
                auto& cap = capture[c];
                cap.gain = jlimit(0.0f, 1.0f, cap.gain + (frozen ? -freezeStep : freezeStep));
                const float* in = c == 0 ? x : side;
                const float add = c == source() ? 1.0f : 0.0f;
                if (cap.gain > 0) cap.write(in[0] + add * fbRing[0][now % K], in[1] + add * fbRing[1][now % K]);
            }
            if (std::abs(scan) < 1e-9) scanned = 0;
            else
            {
                const double range = jmax(1.0, silt::maxBackSeconds * fs - v[Position] * fs / 1000);
                scanned -= scan, scanned -= range * std::floor(scanned / range);
            }
            if (trigger == 0 || trigger == 3)
            {
                const double spacing = period(bpm) / density();
                bool tick = false;
                if (synced)
                {
                    phase = (ppq + t * bpm / 60 / fs) / (4.0 * silt::noteNum[roundToInt(v[Division])] / silt::noteDen[roundToInt(v[Division])]) * density();
                    const double whole = std::floor(phase + 1e-9);
                    tick = !exactlyEqual(whole, lastTick), lastTick = whole;
                    phase += 1 / spacing;
                }
                else if ((phase += 1 / spacing) >= 1 - 1e-10)
                    tick = true, phase -= std::floor(phase + 1e-10);
                if (tick) spawn((int) (random() * v[Jitter] / 100 * spacing), bpm, frozen, stream ^= 1);
            }
            if ((hitIn && (trigger == 1 || trigger == 3)) || (hitSide && trigger == 2)) burst(0, bpm, frozen);
            float y[2] {}, weight = 0;
            for (int k = 0; k < voiceCount; ++k)
            {
                auto& g = grains[(size_t) k];
                if (g.wait > 0) { --g.wait; continue; }
                if (exactlyEqual(g.phase, 0.0))
                {
                    feed.fired.fetch_add(1, std::memory_order_relaxed);
                    feed.lastFire.store(now, std::memory_order_relaxed);
                    feed.lastPitch.store(g.pitch, std::memory_order_relaxed);
                    feed.lastLength.store(g.left, std::memory_order_relaxed);
                }
                const auto& cap = capture[g.source];
                const float w = envelope(g);
                g.phase += g.inc, --g.left;
                float l, r;
                if (!g.shifted) cap.read(g.pos, g.level, g.blend, l, r), g.pos += g.step;
                else
                {
                    float l2, r2;
                    const float second = g.tap + 0.5f * g.span >= g.span ? g.tap - 0.5f * g.span : g.tap + 0.5f * g.span;
                    const float a = lookup(taps, g.tap / g.span), b = 1 - a;
                    cap.read(g.pos - g.step * g.tap, g.level, g.blend, l, r), cap.read(g.pos - g.step * second, g.level, g.blend, l2, r2);
                    l = a * l + b * l2, r = a * r + b * r2;
                    g.pos += g.step, g.tap += g.tapStep;
                    g.tap -= g.span * std::floor(g.tap / g.span);
                }
                y[0] += w * g.gl * l, y[1] += w * g.gr * r, weight += w;
                if (g.left == 0) g = grains[(size_t) --voiceCount], --k;
            }
            // Actual window overlap, with a slow release between clouds. No compressor or limiter.
            overlap += (weight > overlap ? overlapAttack : overlapRelease) * (weight - overlap);
            power.setTargetValue((float) (1 - 0.5 * jlimit(0.0, 1.0, apart + 1500 * control(0) / sizeMs + 12 * control(3))));
            const float exponent = power.getNextValue();
            const float norm = (float) (0.85 / std::pow(jmax(1.0f, weight, (float) overlap), exponent));
            const float mid = 0.5f * (y[0] + y[1]) * norm, sd = 0.5f * (y[0] - y[1]) * norm * width.getNextValue();
            y[0] = mid + sd, y[1] = mid - sd;
            const float lp = lowpass.getNextValue(), hp = highpass.getNextValue(), fbAmount = feedback.getNextValue();
            float f[2];
            for (int c = 0; c < 2; ++c)
            {
                y[c] -= dc[c].low(y[c], dcCoef);
                const float low = lows[c].low(y[c], lp), high = low - highs[c].low(low, hp);
                f[c] = diffuse(diffuse(high, (size_t) c), (size_t) c + 2);
            }
            level += levelGlide * (fbAmount * 0.5f * (std::abs(f[0]) + std::abs(f[1])) - level);
            const float guard = level > 1e-6f ? jmin(1.0f, jmax(0.5f * inputLevel, 0.01f) / level) : 1.0f;
            for (int c = 0; c < 2; ++c) fbRing[c][now % K] = 0.5f * std::tanh(2 * fbAmount * guard * f[c]);
            // SILT_REVERB: removable grain-output reverb; keep its frozen parameters inert if removed.
            verb.setTargetValue(jmin(1.0f, v[VerbMix] / 100 + 0.5f * macro(Bloom) + control(5)));
            float rl = y[0], rr = y[1];
            reverb.process(rl, rr, jmin(1.0f, v[VerbSize] / 100 + 0.4f * macro(Bloom)));
            const float vm = verb.getNextValue();
            y[0] += vm * (rl - y[0]), y[1] += vm * (rr - y[1]);
            const float m = mix.getNextValue(), out = gain.getNextValue();
            if (outs == 1) y[0] = 0.5f * (y[0] + y[1]);
            for (int c = 0; c < outs; ++c)
                buffer.setSample(c, t, exactlyEqual(m, 0.0f) ? dry[c] : exactlyEqual(m, 1.0f) ? y[c] * out : dry[c] + m * (y[c] * out - dry[c]));
            ++now;
        }
        publish(feed);
    }
    float diffuse(float x, size_t k)
    {
        auto& d = diffusion[k];
        const float delayed = d[diffusionAt[k]], w = x + 0.5f * delayed;
        d[diffusionAt[k]] = std::abs(w) < 1e-15f ? 0.0f : w;
        diffusionAt[k] = (diffusionAt[k] + 1) % d.size();
        return delayed - 0.5f * w;
    }
    void publish(silt::Feed& feed)
    {
        size_t k = 0;
        for (int i = 0; i < voiceCount; ++i)
            if (const auto& g = grains[(size_t) i]; g.left > 0 && g.wait == 0 && k < (size_t) silt::Feed::dots)
            {
                feed.back[k].store((float) (((double) capture[g.source].head - g.pos) / fs), std::memory_order_relaxed);
                feed.pitch[k].store(g.pitch, std::memory_order_relaxed);
                feed.env[k++].store(jmax(0.001f, envelope(g)), std::memory_order_relaxed);
            }
        for (; k < (size_t) silt::Feed::dots; ++k) feed.env[k].store(0, std::memory_order_relaxed);
        feed.centre.store((float) ((position(tempo) + scanned) / fs), std::memory_order_relaxed);
        feed.frozen.store(capture[source()].gain <= 0, std::memory_order_relaxed);
    }
};

SiltProcessor::SiltProcessor(File directory)
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", AudioChannelSet::stereo(), false).withOutput("Output", AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "SILT", layout(*this)), presetDirectory(directory), engine(std::make_unique<Engine>())
{
    for (int i = 0; i < silt::numParams; ++i) engine->raw[(size_t) i] = apvts.getRawParameterValue(ids[i]);
    setLatencySamples(0);
}
SiltProcessor::~SiltProcessor() = default;
void SiltProcessor::prepareToPlay(double sr, int)
{
    engine->prepare(sr, feed), resetPending = true;
    setLatencySamples(0); // band-limited capture mipmaps / Hermite reads need no processing latency
}
bool SiltProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const int in = l.getMainInputChannels(), out = l.getMainOutputChannels();
    const int side = l.inputBuses.size() > 1 ? l.inputBuses[1].size() : 0;
    return (in == 1 || in == 2) && (out == 1 || out == 2) && out >= in && side <= 2;
}
void SiltProcessor::processBlock(AudioBuffer<float>& b, MidiBuffer& m)
{
    ScopedNoDenormals noDenormals;
    if (resetPending.exchange(false)) engine->reset(feed);
    engine->process(*this, b, m);
}

// ================================ State, presets, A/B ================================
void SiltProcessor::getStateInformation(MemoryBlock& dest)
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
            if (!slots[k].empty()) // "preset;v0,..,v35", normalised, 9 significant digits: every float reads back exactly
            {
                std::ostringstream t;
                t.imbue(std::locale::classic()), t.precision(9), t << slotPreset[k] << ';';
                for (const float& v : slots[k])
                    t << v << (&v == &slots[k].back() ? "" : ",");
                state.setProperty(k == 0 ? "slotA" : "slotB", String(t.str()), nullptr);
                state.setProperty(k == 0 ? "slotNameA" : "slotNameB", slotName[k], nullptr);
            }
    }
    state.setProperty("showMacros", showMacros.load(), nullptr);
    if (editorWidth > 0)
        state.setProperty("editorWidth", editorWidth.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, dest);
}

void SiltProcessor::applyParams(const XmlElement& state)
{
    // Migration hook: v1 and future versions load known IDs; missing/nonfinite values use v1 defaults.
    // Add explicit migrations here before bumping stateVersion.
    for (int i = 0; i < silt::numParams; ++i)
    {
        auto* p = apvts.getParameter(ids[i]);
        const XmlElement* e = nullptr; // the first PARAM with this id
        for (auto* c : state.getChildWithTagNameIterator("PARAM"))
            e = e == nullptr && c->getStringAttribute("id") == ids[i] ? c : e;
        const auto v = number(e != nullptr ? e->getStringAttribute("value") : String());
        p->setValueNotifyingHost(v ? p->convertTo0to1(p->getNormalisableRange().snapToLegalValue((float) *v)) : p->getDefaultValue());
    }
}

// Malformed documents leave state untouched. Future versions load known IDs and ignore new ones.

void SiltProcessor::setStateInformation(const void* data, int size)
{
    const auto* bytes = static_cast<const char*>(data);
    if (bytes == nullptr || size < 8 || ByteOrder::littleEndianInt(bytes) != 0x21324356 || ByteOrder::littleEndianInt(bytes + 4) > (uint32) size - 8)
        return;
    const auto text = String::fromUTF8(bytes + 8, (int) ByteOrder::littleEndianInt(bytes + 4)).trimEnd();
    const auto tree = text.endsWith("</SILT>") || (text.endsWith("/>") && !text.fromFirstOccurrenceOf("<SILT", false, false).containsChar('<')) ? parseXML(text) : nullptr;
    if (tree == nullptr || !tree->hasTagName("SILT"))
        return;
    applyParams(*tree);
    const ScopedLock sl(lock);
    for (int k = 0; k < 2; ++k)
    {
        const auto t = StringArray::fromTokens(tree->getStringAttribute(k == 0 ? "slotA" : "slotB"), ";,", "");
        slots[k].clear(), slotName[k] = tree->getStringAttribute(k == 0 ? "slotNameA" : "slotNameB");
        slotPreset[k] = (int) jlimit(-1.0, silt::numPresets - 1.0, number(t[0]).value_or(-1));
        for (int i = 1; i < t.size() && i <= silt::numParams; ++i)
            if (const auto v = number(t[i]); v && *v >= 0 && *v <= 1) slots[k].push_back((float) *v);
        if (slots[k].size() != silt::numParams) slots[k].clear();
    }
    showMacros = tree->getBoolAttribute("showMacros", false);
    ab = number(tree->getStringAttribute("ab")).value_or(0) > 0.5 ? 1 : 0;
    editorWidth = (int) jlimit(0.0, 4096.0, number(tree->getStringAttribute("editorWidth")).value_or(0));
    preset = (int) jlimit(-1.0, silt::numPresets - 1.0, number(tree->getStringAttribute("preset")).value_or(-1)), userName = tree->getStringAttribute("presetName");
}

// Values in frozen ID order (see the list above). Preset 0 equals the parameter defaults.
const silt::Preset silt::presets[] = {
    { "soft sediment", {100,0,220,0,3,180,140,1,40,0,0,0,.15,0,1,15,10,50,0,0,5,7,50,-30,0,70,40,0,25,-20,60,35,0,0,0,0} },
    { "low strata",    {100,0,110,0,2,80,12,1,65,0,0,0,0,1,1,0,3,50,0,0,9,7,12,-30,0,35,10,0,12,-45,25,10,0,0,0,0} },
    { "copper dust",   {100,0,45,0,4,120,160,0,50,0,7,0,3,2,1,20,2,25,0,0,12,7,65,-30,0,75,65,0,18,10,35,20,0,0,0,0} },
    { "pale field",    {100,0,650,0,4,500,420,1,60,0,0,4,.2,0,1,30,10,55,0,0,3,7,70,-30,0,95,45,0,28,-30,75,45,0,0,0,0} },
    { "glass pollen",  {100,0,140,0,4,250,300,0,40,0,12,0,0,1,1,20,9,50,0,0,8,7,55,-30,0,85,60,0,15,-10,65,40,0,0,0,0} },
    { "root memory",   {100,0,350,0,3,650,80,1,70,0,-12,0,0,1,1,0,6,40,0,0,5,7,40,-30,0,60,30,0,35,-55,55,25,0,0,0,0} },
    { "loose thread",  {100,0,90,0,2,110,140,0,45,0,0,0,.3,0,0,45,0,30,3,0,8,7,60,-30,0,70,55,0,15,-15,40,20,0,0,0,0} },
    { "tidal paper",   {100,0,800,1,3,350,180,1,50,0,0,0,0,1,1,50,4,65,0,1,4,7,20,-30,0,90,50,0,25,-30,65,30,0,0,0,0} },
    { "amber loop",    {100,0,440,2,4,300,100,1,65,0,0,0,0,1,1,15,7,45,0,1,5,7,15,-30,0,65,25,0,40,-40,45,20,0,0,0,0} },
    { "after rain",    {100,0,1200,0,3,1300,600,0,60,0,0,-5,.2,0,1,70,10,55,0,0,2,7,80,-30,0,100,50,0,30,-25,85,30,0,0,0,0} }
};

void SiltProcessor::setAll(const float* normalised)
{
    for (int k = 0; k < silt::numParams; ++k)
        if (auto* p = apvts.getParameter(ids[k]); std::abs(p->getValue() - normalised[k]) > 1e-7f)
            p->beginChangeGesture(), p->setValueNotifyingHost(normalised[k]), p->endChangeGesture();
}

void SiltProcessor::setIdentity(int index, const String& name)
{
    const ScopedLock sl(lock);
    preset = index, userName = name;
}

void SiltProcessor::setCurrentProgram(int index)
{
    if (!isPositiveAndBelow(index, silt::numPresets))
        return;
    float v[silt::numParams];
    for (int k = 0; k < silt::numParams; ++k)
        v[k] = apvts.getParameter(ids[k])->convertTo0to1(silt::presets[index].v[k]);
    setAll(v), setIdentity(index, {});
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
}

const String SiltProcessor::getProgramName(int index) { return isPositiveAndBelow(index, silt::numPresets) ? String::fromUTF8(silt::presets[index].name) : String(); }

File SiltProcessor::presetFolder() const { return presetDirectory != File() ? presetDirectory : File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("daveydidit/silt/Presets"); }

String SiltProcessor::presetName() const
{
    const ScopedLock sl(lock);
    return preset >= 0 ? String::fromUTF8(silt::presets[preset].name) : userName.isEmpty() ? String("custom") : userName;
}

void SiltProcessor::stepPreset(int delta)
{
    auto files = presetFolder().findChildFiles(File::findFiles, false, "*.xml");
    files.sort();
    const auto current = presetName();
    const int count = silt::numPresets + files.size();
    int at = preset;
    for (int k = 0; k < files.size() && at < 0; ++k)
        at = files[k].getFileNameWithoutExtension() == current ? silt::numPresets + k : at;
    at = (jmax(0, at) + delta + count) % count; // an unknown position steps from "init"
    if (at < silt::numPresets)
        return setCurrentProgram(at);
    if (const auto xml = parseXML(files[at - silt::numPresets]); xml != nullptr && xml->hasTagName("SILT"))
        applyParams(*xml), setIdentity(-1, files[at - silt::numPresets].getFileNameWithoutExtension());
}

File SiltProcessor::saveUserPreset()
{
    const auto folder = presetFolder();
    folder.createDirectory();
    File file;
    for (int k = 1; k < 1000 && (file == File() || file.exists()); ++k)
        file = folder.getChildFile("user " + String(k).paddedLeft('0', 2) + ".xml");
    auto state = apvts.copyState();
    state.setProperty("stateVersion", stateVersion, nullptr);
    const auto xml = state.createXml();
    if (file.exists() || xml == nullptr || !xml->writeTo(file))
        return {};
    setIdentity(-1, file.getFileNameWithoutExtension());
    return file;
}

void SiltProcessor::selectAb(int slot)
{
    std::vector<float> to;
    int toPreset = 0;
    String toName;
    {
        const ScopedLock sl(lock);
        const int from = ab;
        if (slot == from || !isPositiveAndBelow(slot, 2))
            return;
        slots[from].resize(silt::numParams);
        for (int k = 0; k < silt::numParams; ++k)
            slots[from][(size_t) k] = apvts.getParameter(ids[k])->getValue();
        slotPreset[from] = preset, slotName[from] = userName;
        to = slots[slot], toPreset = slotPreset[slot], toName = slotName[slot], ab = slot;
    }
    if (!to.empty()) // never visited: it starts as a copy of the other slot
        setAll(to.data()), setIdentity(toPreset, toName);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SiltProcessor(); }

void SiltProcessor::randomise(int64 seed)
{
    // Store the sound being left in the other slot: A/B is a one-click undo even on the first randomise.
    float current[silt::numParams];
    for (int i = 0; i < silt::numParams; ++i) current[i] = apvts.getParameter(ids[i])->getValue();
    selectAb(1 - abSlot());
    setAll(current);
    Random r(seed);
    const auto between = [&r](float lo, float hi) { return lo + r.nextFloat() * (hi - lo); };
    const auto put = [this](int i, float x) { auto* q = apvts.getParameter(ids[i]); q->beginChangeGesture(); q->setValueNotifyingHost(q->convertTo0to1(x)); q->endChangeGesture(); };
    put(Size, std::exp(between(std::log(35.0f), std::log(900.0f))));
    put(GridMode, 0), put(Density, between(2, 6)), put(Position, between(40, 900)), put(Spray, between(10, 500));
    put(SprayDir, r.nextInt(3)), put(Slope, between(25, 80)), put(Scan, 0);
    const float intervals[] { -12, -7, 0, 0, 0, 7, 12 };
    put(Pitch, intervals[r.nextInt(7)]), put(Fine, between(-8, 8)), put(PitchRand, between(0, 0.4f)), put(Snap, 0);
    put(PitchMode, 1), put(Reverse, between(0, 65)), put(Window, r.nextInt(silt::numWindows)), put(Skew, between(25, 70));
    put(Trigger, 0), put(Sync, 0), put(Rate, between(3, 12)), put(Division, 7), put(Jitter, between(20, 80));
    put(Source, 0), put(Width, between(40, 100)), put(Pan, between(15, 70)), put(Freeze, 0);
    const float fb = between(5, 40);
    put(Feedback, fb), put(Tone, between(-60, 10)), put(VerbSize, between(30, 80)), put(VerbMix, between(10, 50 - fb * 0.5f));
    setIdentity(-1, "random");
}
