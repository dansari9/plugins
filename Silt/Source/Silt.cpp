#include "Silt.h"
#include "SiltReverb.h"
#include <juce_dsp/juce_dsp.h>
#include <sstream>

using namespace juce;

namespace
{
// FROZEN: the 0.1.0 contract (state v1, version hint 1), then 0.2's appended IDs (version hint 2). Only append.
// SILT_REVERB: reverbOn/reverb remain inert if the removable reverb is deleted.
const char* const ids[] { "mix", "output", "size", "sizeSync", "density", "position", "scatter", "pitch",
    "pitchMode", "trigger", "reverbOn", "reverb", "oversample", "freeze", "motion", "texture", "bloom", "chaos",
    "scan", "lock", "heads", "harmony", "shape", "tilt", "reverse", "detune", "scale", "drop", "repeat", "band",
    "centre", "spectra", "space", "width", "feedback", "filter", "sprayDir", "onset", "pitchSnap" };
enum { Mix, Output, Size, SizeSync, Density, Position, Scatter, Pitch, PitchMode, Trigger, ReverbOn, ReverbAmount,
       Oversample, Freeze, Motion, Texture, Bloom, Chaos,
       Scan, Lock, Heads, Harmony, Shape, Tilt, Reverse, Detune, Scale, Drop, Repeat, Band, Centre, Spectra, Space,
       Width, Feedback, Filter, SprayDir, OnsetMode, PitchSnap };
static_assert(std::size(ids) == silt::numParams);
constexpr double pi = MathConstants<double>::pi;
struct Travel
{
    double lo, hi, p;
    bool log;
    double from(double n) const { const double t = std::pow(jlimit(0.0, 1.0, n), p); return log ? lo * std::pow(hi / lo, t) : lo + (hi - lo) * t; }
    double to(double v) const { v = jlimit(lo, hi, v); return std::pow(log ? std::log(v / lo) / std::log(hi / lo) : (v - lo) / (hi - lo), 1 / p); }
};
constexpr Travel sizeTravel { 0.5, 4000, 0.762, true };
int pick(double travel, int count) { return jlimit(0, count - 1, roundToInt(travel * (count - 1))); }
int hint(int i) { return i < silt::numParamsV1 ? 1 : i < silt::numParamsV2 ? 2 : 3; } // the ParameterID version: 0.1.0's, 0.2's, 0.4.2's
bool whole(double st) { return std::abs(st - std::round(st)) < .005; } // a pitch on a semitone
int syncIndex(double n) { return pick(std::pow(n, 1.6), silt::numNotes); }
float overlapOf(float density) { return 1 + 15 * (density - 1) / 63; }
double densityFromOverlap(double overlap) { return 1 + (overlap - 1) * 63 / 15; }
String msText(double ms) { return ms >= 1000 ? String(ms / 1000, 1) + " s" : String(ms, ms < 10 ? 2 : ms < 100 ? 1 : 0) + " ms"; }
AudioProcessorValueTreeState::ParameterLayout layout(const SiltProcessor& self)
{
    using Text = std::function<String(float)>;
    // Every text carries its own unit, so no host label: VST3 hosts append it ("1/16 ms", "12/beat /s").
    // Typed text goes through parse(); text it can't read keeps the current value.
    auto num = [&](int i, const char* name, float lo, float hi, float def, float step, std::optional<Travel> travel, Text text) {
        const auto snap = [per = std::round(1 / step)](float l, float h, float v) { return jlimit(l, h, std::round(v * per) / per + 0.0f); };
        NormalisableRange<float> r(lo, hi, [=](float l, float h, float v) { return snap(l, h, i == Pitch ? 48 * std::copysign(std::pow(std::abs(2*v-1),1.56f),2*v-1) : travel ? (float) travel->from(v) : l + (h - l) * v); },
            [=](float l, float h, float v) { return i == Pitch ? .5f + .5f * std::copysign(std::pow(std::abs(v/48),1/1.56f),v) : travel ? (float) travel->to(v) : (v - l) / (h - l); }, snap);
        r.interval = travel ? 0.0f : step;
        return std::make_unique<AudioParameterFloat>(ParameterID { ids[i], hint(i) }, name, r, def,
            AudioParameterFloatAttributes().withStringFromValueFunction([text](float v, int) { return text(v); }).withValueFromStringFunction([&self, i](const String& t) { return self.parse(ids[i], t).value_or(self.apvts.getRawParameterValue(ids[i])->load()); }));
    };
    auto choice = [](int i, const char* name, StringArray items, int def) { return std::make_unique<AudioParameterChoice>(ParameterID { ids[i], hint(i) }, name, items, def); };
    auto toggle = [](int i, const char* name, bool def) { return std::make_unique<AudioParameterBool>(ParameterID { ids[i], 1 }, name, def); };
    const auto percent = [](float v) { return String(v, std::abs(v - std::round(v)) < .05f ? 0 : 1) + "%"; };
    const auto bipolar = [](float v) { return (v > 0 ? "+" : "") + String(v, std::abs(v - std::round(v)) < .05f ? 0 : 1); };
    const auto sizeText = [&self](float v) { return self.apvts.getRawParameterValue("sizeSync")->load() < .5f ? String(silt::noteNames[syncIndex(sizeTravel.to(v))]) : msText(v); };
    const auto sync = [&self] { return self.apvts.getRawParameterValue("sizeSync")->load() < .5f; };
    const auto perQuarter = [&self] { return jmax(1, roundToInt(self.apvts.getRawParameterValue("density")->load() / 2)); };
    return { num(Mix,"Mix",0,100,100,.1f,{},percent),
        num(Output,"Output",-24,12,0,.1f,{},[](float v) { return String(v,1) + " dB"; }),
        num(Size,"Size",.5f,4000,51.78f,.01f,sizeTravel,sizeText),
        choice(SizeSync,"Size Mode",{"sync","free"},0),
        // In sync the engine plays whole grains per quarter and a position quantised to them: say so.
        // Overlap lock reads density as x1-x16 grains per size: say so.
        num(Density,"Density",1,64,24,.1f,Travel {1,64,1.26,false},[sync,&self](float v) {
            if (roundToInt(self.apvts.getRawParameterValue("lock")->load()) == 1)
            {
                const float overlap = overlapOf(v);
                return String::fromUTF8("×") + String(overlap, std::abs(overlap - std::round(overlap)) < .05f ? 0 : 1);
            }
            return sync() ? String(jmax(1,roundToInt(v/2)))+"/beat" : String(v,std::abs(v-std::round(v))<.05f?0:1)+"/s"; }),
        num(Position,"Position",0,4000,1200,.1f,{},[sync,perQuarter](float v) { const int q=perQuarter(); return sync() ? String(std::round(v*q/125)*.25/q,2)+" beats" : msText(v); }),
        num(Scatter,"Scatter",0,100,45,.1f,{},percent),
        // Pitch snap semi: the pitch plays and reads as its nearest semitone.
        num(Pitch,"Pitch",-48,48,12,.01f,{},[&self](float v) { if (self.apvts.getRawParameterValue("pitchSnap")->load() < .5f) v = std::round(v);
                                                                return (v>0?"+":"")+String(v,std::abs(v-std::round(v))<.005f?0:2)+" st"; }),
        choice(PitchMode,"Pitch Mode",{"shift","tape"},0),
        choice(Trigger,"Trigger",{"clock","transient","sidechain"},0),
        toggle(ReverbOn,"Reverb On",true),
        num(ReverbAmount,"Reverb",0,100,30,.1f,{},percent),
        choice(Oversample,"Oversample",{"off","2x","4x","8x"},1),
        toggle(Freeze,"Freeze",false),
        num(Motion,"Motion",0,100,0,.1f,{},percent),num(Texture,"Texture",0,100,0,.1f,{},percent),
        num(Bloom,"Bloom",0,100,0,.1f,{},percent),num(Chaos,"Chaos",0,100,0,.1f,{},percent),
        // 0.2: every default is 0.1.0's sound.
        num(Scan,"Scan",-200,200,100,.1f,{},percent),
        choice(Lock,"Lock",{"free","overlap","tone"},0),
        choice(Heads,"Heads",{"1","2","3","4"},0),
        choice(Harmony,"Harmony",{"unison","fifth","octave","major","minor","held"},0),
        num(Shape,"Shape",0,100,0,.1f,{},percent),
        num(Tilt,"Tilt",-100,100,0,.1f,{},bipolar),
        num(Reverse,"Reverse",0,100,0,.1f,{},percent),
        num(Detune,"Detune",0,24,0,.01f,{},[](float v) { return String(v,std::abs(v-std::round(v))<.005f?0:2)+" st"; }),
        choice(Scale,"Scale",{"free","chromatic","major","minor","pentatonic"},0),
        num(Drop,"Drop",0,100,0,.1f,{},percent),num(Repeat,"Repeat",0,100,0,.1f,{},percent),
        num(Band,"Band",0,100,0,.1f,{},percent),
        num(Centre,"Centre",100,12000,1200,.1f,Travel {100,12000,1,true},[](float v) { return v>=1000 ? String(v/1000,2)+" kHz" : String(v,0)+" Hz"; }),
        num(Spectra,"Spectra",0,100,0,.1f,{},percent),
        choice(Space,"Space",{"pan","binaural"},0),
        num(Width,"Width",0,200,100,.1f,{},percent),num(Feedback,"Feedback",0,95,0,.1f,{},percent),
        num(Filter,"Filter",-100,100,0,.1f,{},[bipolar](float v) { return std::abs(v)<.05f ? String("off") : bipolar(v); }),
        choice(SprayDir,"Spray",{"both","back","ahead"},0),
        choice(OnsetMode,"Onset",{"ramp","full"},0),
        choice(PitchSnap,"Pitch Snap",{"semi","free"},0) };
}

std::optional<double> number(const String& t) // the whole text is one finite number (C locale); else nothing
{
    std::istringstream in(t.toStdString());
    in.imbue(std::locale::classic());
    double v = 0;
    return in >> v && std::isfinite(v) && (in >> std::ws).eof() ? std::optional(jlimit(-1.0e6, 1.0e6, v)) : std::nullopt;
}

// Typed values (PLAN 4.1): a number with an optional unit, a note value, a note name or a word; anything else is
// nothing, so it never sets a value. Case and spaces are free, "−" and a decimal comma (with no point) are read, and a
// bare number is the shown unit. Results clamp to the range.
} // namespace
std::optional<float> SiltProcessor::parse(const String& id, const String& typed) const
{
    auto* q = dynamic_cast<AudioParameterFloat*>(apvts.getParameter(id)); // continuous parameters only
    if (q == nullptr) return std::nullopt;
    const int i = (int) (std::find_if(std::begin(ids), std::end(ids), [&](const char* s) { return id == s; }) - std::begin(ids));
    auto t = typed.toLowerCase().replace(String::fromUTF8("−"), "-").replace(String::fromUTF8("×"), "x").removeCharacters(" \t");
    if (!t.containsChar('.')) t = t.replaceCharacter(',', '.');
    const auto word = t.initialSectionContainingOnly("abcdefghijklmnopqrstuvwxyz#"), rest = t.substring(word.length());
    const auto digits = rest.initialSectionContainingOnly("+-.0123456789"), unit = rest.substring(digits.length());
    const auto v = number(digits);
    const bool sync = apvts.getRawParameterValue("sizeSync")->load() < .5f;
    const double beat = 60000.0 / feed.tempo.load(), beats = sync ? 500 : beat; // ms; sync shows 500 ms beats
    const auto in = [&](std::initializer_list<std::pair<const char*, double>> units) -> std::optional<double> {
        if (v && word.isEmpty()) for (auto [u, k] : units) if (unit == u) return *v * k;
        return std::nullopt;
    };
    // A note name (c3, f#2, bb4) as semitones above c0. An empty word is not a name: indexOf("") is 0.
    const int letter = word.isNotEmpty() && word.length() <= 2 ? String("c d ef g a b").indexOf(word.substring(0, 1)) : -1;
    const int accidental = word.length() < 2 ? 0 : word[1] == '#' ? 1 : word[1] == 'b' ? -1 : 9;
    const auto name = letter >= 0 && accidental != 9 && v && unit.isEmpty() && !digits.containsChar('.') ? std::optional(12 * *v + letter + accidental) : std::nullopt;
    const auto prefixed = [&](const char* minus, const char* plus) -> std::optional<double> {
        return (word == minus || word == plus) && v && unit.isEmpty() ? std::optional((word == minus ? -1 : 1) * std::abs(*v)) : in({ { "", 1 } });
    };
    std::optional<double> out;
    switch (i)
    {
        case Output: out = t == "-inf" || t == "-infdb" || t == "min" ? -24.0 : in({ { "", 1 }, { "db", 1 } }); break;
        case Pitch: case Detune:
            out = i == Pitch && name ? *name - 36 : in({ { "", 1 }, { "st", 1 }, { "semi", 1 }, { "semis", 1 }, { "c", .01 }, { "cent", .01 }, { "cents", .01 }, { "oct", 12 } }); // c3 = 0
            if (out && i == Pitch && apvts.getRawParameterValue("pitchSnap")->load() < .5f) out = std::round(*out);
            break;
        case Centre: out = name ? 440 * std::pow(2.0, (*name - 57) / 12) : in({ { "", 1 }, { "hz", 1 }, { "k", 1000 }, { "khz", 1000 } }); break; // a4 = 440 Hz
        case Scan: out = t == "stop" ? 0.0 : t == "normal" ? 100.0 : t == "rev" ? -100.0 : in({ { "", 1 }, { "%", 1 }, { "x", 100 } }); break;
        case Filter: out = t == "off" ? 0.0 : prefixed("lp", "hp"); break;
        case Tilt: out = prefixed("pluck", "swell"); break;
        case Density:
            if (v && ((word == "x" && unit.isEmpty()) || (word.isEmpty() && (unit == "x"
                  || (unit.isEmpty() && roundToInt(apvts.getRawParameterValue("lock")->load()) == 1)))))
                out = densityFromOverlap(*v);
            else out = in({ { "", sync ? 2 : 1 }, { "/s", 1 }, { "hz", 1 }, { "/beat", 2 }, { "/b", 2 } });
            break; // grains/s; a beat holds two
        case Position: out = in({ { "", sync ? 500 : 1 }, { "ms", 1 }, { "s", 1000 }, { "b", beats }, { "beat", beats }, { "beats", beats }, { "bar", 4 * beats }, { "bars", 4 * beats } }); break;
        case Size:
        {
            // A note value in whole notes (1/8, 1/8t, 1/4d, 1/4., 1 bar, 2 bars); sync takes the nearest one, also to a time.
            const auto den = unit.substring(1).initialSectionContainingOnly("0123456789"), dot = unit.substring(1 + den.length());
            const auto bars = in({ { "bar", 1 }, { "bars", 1 } });
            const auto whole = bars ? bars
                : v && word.isEmpty() && unit.startsWithChar('/') && den.getIntValue() > 0 && (dot.isEmpty() || dot == "t" || dot == "d" || dot == ".")
                    ? std::optional(*v / den.getIntValue() * (dot == "t" ? 2 / 3.0 : dot.isEmpty() ? 1 : 1.5)) : std::nullopt;
            const auto ms = in({ { "", 1 }, { "ms", 1 }, { "s", 1000 } });
            if (!sync) out = whole ? std::optional(*whole * 4 * beat) : ms;
            else if (const double w = whole ? *whole : ms ? *ms / 4 / beat : 0; w > 0)
            {
                int k = 0;
                for (int j = 1; j < silt::numNotes; ++j)
                    if (std::abs(std::log(w * silt::noteDen[j] / silt::noteNum[j])) < std::abs(std::log(w * silt::noteDen[k] / silt::noteNum[k]))) k = j;
                out = sizeTravel.from(std::pow((double) k / (silt::numNotes - 1), 1 / 1.6));
            }
            break;
        }
        default: out = in({ { "", 1 }, { "%", 1 } }); // percentages: 0.45 is 0.45%
    }
    return out && std::isfinite(*out) ? std::optional((float) jlimit((double) q->range.start, (double) q->range.end, *out)) : std::nullopt;
}
namespace
{
double release(double p, double hold, double k) { return p < hold ? 1 : std::pow(std::max(0.0, 1 - (p - hold) / (1 - hold)), k); } // gulp's, normalised

// ================================ The capture buffer ================================
// The input ring plus four halfband-filtered mipmaps. Each grain selects a band-limited level with
// three semitones of headroom before stepping down, preserving small pitch-ups' bandwidth.
// Level k's sample j sits at 2^k j - delay[k].
// Four-point Hermite reads share stereo coordinates; only the nonlinear feedback needs oversampling.
constexpr int levels = 5;
constexpr double scale[levels] { 1, .5, .25, .125, .0625 };
constexpr int delay[levels] { 0, 63, 189, 441, 945 };
// 127-tap Kaiser halfband: steep enough to reject the first fold above Nyquist, not just distant tones.
constexpr float halfband[32] { 0.3179876426f, -0.1051353898f, 0.06205975135f, -0.0432535297f, 0.03255484816f, -0.0255600393f, 0.02057837779f, -0.0168229528f, 0.01387852327f, -0.011505438f, 0.009555592982f, -0.007932243802f, 0.006569126778f, -0.005418887495f, 0.004446332002f, -0.003624329037f, 0.002931242885f, -0.002349286908f, 0.001863449945f, -0.001460788898f, 0.001129960166f, -0.0008609088363f, 0.0006446625028f, -0.0004731940527f, 0.0003393290235f, -0.0002366806736f, 0.0001596010718f, -0.0001031401866f, 6.300762406e-05f, -3.553363926e-05f, 1.7627528e-05f, -6.732614478e-06f };
constexpr int margin = 192; // minimum read distance; higher mips add their FIR delay and the lookahead span

// The one cubic both the scalar and the batched readers use. Block-size equivalence needs bit-identical arithmetic,
// so CMakeLists.txt also builds with -ffp-contract=off: compilers must not fuse the two copies into FMAs differently.
inline __attribute__((always_inline)) float cubic(float a, float b, float c, float e, float f)
{
    return b + f * (0.5f * (c - a) + f * (a - 2.5f * b + 2 * c - 0.5f * e + f * (0.5f * (e - a) + 1.5f * (b - c))));
}
inline float hermite(const std::vector<float>& d, size_t mask, int64 i, float f)
{
    return cubic(d[(size_t) (i - 1) & mask], d[(size_t) i & mask], d[(size_t) (i + 1) & mask], d[(size_t) (i + 2) & mask], f);
}

struct Capture
{
    std::vector<float> data[levels][2];
    size_t mask[levels] {};
    int64 head = 0;        // frames written
    float gain = 1;        // the write crossfade: 1 live, 0 frozen (writing stops)
    float peak = 0;        // the editor's waveform: the peak of the slice being written
    int count = 0, slice = 1, startup = 240;
    double history = 384000;
    std::array<std::atomic<float>, silt::Feed::bins>* wave = nullptr;
    bool quiet = false; // no audio flowing: the telemetry holds still
    std::atomic<int>* at = nullptr;

    void prepare(size_t size, int sliceLength, double frames)
    {
        for (int k = 0; k < levels; ++k)
            for (auto& c : data[k]) c.assign(size >> k, 0.0f);
        for (int k = 0; k < levels; ++k) mask[k] = (size >> k) - 1;
        slice = jmax(1, sliceLength), history = frames;
        startup = jmax(1, roundToInt(frames / silt::maxBackSeconds * .005));
    }
    void reset()
    {
        head = 0, gain = 1, peak = 0, count = 0;
    }
    // One frame, mixed into what is there by the write gain, then each mipmap sample it completes.
    void write(float l, float r)
    {
        // History before prepare is silence: join an already-playing input to it without a recorded step.
        const float start = head < startup ? (float) head / (float) startup : 1.0f;
        l *= start, r *= start;
        auto& d = data[0];
        const size_t i = (size_t) head & mask[0];
        d[0][i] += gain * (l - d[0][i]), d[1][i] += gain * (r - d[1][i]);
        for (int k = 1, j = (int) (head & 0x7fffffff); k < levels && j % 2 == 0; ++k, j /= 2)
            for (int c = 0; c < 2; ++c)
            {
                const auto& x = data[k - 1][c];
                const auto at_ = [&](int o) { return x[(size_t) (j - o) & mask[k - 1]]; };
                float sample = .5f * at_(63);
                for (int tap = 0; tap < 32; ++tap)
                {
                    const int offset = 2 * tap + 1;
                    sample += halfband[tap] * (at_(63 - offset) + at_(63 + offset));
                }
                data[k][c][(size_t) (j / 2) & mask[k]] = sample;
            }
        ++head;
        peak = jmax(peak, std::abs(d[0][i]), std::abs(d[1][i]));
        if (++count >= slice)
        {
            if (!quiet) // silence pauses the editor's waveform, as it pauses the cloud
            {
                const int k = at->load(std::memory_order_relaxed);
                (*wave)[(size_t) k].store(peak, std::memory_order_relaxed), at->store((k + 1) % silt::Feed::bins, std::memory_order_relaxed);
            }
            peak = 0, count = 0;
        }
    }
    // Both channels at fractional capture time t from the grain's band-limited level.
    void readAt(double t, int k, float& l, float& r) const
    {
        const double j = (t + delay[k]) * scale[k];
        const double whole = std::floor(j);
        const auto i = (int64) whole; const float fraction = (float) (j - whole);
        l = hermite(data[k][0], mask[k], i, fraction), r = hermite(data[k][1], mask[k], i, fraction);
    }
    static constexpr int batch = 128;
    static int readMargin(int k) { return jmax(margin, delay[k] + 4 * (1 << k) + batch); }
    void read(double t, int k, float& l, float& r, double atHead) const
    {
        // Very long synced / reverse / frozen grains loop within recent history. A short splice avoids a step
        // when a reader crosses either edge; ordinary reads away from the edges are unchanged.
        constexpr double fade = 128;
        const double lo = atHead - history + readMargin(k), hi = atHead - readMargin(k), period = hi - lo - fade;
        if (t < lo + fade || t >= hi) t -= period * std::floor((t - lo - fade) / period);
        readAt(t, k, l, r);
        if (t > hi - fade)
        {
            float l2, r2;
            readAt(t - period, k, l2, r2);
            const float mix = (float) ((t - hi + fade) / fade);
            l += mix * (l2 - l), r += mix * (r2 - r);
        }
    }
    void readBatch(const double* positions, int k, float* l, float* r, int length,
                   bool frozen, float freezeStep) const
    {
        alignas(16) float a[2][batch], b[2][batch], c[2][batch], e[2][batch], fraction[batch];
        bool safe = true;
        for (int i = 0; i < length; ++i)
            safe = safe && positions[i] >= head - history + readMargin(k) + 128 + batch && positions[i] <= head - readMargin(k) - 128;
        if (!safe)
        {
            float futureGain = gain;
            double futureHead = (double) head;
            for (int i = 0; i < length; ++i)
            {
                read(positions[i], k, l[i], r[i], futureHead);
                if (frozen) futureGain = jmax(0.0f, futureGain - freezeStep);
                if (!frozen || futureGain > 0) ++futureHead;
            }
            return;
        }
        float* out[] { l, r };
        for (int i = 0; i < length; ++i)
        {
            const double t = (positions[i] + delay[k]) * scale[k];
            auto index = (int64) t;
            if ((double) index > t) --index; // exact floor, including negative capture times
            fraction[i] = (float) (t - (double) index);
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto& d = data[k][ch]; const auto m = mask[k];
                a[ch][i] = d[(size_t) (index - 1) & m], b[ch][i] = d[(size_t) index & m];
                c[ch][i] = d[(size_t) (index + 1) & m], e[ch][i] = d[(size_t) (index + 2) & m];
            }
        }
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < length; ++i) out[ch][i] = cubic(a[ch][i], b[ch][i], c[ch][i], e[ch][i], fraction[i]);
    }
};

// ================================ Grains ================================
struct Grain
{
    int wait = 0, left = 0, length = 0;    // samples until it sounds, samples it still sounds (0 = a free voice)
    double phase = 0, inc = 0;  // through the window
    double pos = 0, step = 1;  // speed: the read position and its step (+-r); pitch: the anchor, moving +-1
    float tap = 0, tapStep = 0, span = 0; // pitch: the first tap's distance behind the anchor, its step, the tap window
    float gl = 0, gr = 0, pitch = 0;
    float peak = 0.5f, attack = 1, decay = 1;
    float morph = 0, edge = 2; // shape: 0 the texture's table, 1 the flat-top table, 2 a rectangle with 1 ms edges
    float sharp = 1;           // tilt: the power the window's long side is raised to (1 with the peak centred)
    unsigned tag = 0;
    int shape = 0, fade = 0;   // fade: a stolen grain's linear release length (samples), 0 = not stolen
    int level = 0;
    bool shifted = false;      // pitch mode: two crossfaded taps keep the timeline while the pitch moves
    int bus = -1;              // binaural: the left of its two azimuth buses (gl, gr: their gains); -1 pans
    float a1 = 0, a2 = 0, a3 = 0, k = 0, sv[4] {}; // colour: a TPT band-pass (k = 0: none), L and R states
    float bandPass(float x, float& s0, float& s1) const // unity peak (times the band's makeup in k)
    {
        const float v3 = x - s1, v1 = a1 * s0 + a2 * v3, v2 = s1 + a2 * s0 + a3 * v3;
        s0 = 2 * v1 - s0, s1 = 2 * v2 - s1;
        return k * v1;
    }
};

// Transients: a peak follower that jumps 6 dB over its own slow (30 ms) copy above a -50 dB gate; re-armed once the
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

// Binaural: one azimuth of seven (-90..+90 degrees). The far ear hears it quieter, up to 0.65 ms later and darker.
struct Azimuth
{
    float ring[256] {}, near = 1, far = 1, delay = 0, coef = 1;
    int at = 0;
    bool left = false;
    OnePole shade;
    void prepare(int b, double fs)
    {
        const double a = std::abs(std::sin((b - 3) / 3.0 * pi / 2));
        *this = {}, near = (float) std::sqrt(1 + a), far = (float) std::sqrt(1 - 0.8 * a), left = b < 3;
        delay = (float) jmin(254.0, 0.00065 * fs * a), coef = coefficient(12000 - 9500 * a, fs);
    }
    void step(float x, float* y)
    {
        if (delay <= 0) { y[0] += x, y[1] += x; return; } // straight ahead
        ring[at] = x;
        const int i = (int) delay;
        const float f = delay - (float) i, d0 = ring[(at - i) & 255], d1 = ring[(at - i - 1) & 255];
        at = (at + 1) & 255;
        y[left ? 0 : 1] += near * x, y[left ? 1 : 0] += far * shade.low(d0 + f * (d1 - d0), coef);
    }
};
} // namespace

const char* const silt::noteNames[] = { "1/64", "1/32T", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2T", "1/2", "1/2D", "1 bar", "2 bars" };

double silt::window(int shape, double x, double skew)
{
    const double k = 0.02 + 0.96 * skew, p = x < k ? 1 - x / k : (x - k) / (1 - k);
    switch (shape)
    {
        case 0: return 0.5 + 0.5 * std::cos(pi * p); // hann
        case 1: return release(p, 0.16, 0.8);       // dome
        default: return release(p, 0, 1.2);        // lean
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
    double fs = 48000;
    Capture capture;            // main input history; sidechain only triggers
    // Locked clocks steal: the oldest grain moves to one of 16 tail voices and releases over 2 ms, so the new one is on time.
    static constexpr int tail = 16;
    std::array<Grain, silt::maxGrains + tail> grains {};
    int voiceCount = 0, stolen = 0;
    std::array<float, 1025> env[silt::numWindows] {}, taps {}; // immutable windows; sin^2 pitch taps
    Onset onsets[2];
    float fastRelease = 0, slowAttack = 0, slowRelease = 0, freezeStep = 0, ccGlide = 0;
    int holdoff = 0;
    // The clock: grains (stream phase) since the last tick; sync while playing: the last whole grain on the host's grid.
    double phase = 0, lastTick = -1;
    bool wasPlaying = false;
    int stream = 0;              // the lane (head) of the last grain; one head alternates two lanes, 0.1.0's L/R streams
    double gap = 1;              // samples between clock grains
    bool wasFrozen = false;
    double scanned = 0;          // the scan's extra distance back (samples)
    double lastStart = 0;        // the previous grain's capture position, for repeat
    uint32 seed = 1, nextGrain = 0;
    // MIDI: held notes (last-note priority) and the fixed controller map's smoothed values, -1 until received.
    std::array<int, 128> held {};
    std::array<float, 128> velocities {};
    int heldCount = 0;
    float velocity = 1;
    float cc[4] {}, ccTarget[4] {};
    int lastNote = 60;
    // Held-chord heads: each held note keeps its head until released, so a middle note-off renumbers nothing.
    std::array<int, 4> slotNote {}, slotOrder {};
    int noteOrder = 0;
    // All quality paths are prepared once; changing quality never allocates on the audio thread.
    std::array<std::unique_ptr<dsp::Oversampling<float>>, 4> oversamplers;
    int quality = -1, previousQuality = 0, qualityFade = 0;
    float meters[3] {};
    float fbRing[2][K] {};
    int64 now = 0;               // samples since reset
    double overlap = 0;          // smoothed sum of the windows sounding
    float overlapAttack = 0, overlapRelease = 0;
    OnePole lows[2], highs[2], dc[2], outputDc[2];
    std::vector<float> diffusion[4];
    size_t diffusionAt[4] {};
    float level = 0, inputLevel = 0, levelGlide = 0, inputAttack = 0, inputRelease = 0, meterDecay = 0;
    bool quietTelemetry = false; // no audio in or out this block: the editor's telemetry holds still
    float lastOutputPeak = 0;
    float lowpass = 0, highpass = 0, dcCoef = 0, outputDcCoef = 0; // the fixed feedback band and DC blockers
    SmoothedValue<float> mix, gain, verb, width, feedback, power;
    std::array<Azimuth, 7> buses;    // binaural; they run while any grain is routed to them (`spatial` grains)
    int spatial = 0;
    OnePole tone[2][2];          // the one-knob filter: two one-poles per channel, low-pass (< 0) or high-pass (> 0)
    float toneCoef = 0, toneMix = 0;
    int toneMode = 0;
    SiltReverb reverb; // SILT_REVERB

    void prepare(double sr, silt::Feed& feed)
    {
        fs = sr > 0 && sr < 1.0e7 ? sr : 48000.0;
        const auto size = (size_t) nextPowerOfTwo((int) ((silt::maxBackSeconds + 2.5) * fs));
        capture.wave = &feed.wave, capture.at = &feed.at;
        capture.prepare(size, (int) (silt::maxBackSeconds * fs / silt::Feed::bins), silt::maxBackSeconds * fs);
        for (int k = 0; k < 4; ++k) diffusion[k].assign((size_t) (fs * (k < 2 ? 0.0047 : 0.0071)) + 1, 0.0f);
        for (size_t q = 0; q < oversamplers.size(); ++q)
        {
            oversamplers[q] = std::make_unique<dsp::Oversampling<float>>(2, q, dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
            oversamplers[q]->initProcessing(K);
        }
        reverb.prepare(fs); // SILT_REVERB
        fastRelease = (float) std::exp(-1 / (0.015 * fs)), slowAttack = (float) (1 - std::exp(-1 / (0.03 * fs)));
        slowRelease = (float) (1 - std::exp(-1 / (0.3 * fs))), holdoff = (int) (0.05 * fs);
        freezeStep = (float) (1 / (0.02 * fs)), ccGlide = (float) (1 - std::exp(-1 / (0.03 * fs)));
        overlapAttack = (float) (1 - std::exp(-1 / (0.002 * fs))), overlapRelease = (float) (1 - std::exp(-1 / (0.08 * fs)));
        levelGlide = (float) (1 - std::exp(-1 / (0.1 * fs)));
        inputAttack = (float) (1 - std::exp(-1 / (0.01 * fs))), inputRelease = (float) (1 - std::exp(-1 / (2.0 * fs)));
        meterDecay = std::pow(0.9995f, (float) (48000 / fs)); // the same 42 ms fall at every rate
        lowpass = coefficient(4000, fs), highpass = coefficient(30, fs), dcCoef = coefficient(20, fs), outputDcCoef = coefficient(8, fs);
        for (auto* s : { &mix, &gain, &verb, &width, &feedback, &power }) s->reset(fs, 0.02);
        for (size_t j = 0; j < taps.size(); ++j)
        {
            taps[j] = (float) std::pow(std::sin(pi * (double) j / 1024), 2);
            for (int shape = 0; shape < silt::numWindows; ++shape) env[shape][j] = (float) silt::window(shape, (double) j / 1024, 0.5);
        }
    }
    void reset(silt::Feed& feed)
    {
        capture.reset();
        voiceCount = 0, grains = {}, onsets[0] = onsets[1] = {}, phase = 0, lastTick = -1, wasPlaying = false, stream = 1, scanned = 0, lastStart = 0, seed = 0x5117u;
        nextGrain = 0, stolen = 0, wasFrozen = false, gap = 1;
        slotNote.fill(-1);
        heldCount = 0, velocity = 1, now = 0, overlap = 0, level = inputLevel = 0, lastNote = 60;
        quality = -1, qualityFade = 0;
        std::fill(std::begin(meters), std::end(meters), 0.0f);
        feed.note.store(60), feed.time.store(0), feed.events.store(0);
        std::fill(std::begin(cc), std::end(cc), -1.0f), std::fill(std::begin(ccTarget), std::end(ccTarget), -1.0f);
        for (auto& r : fbRing) std::fill(std::begin(r), std::end(r), 0.0f);
        std::fill(std::begin(diffusionAt), std::end(diffusionAt), (size_t) 0);
        lows[0] = lows[1] = highs[0] = highs[1] = dc[0] = dc[1] = outputDc[0] = outputDc[1] = {};
        for (int b = 0; b < 7; ++b) buses[(size_t) b].prepare(b, fs);
        spatial = 0, toneMode = 0, toneMix = 0, tone[0][0] = tone[0][1] = tone[1][0] = tone[1][1] = {};
        for (auto& w : feed.wave) w.store(0, std::memory_order_relaxed);
        feed.at.store(0, std::memory_order_relaxed);
        feed.fired.store(0, std::memory_order_relaxed);
        feed.lastFire.store(-1, std::memory_order_relaxed), feed.lastPitch.store(0, std::memory_order_relaxed), feed.lastLength.store(0, std::memory_order_relaxed);
        read();
        width.setCurrentAndTargetValue(0.8f * (v[Width] / 100));
        feedback.setCurrentAndTargetValue(loop());
        power.setCurrentAndTargetValue((float) (1 - 0.5 * jlimit(0.0, 1.0, separation() + (pitched() ? 1 : 0))));
        mix.setCurrentAndTargetValue(v[Mix] / 100), gain.setCurrentAndTargetValue(Decibels::decibelsToGain(v[Output])), verb.setCurrentAndTargetValue(0);
    }
    void read()
    {
        for (size_t i = 0; i < raw.size(); ++i) v[i] = raw[i]->load(std::memory_order_relaxed);
        if (v[PitchSnap] < .5f) v[Pitch] = std::round(v[Pitch]); // semi: whole semitones, whatever automation sends
    }
    float envelope(const Grain& g) const
    {
        // Shape narrows the window as it goes from the texture's curve to flat-top to square: it fills the whole grain at
        // 0 %, three quarters at 50 % and half at 100 %, so the cloud thins into a gated chop.
        const float duty = 1.0f - 0.25f * g.morph, at = (float) g.phase / duty;
        if (at >= 1.0f)
            return 0.0f;
        const bool rising = at < g.peak;
        const float x = rising ? at * g.attack : 0.5f + (at - g.peak) * g.decay;
        float w = lookup(env[g.shape], x);
        if (g.morph > 0) // shape 0: 0.1.0's windows
        {
            const float flat = lookup(env[1], x);
            if (g.morph <= 1)
                w = w + g.morph * (flat - w);
            else
            {
                const float rect = jmin(1.0f, at * g.edge, (1 - at) * g.edge);
                w = flat + (g.morph - 1) * (rect - flat);
            }
        }
        // Tilt sharpens the long side as well as moving the peak: a real pluck (fast fall) or swell (late rise).
        if (!exactlyEqual(g.sharp, 1.0f) && rising == (g.peak > 0.5f))
            w = std::pow(w, g.sharp);
        return w;
    }
    // The nearest note of a scale (chromatic, major, minor, pentatonic) over the source's pitch.
    static double snap(double st, int mode)
    {
        constexpr int masks[] { 0xfff, 0xab5, 0x5ad, 0x295 };
        double best = 1e9;
        for (int n = (int) std::floor(st) - 6, end = n + 14; n < end; ++n)
            if ((masks[mode - 1] >> ((n % 12 + 12) % 12) & 1) != 0 && std::abs(n - st) < std::abs(best - st)) best = n;
        return best;
    }

    float random() { seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5; return (float) (seed >> 8) * 0x1p-24f; } // 0..1
    float macro(int i) const { return jmin(1.0f, v[i] / 100 + control(i - Motion)); }
    float control(int k) const { return jmax(0.0f, cc[k]); } // a controller's value, 0 until received
    static float lookup(const std::array<float, 1025>& t, float x)
    {
        const float at = jlimit(0.0f, 1024.0f, x * 1024);
        const int j = jmin(1023, (int) at);
        return t[(size_t) j] + (at - (float) j) * (t[(size_t) j + 1] - t[(size_t) j]);
    }

    // Captured audio is not saved: after a reset (a reopened project, a new buffer size) freeze refills 4 s first.
    float loop() const { return jmin(0.95f, 0.12f + 0.48f * macro(Bloom) + v[Feedback] / 100); } // the feedback amount
    bool holding() const { return v[Freeze] > 0.5f && capture.head >= (int64) (4 * fs); }
    double separation() const { return v[Scatter] / 25 + 4 * macro(Chaos) + macro(Motion); }
    bool pitched() const { return std::abs(v[Pitch]) > .005f || heldCount > 0 || v[Freeze] > .5f || (v[Heads] > .5f && v[Harmony] > .5f); }
    double note(int k, double bpm) const { return 240.0 * silt::noteNum[k] / silt::noteDen[k] / bpm * fs; }
    double grainSize(double bpm) const { return v[SizeSync] < .5f ? note(syncIndex(sizeTravel.to(v[Size])), bpm) : v[Size] * fs / 1000; }
    // Tone lock: one grain per period of the last held note, else of c3 transposed by pitch; 20 Hz - 2 kHz.
    double toneGap() const { return fs / jlimit(20.0, 2000.0, 261.6255653 * std::exp2((heldCount > 0 ? held[(size_t) heldCount - 1] - 60 : v[Pitch]) / 12.0)); }
    double period(double bpm) const
    {
        return v[SizeSync] < .5f ? fs * 60 / bpm / jmax(1, roundToInt(v[Density] / 2)) : fs / v[Density];
    }
    double position(double bpm) const
    {
        const double back = v[Position] * fs / 1000;
        // The position travel is referenced to 120 BPM, then expressed as a ratio of the grain clock.
        const double reference = period(120) * .25;
        return v[SizeSync] < .5f ? std::round(back / reference) * (period(bpm) * .25) : back;
    }

    // Starts one grain after `wait` samples, `early` samples into its life (back-dated). Grains are dealt round-robin to
    // the heads' lanes: each lane has its own pan, a small size difference, a harmony interval and a position stagger.
    void spawn(int wait, double bpm, bool frozen, double early = 0)
    {
        const int heads = roundToInt(v[Heads]) + 1, lanes = jmax(2, heads), harmony = roundToInt(v[Harmony]);
        int side = stream = (stream + 1) % lanes;
        for (int k = 0; harmony == 5 && k < lanes && slotNote[(size_t) (side % heads)] < 0; ++k) side = stream = (stream + 1) % lanes;
        const int head = side % heads, heldNote = harmony == 5 ? slotNote[(size_t) head] : -1;
        if (v[Drop] > 0 && random() < v[Drop] / 100) return; // a silent beat: the clock keeps its time
        if (voiceCount - stolen == silt::maxGrains) // all voices busy: free lock skips (0.1.0); locked clocks steal the oldest
        {
            if (v[Lock] < .5f || voiceCount == silt::maxGrains + tail) return;
            Grain* oldest = nullptr;
            for (int k = 0; k < voiceCount; ++k)
                if (auto& o = grains[(size_t) k]; o.fade == 0 && (oldest == nullptr || nextGrain - o.tag > nextGrain - oldest->tag)) oldest = &o;
            oldest->fade = oldest->left = jmin(oldest->left, jmax(1, roundToInt(0.002 * fs))), ++stolen;
            if (oldest->wait > 0) oldest->length = oldest->left; // not started: trace() still sees its first sample
        }
        auto* g = &grains[(size_t) voiceCount++];
        g->tag = ++nextGrain, g->fade = 0;
        const double stereo = 0.8 + 0.2 * macro(Bloom);
        double size = grainSize(bpm);
        size *= (1 + (2 * random() - 1) * 0.65 * macro(Texture)) * (side == 1 ? 1 + 0.04 * stereo : 1.0);
        if (v[Lock] > 1.5f) size = jmin(size, 120 * gap); // tone: the window shrinks to fit the voices
        size = jlimit(0.0005 * fs, v[SizeSync] > .5f ? 4 * fs : jmax(4 * fs, note(silt::numNotes - 1, bpm)), size);
        // Randomisation precedes chromatic snapping, then detune spreads and scale snaps. Zero spread preserves fractional pitch.
        double st = v[Pitch] + (2 * random() - 1) * 24 * macro(Chaos);
        if (macro(Chaos) > 0) st = std::round(st);
        if (v[Detune] > 0) st += (2 * random() - 1) * v[Detune];
        if (v[Scale] > .5f && (v[Detune] > 0 || macro(Chaos) > 0)) st = snap(st, roundToInt(v[Scale]));
        constexpr int intervals[5][4] { { 0, 0, 0, 0 }, { 0, 7, 12, 19 }, { 0, 12, -12, 24 }, { 0, 4, 7, 12 }, { 0, 3, 7, 12 } };
        st += harmony == 5 ? (heldNote >= 0 ? heldNote - 60 : 0) : (heldCount > 0 ? held[(size_t) heldCount - 1] - 60 : 0) + intervals[harmony][head];
        st = jlimit(-48.0, 48.0, st);
        const double r = std::exp2(st / 12);
        const int mip = r <= 1.19 ? 0 : jmin(levels - 1, (int) std::ceil(std::log2(r / 1.19)));
        const int readMargin = Capture::readMargin(mip);
        const bool reverse = random() < 0.65f * macro(Chaos) || (v[Reverse] > 0 && random() < v[Reverse] / 100), shifted = v[PitchMode] < 0.5f;
        const double dir = reverse ? -1 : 1;
        // Keep the tap modulation below 25 Hz even at +48 st. Grain length stays independent of pitch;
        // a tiny grain can still read through a longer, smooth pitch crossfade.
        // The read: how long it sounds and how much source it covers.
        const double dur = jmin(2.0e9, shifted ? size : size / r), span = shifted ? .04 * fs * jmax(1.0, r - 1) : 0;
        early = jmin(early, dur - 1); // a burst spaced over the plain size never back-dates past a shorter grain's end
        // How far back it starts: position (+ the scan) + spray, kept inside what the buffer holds for the whole grain
        // (live, the head moves on while it plays; frozen, it stands still). a: room needed ahead, b: behind.
        const double maxBack = silt::maxBackSeconds * fs;
        double back = position(bpm);
        const double spray = (v[Scatter] * 20 + 600 * macro(Chaos)) * fs / 1000;
        double u = random();
        u = std::pow(u, 1 + 3 * (1 - macro(Texture))); // slope: small distances more likely
        const int sprayDir = roundToInt(v[SprayDir]); // both, back (older only), ahead (newer only)
        back += spray * (sprayDir == 1 || (sprayDir == 0 && (macro(Motion) > .5f || random() < .5f)) ? u : -u) + scanned;
        back += head * 0.25 * spray; // heads stagger behind the shared anchor
        // Repeat re-reads the previous grain's audio; the clamps below still keep it inside the buffer.
        if (v[Repeat] > 0 && g->tag > 1 && random() < v[Repeat] / 100) back = (double) capture.head + (frozen ? 0 : wait) - lastStart;
        const double extent = shifted ? size : dur * r;
        double a = 0, b = 0;
        if (frozen) a = extent + (shifted && reverse ? span : 0), b = shifted && !reverse ? span : 0;
        else if (shifted) a = reverse ? size + span : 0, b = reverse ? size : span;
        else if (reverse) a = extent, b = dur;
        else a = jmax(0.0, dur * (r - 1)), b = jmax(0.0, dur * (1 - r));
        // Fit the whole segment when possible; longer notes retain their duration and use the capture's splice.
        back = a + b < maxBack - 2 * readMargin ? jlimit(readMargin + a, maxBack - readMargin - b, back) : jlimit((double) readMargin, maxBack - readMargin, back);
        const double start = (double) capture.head + (frozen ? 0 : wait) - back - (frozen ? 0 : early); // frozen, the head stood still
        lastStart = start;
        g->wait = wait, g->length = jmax(2, (int) dur), g->left = g->length, g->phase = 0, g->inc = 1.0 / g->left;
        g->shape = macro(Texture) < .33f ? 0 : macro(Texture) < .66f ? 1 : 2;
        g->peak = jlimit(0.02f, 0.98f, 0.5f - 0.3f * macro(Texture) + 0.0048f * v[Tilt]); // tilt: -100 pluck .. +100 swell
        g->morph = v[Shape] / 50, g->edge = (float) jmax(2.0, g->length / (0.001 * fs));
        g->attack = 0.5f / g->peak, g->decay = 0.5f / (1 - g->peak);
        g->sharp = 1.0f + 0.02f * std::abs(v[Tilt]); // the tilt control alone: x3 at +-100 %, 1 (as before) at 0
        g->shifted = shifted && std::abs(r - 1) > 1e-8, g->pitch = (float) st;
        g->step = shifted ? dir : dir * r;
        g->pos = shifted ? (reverse ? start + size : start) : (reverse ? start + dur * r : start);
        g->span = (float) jmax(2.0, 2 * std::round(span / 2)), g->tap = 0, g->tapStep = (float) (1 - r);
        // The lower mip of a crossfade still aliases for fractional pitch ratios. A single band-limited
        // level rejects that fold and halves the read work; Hermite preserves smooth sub-sample motion.
        g->level = mip;
        if (early > 0) // back-dated: it began `early` samples ago at the same distance behind the input
        {
            g->phase = early * g->inc, g->pos += early * g->step, g->tap = (float) (early * g->tapStep);
            g->tap -= g->span * std::floor(g->tap / g->span);
            g->left = g->length = jmax(1, g->left - (int) early);
        }
        // Pan: the stream's side of the width plus a random spread; equal power, unity in the middle.
        const double pan = jlimit(-1.0, 1.0, (side == 0 ? -0.6 : lanes == 2 ? 0.6 : 1.2 * side / (lanes - 1) - 0.6) * stereo + (2 * random() - 1) * jmin(1.0, 0.25 + 0.7 * macro(Motion)));
        const double angle = (pan + 1) * pi / 4;
        const float amp = heldNote >= 0 ? velocities[(size_t) heldNote] : velocity;
        g->gl = (float) (amp * std::cos(angle) * std::sqrt(2.0)), g->gr = (float) (amp * std::sin(angle) * std::sqrt(2.0));
        g->bus = -1;
        if (v[Space] > .5f) // binaural: equal power between the two nearest of seven azimuths
        {
            const double azimuth = (pan + 1) * 3;
            g->bus = jmin(5, (int) azimuth), ++spatial;
            g->gl = (float) (amp * std::cos((azimuth - g->bus) * pi / 2)), g->gr = (float) (amp * std::sin((azimuth - g->bus) * pi / 2));
        }
        // Colour: a band-pass from wide to one critical band (ERB) at the centre, or spectra's random one (+-4 oct).
        g->k = 0;
        if (v[Band] > 0 || v[Spectra] > 0)
        {
            const double f = jlimit(20.0, 0.45 * fs, v[Centre] * (v[Spectra] > 0 ? std::exp2(v[Spectra] / 100 * (8 * random() - 4)) : 1.0));
            const double q = 0.1 * std::pow(f / (24.7 + 0.108 * f) / 0.1, v[Band] / 100), w = std::tan(pi * f / fs), a1 = 1 / (1 + w * (w + 1 / q));
            g->a1 = (float) a1, g->a2 = (float) (w * a1), g->a3 = (float) (w * w * a1), g->k = (float) ((1 + v[Band] / 100) / q);
            std::fill_n(g->sv, 4, 0.0f);
        }
    }
    // A note-on's grain; onset full back-dates a whole size of the head's grains, so the cloud is there at once.
    // In held mode the burst goes to the note's head (slot), whose grains arrive every `occupied` clock grains.
    void fill(double bpm, bool frozen, int slot)
    {
        const int heads = roundToInt(v[Heads]) + 1, lanes = jmax(2, heads);
        const int occupied = slot < 0 ? 1 : (int) std::count_if(slotNote.begin(), slotNote.begin() + heads, [](int n) { return n >= 0; });
        const double size = v[Lock] > 1.5f ? jmin(grainSize(bpm), 120 * gap) : grainSize(bpm); // as spawn() fits tone
        const int count = v[OnsetMode] > .5f ? jlimit(1, 64, (int) std::ceil(size / (gap * occupied) - 1e-9)) : 1;
        for (int k = 0; k < count; ++k)
        {
            if (slot >= 0) stream = (slot + lanes - 1) % lanes;
            spawn(0, bpm, frozen, k * size / count);
        }
    }
    void midi(const MidiMessage& m, double bpm, bool frozen)
    {
        if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            heldCount = 0, velocity = 1, slotNote.fill(-1);
            if (v[Lock] > 1.5f) gap = toneGap(); // tone lock's period follows the notes from their event on
        }
        else if (m.isNoteOn())
        {
            const auto end = std::remove(held.begin(), held.begin() + heldCount, m.getNoteNumber());
            heldCount = (int) (end - held.begin());
            held[(size_t) heldCount++] = m.getNoteNumber();
            lastNote = m.getNoteNumber();
            velocities[(size_t) m.getNoteNumber()] = velocity = m.getFloatVelocity();
            const int heads = roundToInt(v[Heads]) + 1;
            int slot = -1;
            for (int k = 0; k < heads; ++k) slot = slotNote[(size_t) k] == lastNote ? k : slot; // its own head,
            for (int k = 0; k < heads; ++k) slot = slot < 0 && slotNote[(size_t) k] < 0 ? k : slot; // else a free one,
            if (slot < 0) // else the oldest note's
            {
                slot = 0;
                for (int k = 1; k < heads; ++k) slot = slotOrder[(size_t) k] < slotOrder[(size_t) slot] ? k : slot;
            }
            slotNote[(size_t) slot] = lastNote, slotOrder[(size_t) slot] = ++noteOrder;
            if (v[Lock] > 1.5f) gap = toneGap(); // before the note's own burst
            fill(bpm, frozen, v[Harmony] > 4.5f ? slot : -1);
        }
        else if (m.isNoteOff())
        {
            const auto end = std::remove(held.begin(), held.begin() + heldCount, m.getNoteNumber());
            heldCount = (int) (end - held.begin());
            velocity = heldCount > 0 ? velocities[(size_t) held[(size_t) heldCount - 1]] : 1;
            std::replace(slotNote.begin(), slotNote.end(), m.getNoteNumber(), -1);
            if (v[Lock] > 1.5f) gap = toneGap();
        }
        else if (m.isController())
        {
            // Fixed smoothed macro offsets, never writes automation: CC1/2/3/4 motion/texture/bloom/chaos.
            if (const int k = m.getControllerNumber() - 1; isPositiveAndBelow(k, 4))
                cc[k] = jmax(0.0f, cc[k]), ccTarget[k] = (float) m.getControllerValue() / 127;
            if (m.getControllerNumber() == 121)
                std::fill(std::begin(cc), std::end(cc), -1.0f), std::fill(std::begin(ccTarget), std::end(ccTarget), -1.0f);
        }
    }

    void trace(Grain& g, silt::Feed& feed, int64 frame, int64 head)
    {
        if (g.left == g.length) // its first sample
        {
            feed.fired.fetch_add(1, std::memory_order_relaxed);
            feed.lastFire.store(frame, std::memory_order_relaxed);
            feed.lastPitch.store(g.pitch, std::memory_order_relaxed);
            feed.lastLength.store(g.left, std::memory_order_relaxed);
            // One event per grain at its start: the cloud colours a grain by its age since it began. Grains of silence
            // (no audio flowing in or out) leave no trace, so the cloud only moves with sound.
            if (quietTelemetry)
                return;
            const auto serial = feed.events.load(std::memory_order_relaxed);
            auto& e = feed.history[(size_t) serial % silt::Feed::historySize];
            // A seqlock per slot prevents a GUI frame combining data from two ring generations.
            e.sequence.fetch_add(1, std::memory_order_acq_rel);
            e.grain.store(g.tag, std::memory_order_relaxed);
            e.frame.store(frame, std::memory_order_relaxed);
            e.captureHead.store(head, std::memory_order_relaxed);
            e.back.store((float) ((head - g.pos) / fs), std::memory_order_relaxed);
            e.pitch.store(g.pitch, std::memory_order_relaxed);
            e.size.store((float) (g.length / fs), std::memory_order_relaxed);
            e.sequence.fetch_add(1, std::memory_order_release);
            feed.events.store(serial + 1, std::memory_order_release);
        }
    }
    // No event or voice completion lies inside this span. Read each voice in time order, then accumulate
    // contiguous samples in the original voice order. Reads stay at least `margin` behind the write head.
    // Rows 0-1 are the panned sum, 2-8 the binaural buses (filled while any grain is routed to them).
    void cloud(int length, bool frozen, silt::Feed& feed, float out[9][Capture::batch], float* weights)
    {
        for (int c = 0; c < (spatial > 0 ? 9 : 2); ++c) std::fill_n(out[c], length, 0.0f);
        std::fill_n(weights, length, 0.0f);
        for (int k = 0; k < voiceCount; ++k)
        {
            auto& g = grains[(size_t) k];
            if (g.wait > 0) { g.wait -= length; continue; }
            trace(g, feed, now, capture.head); // a grain can only start at a span's first sample
            double positions[Capture::batch], positions2[Capture::batch];
            alignas(16) float w[Capture::batch], a[Capture::batch], audio[2][Capture::batch], other[2][Capture::batch];
            for (int j = 0; j < length; ++j)
            {
                w[j] = envelope(g);
                positions[j] = g.pos;
                if (g.shifted)
                {
                    const float second = g.tap + 0.5f * g.span >= g.span ? g.tap - 0.5f * g.span : g.tap + 0.5f * g.span;
                    a[j] = lookup(taps, g.tap / g.span);
                    positions[j] -= g.step * g.tap, positions2[j] = g.pos - g.step * second;
                    g.tap += g.tapStep;
                    if (g.tap < 0 || g.tap >= g.span) g.tap -= g.span * std::floor(g.tap / g.span);
                }
                g.phase += g.inc, g.pos += g.step, --g.left;
            }
            capture.readBatch(positions, g.level, audio[0], audio[1], length, frozen, freezeStep);
            if (g.shifted)
            {
                capture.readBatch(positions2, g.level, other[0], other[1], length, frozen, freezeStep);
                for (int c = 0; c < 2; ++c)
                    for (int j = 0; j < length; ++j) audio[c][j] = a[j] * audio[c][j] + (1 - a[j]) * other[c][j];
            }
            if (g.k > 0) // the states stay in registers: the output arrays may alias them
            {
                float s[4] { g.sv[0], g.sv[1], g.sv[2], g.sv[3] };
                for (int j = 0; j < length; ++j) audio[0][j] = g.bandPass(audio[0][j], s[0], s[1]), audio[1][j] = g.bandPass(audio[1][j], s[2], s[3]);
                std::copy_n(s, 4, g.sv);
            }
            if (g.bus >= 0)
                for (int j = 0; j < length; ++j)
                {
                    const float m = w[j] * 0.5f * (audio[0][j] + audio[1][j]);
                    out[2 + g.bus][j] += g.gl * m, out[3 + g.bus][j] += g.gr * m;
                    weights[j] += w[j];
                }
            else
                for (int j = 0; j < length; ++j)
                {
                    out[0][j] += w[j] * g.gl * audio[0][j], out[1][j] += w[j] * g.gr * audio[1][j];
                    weights[j] += w[j];
                }
        }
    }

    void process(SiltProcessor& p, AudioBuffer<float>& buffer, MidiBuffer& midiBuffer)
    {
        read();
        auto& feed = p.feed;
        const int n = buffer.getNumSamples(), ins = jmin(2, p.getMainBusNumInputChannels()), outs = jmin(2, p.getMainBusNumOutputChannels());
        const auto sc = p.getBusBuffer(buffer, true, 1);
        const int scs = jmin(2, sc.getNumChannels());
        float inputPeak = 0;
        for (int c = 0; c < ins; ++c)
            inputPeak = jmax(inputPeak, buffer.getMagnitude(c, 0, n));
        for (int c = 0; c < scs; ++c)
            inputPeak = jmax(inputPeak, sc.getMagnitude(c, 0, n));
        quietTelemetry = capture.quiet = !(inputPeak > 1.0e-5f) && !(lastOutputPeak > 1.0e-5f);
        // The host's tempo (120 without one) and, while it plays, its position for the synced clock.
        double bpm = 120, ppq = 0;
        bool playing = false;
        if (auto* head = p.getPlayHead())
            if (const auto pos = head->getPosition())
            {
                if (const auto b = pos->getBpm(); b && std::isfinite(*b) && *b > 0) bpm = jlimit(20.0, 999.0, *b);
                if (const auto q = pos->getPpqPosition(); q && std::isfinite(*q) && pos->getIsPlaying()) playing = true, ppq = *q;
            }
        feed.tempo = (float) bpm;
        const bool synced = v[SizeSync] < 0.5f && playing && v[Lock] < .5f; // locked clocks run free
        const double quartersPerGrain = 1.0 / jmax(1, roundToInt(v[Density] / 2));
        if (synced)
        {
            const double target = ppq / quartersPerGrain;
            if (!wasPlaying || std::abs(target - phase) > 1e-5) lastTick = std::ceil(target - 1e-9) - 1;
            phase = target;
        }
        else if (wasPlaying) phase -= std::floor(phase);
        wasPlaying = synced;
        const int trigger = roundToInt(v[Trigger]);
        const float threshold = Decibels::decibelsToGain(-50.0f);
        mix.setTargetValue(v[Mix] / 100), gain.setTargetValue(Decibels::decibelsToGain(v[Output]));
        // Similar grains add in amplitude; scattered/pitched grains add mostly in power. Interpolate the overlap
        // law using their source separation, preserving level without analysing or limiting the output.
        const int selectedQuality = roundToInt(v[Oversample]);
        if (quality != selectedQuality)
        {
            previousQuality = jmax(0, quality);
            qualityFade = quality < 0 ? 0 : 128;
            quality = selectedQuality;
            oversamplers[(size_t) quality]->reset();
        }
        // The clock: free reads density; overlap reads it as x1-x16 grains per size (see overlapOf); tone, one grain per period.
        const int lock = roundToInt(v[Lock]);
        gap = lock == 0 ? period(bpm) : lock == 1 ? grainSize(bpm) / overlapOf(v[Density]) : toneGap();
        verb.setTargetValue(v[ReverbOn] > .5f ? v[ReverbAmount] / 100 : 0.0f);
        const double scanRange = jmax(1.0, silt::maxBackSeconds * fs - v[Position] * fs / 1000);
        const double anchor = position(bpm), ride = 0.02 * fs, wrap = silt::maxBackSeconds * fs - 2 * ride;
        // The one-knob filter on the cloud (and so its feedback): 0 bypasses; -100 low-passes to 80 Hz, +100 high-passes to 8 kHz.
        const int filterMode = v[Filter] < 0 ? -1 : v[Filter] > 0 ? 1 : 0;
        const float toneTarget = coefficient(filterMode < 0 ? 20000 * std::pow(0.004, -v[Filter] / 100.0) : 20 * std::pow(400.0, v[Filter] / 100.0), fs);
        auto event = midiBuffer.cbegin();
        float cloudFrames[9][Capture::batch] {}, cloudWeights[Capture::batch] {};
        int cloudAt = 0, cloudLength = 0;
        for (int chunk = 0; chunk < n;)
        {
            const int count = jmin(K - (int) (now % K), n - chunk);
            const int64 chunkTime = now;
            float frames[4][K] {}, old[4][K] {}, dryFrames[2][K] {}, sideFrames[K] {}, bloomFrames[K] {};
            for (int i = 0; i < count; ++i)
            {
                const int t = chunk + i;
                bool frozen = holding();
                for (; event != midiBuffer.cend() && (*event).samplePosition <= t; ++event)
                    if ((*event).numBytes <= 3) midi((*event).getMessage(), bpm, frozen); // ignore SysEx without allocating
                for (int k = 0; k < 4; ++k)
                    if (ccTarget[k] >= 0) cc[k] += ccGlide * (ccTarget[k] - cc[k]);
                frozen = holding();
                if (wasFrozen && !frozen && v[OnsetMode] > .5f) fill(bpm, frozen, -1); // onset full: fresh audio at once
                wasFrozen = frozen;
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
                capture.gain = jlimit(0.0f, 1.0f, capture.gain + (frozen ? -freezeStep : freezeStep));
                if (capture.gain > 0) capture.write(x[0] + fbRing[0][now % K], x[1] + fbRing[1][now % K]);
                // The anchor's extra lag per sample: scan 100% (with motion's slow drift) is 0.1.0's; 0% holds a moment,
                // negative runs back through history, over 100% catches up with the input.
                const double rate = 1 - v[Scan] / 100.0 + 0.25 * macro(Motion);
                if (exactlyEqual(v[Scan], 100.0f) && rate < 1e-9) scanned = 0;
                else
                {
                    scanned += rate;
                    if (scanned >= scanRange) scanned -= scanRange; // past the oldest audio: back to position, fresh
                    if (!exactlyEqual(v[Scan], 100.0f)) // live, it rides 20 ms behind the input; frozen, it wraps in the held audio
                        scanned = frozen ? scanned - wrap * std::floor((anchor + scanned - ride) / wrap) : jmax(scanned, ride - anchor);
                }
                if (trigger == 0)
                {
                    bool tick = false;
                    if (synced)
                    {
                        phase = (ppq + t * bpm / 60 / fs) / quartersPerGrain;
                        const double whole = std::floor(phase + 1e-9);
                        tick = !exactlyEqual(whole, lastTick), lastTick = whole;
                        phase += 1 / gap;
                    }
                    else if ((phase += 1 / gap) >= 1 - 1e-10)
                        tick = true, phase -= std::floor(phase + 1e-10);
                    // Locked clocks start each grain at its exact fractional time (tone stays in tune).
                    if (tick) spawn((int) (random() * (0.4f * macro(Motion) + 0.5f * macro(Chaos)) * gap), bpm, frozen, lock > 0 ? phase * gap : 0.0);
                }
                if ((hitIn && trigger == 1) || (hitSide && trigger == 2)) spawn(0, bpm, frozen);
                feedback.setTargetValue(loop());
                width.setTargetValue((0.8f + 0.2f * macro(Bloom)) * (v[Width] / 100)); // width: 100% is 0.1.0's
                float y[2] {}, weight = 0, bus[7] {};
                const bool binaural = spatial > 0;
                if (cloudAt == cloudLength)
                {
                    cloudAt = cloudLength = 0;
                    if (trigger == 0 && stolen == 0) // stolen grains release on this scalar path
                    {
                        // Samples until the next tick; the synced clock's phase is already the next sample's.
                        int span = jmin(n - t, (int) jlimit(1.0, (double) Capture::batch, std::floor((synced ? lastTick + 1 - phase : 1 - (phase - std::floor(phase))) * gap - 1e-6)));
                        if (event != midiBuffer.cend()) span = jmin(span, jmax(1, (*event).samplePosition - t));
                        for (int k = 0; k < voiceCount; ++k)
                        {
                            const auto& g = grains[(size_t) k];
                            span = jmin(span, g.wait > 0 ? g.wait : g.left - 1);
                        }
                        if (span >= 4) cloudLength = span, cloud(span, frozen, feed, cloudFrames, cloudWeights);
                    }
                }
                if (cloudAt < cloudLength)
                {
                    for (int b = 0; binaural && b < 7; ++b) bus[b] = cloudFrames[2 + b][cloudAt];
                    y[0] = cloudFrames[0][cloudAt], y[1] = cloudFrames[1][cloudAt], weight = cloudWeights[cloudAt++];
                }
                else
                {
                    for (int k = 0; k < voiceCount; ++k)
                    {
                        auto& g = grains[(size_t) k];
                        if (g.wait > 0) { --g.wait; continue; }
                        trace(g, feed, now, capture.head);
                        const float w = envelope(g) * (g.fade > 0 ? (float) g.left / (float) g.fade : 1.0f);
                        g.phase += g.inc, --g.left;
                        float l, r;
                        if (!g.shifted) capture.read(g.pos, g.level, l, r, capture.head), g.pos += g.step;
                        else
                        {
                            float l2, r2;
                            const float second = g.tap + 0.5f * g.span >= g.span ? g.tap - 0.5f * g.span : g.tap + 0.5f * g.span;
                            const float a = lookup(taps, g.tap / g.span), b = 1 - a;
                            capture.read(g.pos - g.step * g.tap, g.level, l, r, capture.head), capture.read(g.pos - g.step * second, g.level, l2, r2, capture.head);
                            l = a * l + b * l2, r = a * r + b * r2;
                            g.pos += g.step, g.tap += g.tapStep;
                            if (g.tap < 0 || g.tap >= g.span) g.tap -= g.span * std::floor(g.tap / g.span);
                        }
                        if (g.k > 0) l = g.bandPass(l, g.sv[0], g.sv[1]), r = g.bandPass(r, g.sv[2], g.sv[3]);
                        if (g.bus < 0) y[0] += w * g.gl * l, y[1] += w * g.gr * r;
                        else
                        {
                            const float m = w * 0.5f * (l + r); // binaural grains are mono points
                            bus[g.bus] += g.gl * m, bus[g.bus + 1] += g.gr * m;
                        }
                        weight += w;
                        if (g.left == 0) stolen -= g.fade > 0 ? 1 : 0, spatial -= g.bus >= 0 ? 1 : 0, g = grains[(size_t) --voiceCount], --k;
                    }
                }
                for (int b = 0; binaural && b < 7; ++b) buses[(size_t) b].step(bus[b], y);
                // Actual window overlap, with a slow release between clouds. No compressor or limiter.
                overlap += (weight > overlap ? overlapAttack : overlapRelease) * (weight - overlap);
                power.setTargetValue((float) (1 - 0.5 * jlimit(0.0, 1.0, separation() + (pitched() ? 1 : 0))));
                const float exponent = power.getNextValue();
                const float base = jmax(1.0f, weight, (float) overlap);
                const float norm = 0.85f / (exponent <= .500001f ? std::sqrt(base) : exponent >= .999999f ? base : std::pow(base, exponent));
                const float mid = 0.5f * (y[0] + y[1]) * norm, sd = 0.5f * (y[0] - y[1]) * norm * width.getNextValue();
                y[0] = mid + sd, y[1] = mid - sd;
                if (filterMode != 0 || toneMix > 0) // it fades in and out over 20 ms; bypassed at rest
                {
                    const int mode = toneMix > 0 && toneMode != filterMode ? 0 : filterMode; // a flip fades the old side out first
                    if (mode != 0 && toneMix <= 0) // low-pass starts settled on the signal, high-pass at rest
                        for (int c = 0; c < 2; ++c) toneCoef = toneTarget, tone[c][0].s = tone[c][1].s = mode < 0 ? y[c] : 0.0f;
                    if (mode != 0) toneMode = mode, toneCoef += ccGlide * (toneTarget - toneCoef);
                    toneMix = jlimit(0.0f, 1.0f, toneMix + (mode != 0 ? freezeStep : -freezeStep));
                    for (int c = 0; c < 2; ++c)
                    {
                        const float low = tone[c][0].low(y[c], toneCoef);
                        y[c] += toneMix * ((toneMode < 0 ? tone[c][1].low(low, toneCoef) : y[c] - low - tone[c][1].low(y[c] - low, toneCoef)) - y[c]);
                    }
                }
                const float fbAmount = feedback.getNextValue();
                float f[2];
                for (int c = 0; c < 2; ++c)
                {
                    y[c] -= dc[c].low(y[c], dcCoef);
                    // A fixed 30 Hz - 4 kHz band; bloom raises diffusion and feedback, never the cutoffs.
                    const float low = lows[c].low(y[c], lowpass), high = low - highs[c].low(low, highpass);
                    const float diffused = diffuse(diffuse(high, (size_t) c), (size_t) c + 2);
                    f[c] = high + (0.2f + 0.8f * macro(Bloom)) * (diffused - high);
                }
                level += levelGlide * (fbAmount * 0.5f * (std::abs(f[0]) + std::abs(f[1])) - level);
                const float guard = level > 1e-6f ? jmin(1.0f, jmax(0.5f * inputLevel, 0.01f) / level) : 1.0f;
                frames[0][i] = y[0], frames[1][i] = y[1];
                frames[2][i] = fbAmount * guard * f[0], frames[3][i] = fbAmount * guard * f[1];
                dryFrames[0][i] = dry[0], dryFrames[1][i] = dry[1];
                sideFrames[i] = jmax(std::abs(side[0]), std::abs(side[1]));
                bloomFrames[i] = macro(Bloom);
                ++now;
            }
            // The feedback delay is K samples. Filter that quantum together, before any of it is read back.
            // This keeps the feedback timing and FIR history identical for every host block size.
            const auto filtered = [&](int q, float data[4][K], int length) {
                float* channels[] { data[2], data[3] };
                dsp::AudioBlock<float> samples(channels, 2, (size_t) length);
                auto up = oversamplers[(size_t) q]->processSamplesUp(samples);
                for (size_t c = 0; c < 2; ++c)
                    for (auto* x = up.getChannelPointer(c), *end = x + up.getNumSamples(); x < end; ++x)
                    {
                        // Padé tanh on [-1,1] (below float roundoff), std::tanh beyond. Chosen per sample, never per
                        // chunk: chunks end at host block edges, and every block size must sound bit-identical.
                        const float y = 2 * *x, yy = y * y;
                        *x = std::abs(y) <= 1 ? .5f * y * (945 + yy * (105 + yy)) / (945 + yy * (420 + 15 * yy)) : .5f * std::tanh(y);
                    }
                oversamplers[(size_t) q]->processSamplesDown(samples);
            };
            const int fading = jmin(count, qualityFade);
            if (fading > 0)
            {
                for (int c = 0; c < 4; ++c) std::copy_n(frames[c], fading, old[c]);
                filtered(previousQuality, old, fading);
            }
            filtered(quality, frames, count);
            for (int i = 0; i < count; ++i)
            {
                const int t = chunk + i;
                const float blend = qualityFade > 0 ? 1.0f - (float) qualityFade / 128 : 1;
                float frame[4], y[2], dry[2] { dryFrames[0][i], dryFrames[1][i] };
                for (int c = 2; c < 4; ++c) frame[c] = i < fading ? old[c][i] + blend * (frames[c][i] - old[c][i]) : frames[c][i];
                for (int c = 0; c < 2; ++c)
                {
                    fbRing[c][(chunkTime + i) % K] = frame[c + 2];
                    y[c] = frames[c][i];
                }
                qualityFade = jmax(0, qualityFade - 1);
                // SILT_REVERB: removable grain-output reverb; keep its frozen parameters inert if removed.
                float rl = y[0], rr = y[1];
                reverb.process(rl, rr, (0.45f + 0.5f * bloomFrames[i]));
                const float vm = verb.getNextValue();
                y[0] += vm * (rl - y[0]), y[1] += vm * (rr - y[1]);
                // Remove subsonic drift left by long, frozen windows and a modulated reverb tail.
                for (int c = 0; c < 2; ++c) y[c] -= outputDc[c].low(y[c], outputDcCoef);
                const float m = mix.getNextValue(), out = gain.getNextValue();
                if (outs == 1) y[0] = 0.5f * (y[0] + y[1]);
                for (int c = 0; c < outs; ++c)
                    buffer.setSample(c, t, exactlyEqual(m, 0.0f) ? dry[c] : exactlyEqual(m, 1.0f) ? y[c] * out : dry[c] + m * (y[c] * out - dry[c]));
                for (int c = 0; c < 2; ++c) meters[c] = jmax(std::abs(buffer.getSample(jmin(c, outs - 1), t)), meters[c] * meterDecay);
                meters[2] = jmax(sideFrames[i], meters[2] * meterDecay);
            }
            chunk += count;
        }
        lastOutputPeak = 0;
        for (int c = 0; c < outs; ++c)
            lastOutputPeak = jmax(lastOutputPeak, buffer.getMagnitude(c, 0, n));
        publish(feed, bpm);
    }
    float diffuse(float x, size_t k)
    {
        auto& d = diffusion[k];
        const float delayed = d[diffusionAt[k]], w = x + 0.5f * delayed;
        d[diffusionAt[k]] = std::abs(w) < 1e-15f ? 0.0f : w;
        if (++diffusionAt[k] == d.size()) diffusionAt[k] = 0;
        return delayed - 0.5f * w;
    }
    void publish(silt::Feed& feed, double bpm)
    {
        size_t k = 0;
        for (int i = 0; i < voiceCount; ++i)
            if (const auto& g = grains[(size_t) i]; g.left > 0 && g.wait == 0 && k < (size_t) silt::Feed::dots)
            {
                feed.env[k++].store(jmax(0.001f, envelope(g)), std::memory_order_relaxed);
            }
        for (; k < (size_t) silt::Feed::dots; ++k) feed.env[k].store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < 3; ++i) feed.meter[i].store(meters[i], std::memory_order_relaxed);
        if (quietTelemetry) // the cloud's clock and positions hold while no audio flows
            return;
        feed.centre.store((float) ((position(bpm) + scanned) / fs), std::memory_order_relaxed);
        feed.frozen.store(capture.gain <= 0, std::memory_order_relaxed);
        feed.captureHead.store(capture.head, std::memory_order_relaxed);
        feed.time.store(now, std::memory_order_relaxed), feed.sampleRate.store((float) fs, std::memory_order_relaxed);
        feed.note.store(lastNote, std::memory_order_relaxed), feed.held.store(heldCount > 0, std::memory_order_relaxed);
        for (size_t i = 0; i < 3; ++i) feed.meter[i].store(meters[i], std::memory_order_relaxed);
    }
};

SiltProcessor::SiltProcessor(File directory)
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", AudioChannelSet::stereo(), false).withOutput("Output", AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "SILT", layout(*this)), presetDirectory(directory), engine(std::make_unique<Engine>())
{
    for (int i = 0; i < silt::numParams; ++i) engine->raw[(size_t) i] = apvts.getRawParameterValue(ids[i]);
}
SiltProcessor::~SiltProcessor() = default;
void SiltProcessor::prepareToPlay(double sr, int)
{
    engine->prepare(sr, feed), resetPending = true;
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
            if (!slots[k].empty()) // "preset;v0,..,v37", normalised, 9 significant digits: every float reads back exactly
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

void SiltProcessor::applyParams(const XmlElement& state, bool gestures)
{
    // Migration hook: v1, v2 and future versions load known IDs; missing/nonfinite values use the defaults, so a v1
    // document plays 0.1.0's sound. Add explicit migrations here before bumping stateVersion.
    float normalised[silt::numParams];
    for (int i = 0; i < silt::numParams; ++i)
    {
        auto* p = apvts.getParameter(ids[i]);
        const auto* e = state.getChildByAttribute("id", ids[i]);
        const auto v = number(e != nullptr ? e->getStringAttribute("value") : String());
        normalised[i] = v ? p->convertTo0to1(p->getNormalisableRange().snapToLegalValue((float) *v)) : p->getDefaultValue();
        if (i == PitchSnap && e == nullptr) // saved before 0.4.2: a pitch between semitones stays free, so it sounds as it did
            normalised[i] = whole(apvts.getParameter(ids[Pitch])->convertFrom0to1(normalised[Pitch])) ? 0.0f : 1.0f;
        if (!gestures) p->setValueNotifyingHost(normalised[i]); // a host restoring state: no gestures
    }
    if (gestures) setAll(normalised); // a user's choice: touch/latch automation records it like a factory preset
}

// Malformed documents leave state untouched. Future versions load known IDs and ignore new ones.
void SiltProcessor::setStateInformation(const void* data, int size)
{
    const auto tree = getXmlFromBinary(data, size);
    if (tree == nullptr || !tree->hasTagName("SILT"))
        return;
    applyParams(*tree, false);
    const ScopedLock sl(lock);
    for (int k = 0; k < 2; ++k)
    {
        const auto t = StringArray::fromTokens(tree->getStringAttribute(k == 0 ? "slotA" : "slotB"), ";,", "");
        slots[k].clear(), slotName[k] = tree->getStringAttribute(k == 0 ? "slotNameA" : "slotNameB");
        slotPreset[k] = (int) jlimit(-1.0, silt::numPresets - 1.0, number(t[0]).value_or(-1));
        for (int i = 1; i < t.size() && i <= silt::numParams; ++i)
            if (const auto v = number(t[i]); v && *v >= 0 && *v <= 1) slots[k].push_back((float) *v);
        if (slots[k].size() == silt::numParamsV1) // a v1 slot: the 0.2 parameters at their defaults
            for (int i = silt::numParamsV1; i < silt::numParamsV2; ++i) slots[k].push_back(apvts.getParameter(ids[i])->getDefaultValue());
        if (slots[k].size() == silt::numParamsV2) // before 0.4.2: pitch snap as applyParams decides it
            slots[k].push_back(whole(apvts.getParameter(ids[Pitch])->convertFrom0to1(slots[k][Pitch])) ? 0.0f : 1.0f);
        if (slots[k].size() != silt::numParams) slots[k].clear();
    }
    ab = number(tree->getStringAttribute("ab")).value_or(0) > 0.5 ? 1 : 0;
    editorWidth = (int) jlimit(0.0, 4096.0, number(tree->getStringAttribute("editorWidth")).value_or(0));
    preset = (int) jlimit(-1.0, silt::numPresets - 1.0, number(tree->getStringAttribute("preset")).value_or(-1)), userName = tree->getStringAttribute("presetName");
}

// Values in frozen ID order. Preset 4 (tide) equals the parameter defaults.
// All factory voicing is expressed by the parameters; no hidden preset values. 0.1.0's ten keep the 0.2 defaults.
const silt::Preset silt::presets[] {
    {"sub dust", {100,0,90,1,16,140,12,-12,1,0,1,12,1,0,0,25,5,0, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"grit",     {100,3,14,1,42,240,38,0,1,0,0,8,1,0,12,90,5,20, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"glass",    {100,0,85,1,28,700,30,12,0,0,1,28,1,0,5,20,15,0, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"halo",     {100,0,650,1,18,800,55,0,0,0,1,55,1,0,15,10,65,0, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"tide",     {100,0,51.78f,0,24,1200,45,12,0,0,1,30,1,0,0,0,0,0, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"choir",    {100,0,900,1,22,1600,32,0,0,0,1,48,1,0,20,15,45,5, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"tines",    {100,6,35,1,14,450,18,7,1,0,1,22,1,0,5,75,10,0, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"chime",    {100,0,160,1,12,900,25,12,0,0,1,42,1,0,10,45,45,0, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"rain",     {100,0,55,1,48,1800,78,0,1,0,1,32,1,0,35,55,25,35, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"drift",    {100,0,1200,1,15,2200,62,-7,0,0,1,45,1,0,65,20,55,15, 100,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    // 0.2's ten: one idea each from the new controls. Output trims level-match them to the ten above (loudness suite).
    // organ: tone lock over a slowly walking anchor; short grains, because overlapped wavetable reads of
    // inharmonic input partly cancel (scan 100% would read time-aligned copies: a plain delay).
    {"swarm",    {100,0,180,1,30,900,50,0,0,0,1,35,1,0,0,0,0,0, 100,0,3,0,0,0,0,3,1,0,0,0,1200,0,1,150,0,0,0,0,0}},
    {"organ",    {100,2,6,1,24,300,0,0,0,0,1,30,1,0,0,0,0,0, 10,2,0,0,30,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"cathedral",{100,0,600,1,16,1500,40,12,0,0,1,60,1,0,0,0,0,0, 100,0,2,1,0,0,0,0,0,0,0,20,1200,0,0,100,55,0,0,0,0}},
    {"stutter",  {100,0,14.05f,0,16,250,10,0,0,0,1,15,1,0,0,0,0,0, 100,0,0,0,90,0,0,0,0,30,70,0,1200,0,0,100,0,0,0,0,0}},
    {"undertow", {100,0,700,1,12,1500,40,0,0,0,1,40,1,0,0,0,0,0, 50,0,0,0,0,80,100,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"stretch",  {100,0,60,1,13.6f,100,5,0,0,0,1,15,1,0,0,0,0,0, 50,1,0,0,40,0,0,0,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
    {"prism",    {100,6,120,1,40,1200,60,0,0,0,1,35,1,0,0,0,0,0, 100,0,0,0,0,0,0,0,0,0,0,85,1200,70,1,100,0,0,0,0,0}},
    {"orbit",    {100,-1.5f,400,1,20,1400,45,0,0,0,1,35,1,0,40,0,0,0, 100,0,1,2,0,0,0,0,0,0,0,0,1200,0,1,180,0,0,0,0,0}},
    {"snapshot", {100,0,250,1,24,200,15,0,0,0,1,25,1,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,1200,0,0,100,0,0,1,1,0}},
    {"chorale",  {100,0,300,1,24,600,35,0,0,0,1,45,1,0,0,0,0,0, 100,0,3,5,0,0,0,0.3f,0,0,0,0,1200,0,0,100,0,0,0,0,0}},
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

File SiltProcessor::presetFolder() const
{
    if (presetDirectory != File()) return presetDirectory;
   #if JUCE_MAC // where macOS keeps audio plug-in presets
    return File::getSpecialLocation(File::userHomeDirectory).getChildFile("Library/Audio/Presets/daveydidit/silt");
   #else
    return File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("daveydidit/silt/Presets");
   #endif
}

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
    at = (jmax(0, at) + delta + count) % count; // a custom or random sound steps from the first preset
    if (at < silt::numPresets)
        return setCurrentProgram(at);
    loadUserPreset(files[at - silt::numPresets]);
}

void SiltProcessor::loadUserPreset(const File& file)
{
    if (const auto xml = parseXML(file); xml != nullptr && xml->hasTagName("SILT"))
        applyParams(*xml, true), setIdentity(-1, file.getFileNameWithoutExtension());
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

void SiltProcessor::selectAb(int slot, bool copy)
{
    std::vector<float> to;
    int toPreset = 0;
    String toName;
    {
        const ScopedLock sl(lock);
        const int from = ab;
        if (slot == from || !isPositiveAndBelow(slot, 2))
            return;
        const int saveTo = copy ? slot : from;
        slots[saveTo].resize(silt::numParams);
        for (int k = 0; k < silt::numParams; ++k)
            slots[saveTo][(size_t) k] = apvts.getParameter(ids[k])->getValue();
        slotPreset[saveTo] = preset, slotName[saveTo] = userName;
        if (copy) return;
        to = slots[slot], toPreset = slotPreset[slot], toName = slotName[slot], ab = slot;
    }
    if (!to.empty()) // never visited: it starts as a copy of the other slot
        setAll(to.data()), setIdentity(toPreset, toName);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SiltProcessor(); }

void SiltProcessor::nudge(int64 seed)
{
    float next[silt::numParams];
    Random random(seed);
    for (int i = 0; i < silt::numParams; ++i)
    {
        auto* param = apvts.getParameter(ids[i]);
        next[i] = param->getValue();
        if (!param->isDiscrete() && i != Mix && i != Output && (i < Motion || i > Chaos))
        {
            const float target = next[i] + .3f * (random.nextFloat() - .5f);
            const float snapped = param->convertTo0to1(param->convertFrom0to1(target));
            if (std::abs(snapped - next[i]) <= .15f) next[i] = snapped;
        }
    }
    if (auto* pitch = apvts.getParameter(ids[Pitch]); apvts.getRawParameterValue("pitchSnap")->load() < .5f)
        next[Pitch] = pitch->convertTo0to1(std::round(pitch->convertFrom0to1(next[Pitch])));
    selectAb(1 - abSlot(), true);
    setAll(next);
    setIdentity(-1, "random");
}

void SiltProcessor::randomise(int64 seed)
{
    // Store the sound being left in the other slot: A/B is a one-click undo even on the first randomise.
    selectAb(1 - abSlot(), true);
    Random r(seed);
    r.nextInt(); // a small seed's first value is always near zero
    const auto between = [&r](float lo, float hi) { return lo + r.nextFloat() * (hi - lo); };
    const auto put = [this](int i, float x) { auto* q = apvts.getParameter(ids[i]); q->beginChangeGesture(); q->setValueNotifyingHost(q->convertTo0to1(x)); q->endChangeGesture(); };
    const float density = between(8, 40); // grains overlap at least 1.5x: never a gappy, quiet cloud
    put(Size, std::exp(between(std::log(jmax(35.0f, 1500 / density)), std::log(900.0f))));
    put(SizeSync, 1), put(Density, density), put(Position, between(80, 2000)), put(Scatter, between(5, 70));
    const float intervals[] { -12, -7, 0, 0, 0, 7, 12 };
    put(Pitch, intervals[r.nextInt(7)]), put(PitchMode, r.nextInt(2)), put(Freeze, 0);
    put(ReverbOn, 1), put(ReverbAmount, between(10, 45));
    // 0.2: the free clock keeps the overlap above; each colour is applied at a moderate depth, often not at all.
    // Scan never falls below 100% (the anchor would leave a fresh capture). No narrow band: one far from the input's
    // partials is near silence, one on them rings; spectra alone only tilts (a wide band).
    const auto maybe = [&](float lo, float hi) { return r.nextBool() ? between(lo, hi) : 0.0f; };
    put(Lock, 0), put(Heads, r.nextInt(4)), put(Harmony, r.nextInt(5)), put(Scan, 100 + maybe(0, 50));
    put(Shape, between(0, 70)), put(Tilt, between(-60, 60)), put(Reverse, maybe(10, 60));
    put(Detune, maybe(.1f, 5)), put(Scale, r.nextInt(5)), put(Drop, maybe(5, 30)), put(Repeat, maybe(10, 50));
    put(Band, 0), put(Centre, std::exp(between(std::log(400.0f), std::log(2500.0f)))), put(Spectra, maybe(10, 40));
    put(Space, r.nextInt(2)), put(Width, between(60, 140)), put(Feedback, maybe(5, 20)), put(Filter, maybe(-30, 20));
    put(SprayDir, r.nextInt(3));
    setIdentity(-1, "random");
}
