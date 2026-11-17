#include "Silt.h"
#include "SiltReverb.h"
#include <optional>
#include <juce_dsp/juce_dsp.h>
#include <sstream>
#include <cstdlib>
#if JUCE_INTEL && (defined(__GNUC__) || defined(__clang__))
 #include <immintrin.h>
 #define SILT_AVX2 1
#endif

using namespace juce;

namespace
{
// FROZEN state v1: final 0.1.0 contract. Only append IDs with a newer version hint after this release.
// SILT_REVERB: reverbOn/reverb remain inert if the removable reverb is deleted.
const char* const ids[] { "mix", "output", "size", "sizeSync", "density", "position", "scatter", "pitch",
    "pitchMode", "trigger", "reverbOn", "reverb", "oversample", "freeze", "motion", "texture", "bloom", "chaos" };
enum { Mix, Output, Size, SizeSync, Density, Position, Scatter, Pitch, PitchMode, Trigger, ReverbOn, ReverbAmount,
       Oversample, Freeze, Motion, Texture, Bloom, Chaos };
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
int syncIndex(double n) { return pick(std::pow(n, 1.6), silt::numNotes); }
String msText(double ms) { return ms >= 1000 ? String(ms / 1000, 1) + " s" : String(ms, ms < 10 ? 2 : ms < 100 ? 1 : 0) + " ms"; }
AudioProcessorValueTreeState::ParameterLayout layout(const SiltProcessor& self)
{
    using Text = std::function<String(float)>;
    using Parse = std::function<float(const String&)>;
    const auto parse = [](const String& t) { return t.replace(String::fromUTF8("−"), "-").retainCharacters("+-.0123456789").getFloatValue(); };
    auto num = [&](int i, const char* name, float lo, float hi, float def, float step, std::optional<Travel> travel, const char* unit, Text text, Parse from = {}) {
        const auto snap = [per = std::round(1 / step)](float l, float h, float v) { return jlimit(l, h, std::round(v * per) / per + 0.0f); };
        NormalisableRange<float> r(lo, hi, [=](float l, float h, float v) { return snap(l, h, i == Pitch ? 48 * std::copysign(std::pow(std::abs(2*v-1),1.56f),2*v-1) : travel ? (float) travel->from(v) : l + (h - l) * v); },
            [=](float l, float h, float v) { return i == Pitch ? .5f + .5f * std::copysign(std::pow(std::abs(v/48),1/1.56f),v) : travel ? (float) travel->to(v) : (v - l) / (h - l); }, snap);
        r.interval = travel ? 0.0f : step;
        return std::make_unique<AudioParameterFloat>(ParameterID { ids[i], 1 }, name, r, def,
            AudioParameterFloatAttributes().withLabel(unit).withStringFromValueFunction([text](float v, int) { return text(v); }).withValueFromStringFunction(from ? from : Parse(parse)));
    };
    auto choice = [](int i, const char* name, StringArray items, int def) { return std::make_unique<AudioParameterChoice>(ParameterID { ids[i], 1 }, name, items, def); };
    auto toggle = [](int i, const char* name, bool def) { return std::make_unique<AudioParameterBool>(ParameterID { ids[i], 1 }, name, def); };
    const auto percent = [](float v) { return String(v, std::abs(v - std::round(v)) < .05f ? 0 : 1) + "%"; };
    const auto sizeText = [&self](float v) { return self.apvts.getRawParameterValue("sizeSync")->load() < .5f ? String(silt::noteNames[syncIndex(sizeTravel.to(v))]) : msText(v); };
    const auto sizeFrom = [&self, parse](const String& text) {
        if (self.apvts.getRawParameterValue("sizeSync")->load() < .5f)
            for (int k = 0; k < silt::numNotes; ++k)
                if (text.trim() == silt::noteNames[k]) return (float) sizeTravel.from(std::pow((double) k / (silt::numNotes - 1), 1 / 1.6));
        return parse(text) * (text.endsWithIgnoreCase(" s") ? 1000 : 1);
    };
    return { num(Mix,"Mix",0,100,100,.1f,{},"%",percent),
        num(Output,"Output",-24,12,0,.1f,{},"dB",[](float v) { return String(v,1) + " dB"; }),
        num(Size,"Size",.5f,4000,51.78f,.01f,sizeTravel,"ms",sizeText,sizeFrom),
        choice(SizeSync,"Size Mode",{"sync","free"},0),
        num(Density,"Density",1,64,24,.1f,Travel {1,64,1.26,false},"/s",[](float v) { return String(v,std::abs(v-std::round(v))<.05f?0:1)+"/s"; }),
        num(Position,"Position",0,4000,1200,.1f,{},"ms",[](float v) { return msText(v); },[parse](const String& t) { return parse(t)*(t.endsWithIgnoreCase(" s")?1000:1); }),
        num(Scatter,"Scatter",0,100,45,.1f,{},"%",percent),
        num(Pitch,"Pitch",-48,48,7,.01f,{},"st",[](float v) { return (v>0?"+":"")+String(v,std::abs(v-std::round(v))<.005f?0:2)+" st"; }),
        choice(PitchMode,"Pitch Mode",{"shift","tape"},0),
        choice(Trigger,"Trigger",{"clock","transient","sidechain"},0),
        toggle(ReverbOn,"Reverb On",true),
        num(ReverbAmount,"Reverb",0,100,30,.1f,{},"%",percent),
        choice(Oversample,"Oversample",{"off","2x","4x","8x"},1),
        toggle(Freeze,"Freeze",false),
        num(Motion,"Motion",0,100,0,.1f,{},"%",percent),num(Texture,"Texture",0,100,0,.1f,{},"%",percent),
        num(Bloom,"Bloom",0,100,0,.1f,{},"%",percent),num(Chaos,"Chaos",0,100,0,.1f,{},"%",percent) };
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
// The input ring plus four halfband-filtered mipmaps. Each grain selects ceil(log2 pitch ratio), so every
// pitched-up reader is band-limited before resampling. Level k's sample j sits at 2^k j - delay[k].
// Four-point Hermite reads share stereo coordinates; only the nonlinear feedback needs oversampling.
constexpr int levels = 5;
constexpr double scale[levels] { 1, .5, .25, .125, .0625 };
constexpr int delay[levels] { 0, 63, 189, 441, 945 };
// 127-tap Kaiser halfband: steep enough to reject the first fold above Nyquist, not just distant tones.
constexpr float halfband[32] { 0.3179876426f, -0.1051353898f, 0.06205975135f, -0.0432535297f, 0.03255484816f, -0.0255600393f, 0.02057837779f, -0.0168229528f, 0.01387852327f, -0.011505438f, 0.009555592982f, -0.007932243802f, 0.006569126778f, -0.005418887495f, 0.004446332002f, -0.003624329037f, 0.002931242885f, -0.002349286908f, 0.001863449945f, -0.001460788898f, 0.001129960166f, -0.0008609088363f, 0.0006446625028f, -0.0004731940527f, 0.0003393290235f, -0.0002366806736f, 0.0001596010718f, -0.0001031401866f, 6.300762406e-05f, -3.553363926e-05f, 1.7627528e-05f, -6.732614478e-06f };
constexpr int margin = 192; // minimum read distance; higher mips add their FIR delay and the lookahead span

inline __attribute__((always_inline)) float hermite(const std::vector<float>& d, size_t mask, int64 i, float f)
{
    const float a = d[(size_t) (i - 1) & mask], b = d[(size_t) i & mask], c = d[(size_t) (i + 1) & mask], e = d[(size_t) (i + 2) & mask];
    return b + f * (0.5f * (c - a) + f * (a - 2.5f * b + 2 * c - 0.5f * e + f * (0.5f * (e - a) + 1.5f * (b - c))));
}

struct Capture
{
    bool wide = false;
    std::vector<float> data[levels][2];
    size_t mask[levels] {};
    int64 head = 0;        // frames written
    float gain = 1;        // the write crossfade: 1 live, 0 frozen (writing stops)
    float peak = 0;        // the editor's waveform: the peak of the slice being written
    int count = 0, slice = 1, startup = 240;
    double history = 384000;
    std::array<std::atomic<float>, silt::Feed::bins>* wave = nullptr;
    std::atomic<int>* at = nullptr;

    void prepare(size_t size, int sliceLength, double frames)
    {
        for (int k = 0; k < levels; ++k)
            for (auto& c : data[k]) c.assign(size >> k, 0.0f);
        for (int k = 0; k < levels; ++k) mask[k] = (size >> k) - 1;
        slice = jmax(1, sliceLength), history = frames;
        startup = jmax(1, roundToInt(frames / silt::maxBackSeconds * .005));
        wide = SystemStats::hasAVX2() && std::getenv("SILT_PORTABLE") == nullptr;
    }
    void reset()
    {
        for (auto& level : data) for (auto& c : level) std::fill(c.begin(), c.end(), 0.0f);
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
            const int k = at->load(std::memory_order_relaxed);
            (*wave)[(size_t) k].store(peak, std::memory_order_relaxed), at->store((k + 1) % silt::Feed::bins, std::memory_order_relaxed);
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
    void read(double t, int k, float& l, float& r, double atHead) const
    {
        // Very long synced / reverse / frozen grains loop within recent history. A short splice avoids a step
        // when a reader crosses either edge; ordinary reads away from the edges are unchanged.
        constexpr double fade = 128;
        const int readMargin = jmax(margin, delay[k] + 4 * (1 << k) + batch);
        const double lo = atHead - history + readMargin, hi = atHead - readMargin, period = hi - lo - fade;
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
#if defined(SILT_AVX2)
    __attribute__((target("avx2"))) static __m256 wideCubic(__m256 a, __m256 b, __m256 c, __m256 e, __m256 f)
    {
        const auto half = _mm256_set1_ps(.5f), oneHalf = _mm256_set1_ps(1.5f);
        const auto inner = _mm256_add_ps(_mm256_mul_ps(half, _mm256_sub_ps(e, a)), _mm256_mul_ps(oneHalf, _mm256_sub_ps(b, c)));
        const auto middle = _mm256_add_ps(_mm256_sub_ps(_mm256_add_ps(_mm256_sub_ps(a, _mm256_mul_ps(_mm256_set1_ps(2.5f), b)), _mm256_mul_ps(_mm256_set1_ps(2), c)), _mm256_mul_ps(half, e)), _mm256_mul_ps(f, inner));
        return _mm256_add_ps(b, _mm256_mul_ps(f, _mm256_add_ps(_mm256_mul_ps(half, _mm256_sub_ps(c, a)), _mm256_mul_ps(f, middle))));
    }
    // Runtime dispatch preserves older Intel hosts and the portable/NEON compiler path on Apple Silicon.
    __attribute__((target("avx2"))) void wideRead(const double* positions, int k,
                                                float* l, float* r, int length) const
    {
        const double base = (double) ((int64) ((positions[0] + delay[k]) * scale[k]) & ~(int64) mask[k]);
        int j = 0;
        for (; j + 8 <= length; j += 8)
        {
            const auto t0 = _mm256_sub_pd(_mm256_mul_pd(_mm256_add_pd(_mm256_loadu_pd(positions + j), _mm256_set1_pd(delay[k])), _mm256_set1_pd(scale[k])), _mm256_set1_pd(base));
            const auto t1 = _mm256_sub_pd(_mm256_mul_pd(_mm256_add_pd(_mm256_loadu_pd(positions + j + 4), _mm256_set1_pd(delay[k])), _mm256_set1_pd(scale[k])), _mm256_set1_pd(base));
            const auto whole0 = _mm256_floor_pd(t0), whole1 = _mm256_floor_pd(t1);
            const auto index = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm256_cvttpd_epi32(whole0)), _mm256_cvttpd_epi32(whole1), 1);
            const auto f = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm256_cvtpd_ps(_mm256_sub_pd(t0, whole0))), _mm256_cvtpd_ps(_mm256_sub_pd(t1, whole1)), 1);
            const auto m = _mm256_set1_epi32((int) mask[k]);
            for (int c = 0; c < 2; ++c)
            {
                const auto* d = data[k][c].data();
                const auto a = _mm256_i32gather_ps(d, _mm256_and_si256(_mm256_sub_epi32(index, _mm256_set1_epi32(1)), m), 4);
                const auto b = _mm256_i32gather_ps(d, _mm256_and_si256(index, m), 4);
                const auto cv = _mm256_i32gather_ps(d, _mm256_and_si256(_mm256_add_epi32(index, _mm256_set1_epi32(1)), m), 4);
                const auto e = _mm256_i32gather_ps(d, _mm256_and_si256(_mm256_add_epi32(index, _mm256_set1_epi32(2)), m), 4);
                const auto value = wideCubic(a, b, cv, e, f);
                _mm256_storeu_ps((c == 0 ? l : r) + j, value);
            }
        }
        for (; j < length; ++j) readAt(positions[j], k, l[j], r[j]);
    }
#endif
    static constexpr int batch = 128;
    void readBatch(const double* positions, int k, float* l, float* r, int length,
                   bool frozen, float freezeStep) const
    {
        alignas(16) float a[2][batch], b[2][batch], c[2][batch], e[2][batch], fraction[batch];
        const int readMargin = jmax(margin, delay[k] + 4 * (1 << k) + batch);
        bool safe = true;
        for (int i = 0; i < length; ++i)
            safe = safe && positions[i] >= head - history + readMargin + 128 + batch && positions[i] <= head - readMargin - 128;
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
#if defined(SILT_AVX2)
        if (wide && length >= 8) { wideRead(positions, k, l, r, length); return; }
#endif
        float* out[] { l, r };
        for (int i = 0; i < length; ++i)
        {
            const double t = (positions[i] + delay[k]) * scale[k];
            auto index = (int64) t;
            if ((double) index > t) --index; // exact floor, including negative capture times
            fraction[i] = (float) (t - (double) index);
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto& d = data[k][ch];const auto m = mask[k];
                a[ch][i] = d[(size_t) (index - 1) & m], b[ch][i] = d[(size_t) index & m];
                c[ch][i] = d[(size_t) (index + 1) & m], e[ch][i] = d[(size_t) (index + 2) & m];
            }
        }
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < length; ++i)
            {
                const float f = fraction[i], av = a[ch][i], bv = b[ch][i], cv = c[ch][i], ev = e[ch][i];
                out[ch][i] = bv + f * (0.5f * (cv - av) + f * (av - 2.5f * bv + 2 * cv - 0.5f * ev + f * (0.5f * (ev - av) + 1.5f * (bv - cv))));
            }
    }
};

// ================================ Grains ================================
struct Grain
{
    int wait = 0, left = 0, traceIn = 0, length = 0;    // samples until it sounds, samples it still sounds (0 = a free voice)
    double phase = 0, inc = 0;  // through the window
    double pos = 0, step = 1;  // speed: the read position and its step (+-r); pitch: the anchor, moving +-1
    float tap = 0, tapStep = 0, span = 0; // pitch: the first tap's distance behind the anchor, its step, the tap window
    float gl = 0, gr = 0, pitch = 0;
    float peak = 0.5f, attack = 1, decay = 1;
    unsigned tag = 0;
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
    uint32 seed = 1, nextGrain = 0;
    // MIDI: held notes (last-note priority) and the fixed controller map's smoothed values, -1 until received.
    std::array<int, 128> held {};
    std::array<float, 128> velocities {};
    int heldCount = 0;
    float velocity = 1;
    float cc[4] {}, ccTarget[4] {};
    int lastNote = 60;
    // All quality paths are prepared once; changing quality never allocates on the audio thread.
    std::array<std::unique_ptr<dsp::Oversampling<float>>, 4> oversamplers;
    int quality = -1, latency = 0, previousQuality = 0, previousLatency = 0, qualityFade = 0;
    std::array<float, 512> dryDelay[2] {}, wetDelay[2] {};
    size_t dryAt = 0;
    float meters[3] {};
    
    float fbRing[2][K] {};
    int64 now = 0;               // samples since reset
    double overlap = 0;          // smoothed sum of the windows sounding
    float overlapAttack = 0, overlapRelease = 0;
    OnePole lows[2], highs[2], dc[2], outputDc[2];
    std::vector<float> diffusion[4];
    size_t diffusionAt[4] {};
    float level = 0, inputLevel = 0, levelGlide = 0, inputAttack = 0, inputRelease = 0;
    SmoothedValue<float> mix, gain, verb, width, feedback, lowpass, highpass, power;
    SiltReverb reverb; // SILT_REVERB

    void prepare(double sr, silt::Feed& feed)
    {
        fs = sr > 0 && sr < 1.0e7 ? sr : 48000.0;
        const auto size = (size_t) nextPowerOfTwo((int) ((silt::maxBackSeconds + 2.5) * fs));
        for (int c = 0; c < 1; ++c)
        {
            capture[c].wave = &feed.wave[c], capture[c].at = &feed.at[c];
            capture[c].prepare(size, (int) (silt::maxBackSeconds * fs / silt::Feed::bins), silt::maxBackSeconds * fs);
        }
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
        voiceCount = 0, grains = {}, onsets[0] = onsets[1] = {}, phase = 0, lastTick = -1, wasPlaying = false, stream = 1, scanned = 0, seed = 0x5117u;
        nextGrain = 0;
        heldCount = 0, velocity = 1, now = 0, overlap = 0, level = inputLevel = 0, lastNote = 60;
        quality = -1, qualityFade = 0, dryAt = 0, dryDelay[0].fill(0), dryDelay[1].fill(0), wetDelay[0].fill(0), wetDelay[1].fill(0);
        for (auto& os : oversamplers) os->reset();
        std::fill(std::begin(meters), std::end(meters), 0.0f);
        feed.note.store(60), feed.time.store(0), feed.events.store(0);
        std::fill(std::begin(cc), std::end(cc), -1.0f), std::fill(std::begin(ccTarget), std::end(ccTarget), -1.0f);
        for (auto& r : fbRing) std::fill(std::begin(r), std::end(r), 0.0f);
        for (auto& d : diffusion) std::fill(d.begin(), d.end(), 0.0f);
        std::fill(std::begin(diffusionAt), std::end(diffusionAt), (size_t) 0);
        lows[0] = lows[1] = highs[0] = highs[1] = dc[0] = dc[1] = outputDc[0] = outputDc[1] = {};
        reverb.reset(); // SILT_REVERB
        for (int c = 0; c < 2; ++c)
        {
            for (auto& w : feed.wave[c]) w.store(0, std::memory_order_relaxed);
            feed.at[c].store(0, std::memory_order_relaxed);
        }
        feed.fired.store(0, std::memory_order_relaxed);
        feed.lastFire.store(-1, std::memory_order_relaxed), feed.lastPitch.store(0, std::memory_order_relaxed), feed.lastLength.store(0, std::memory_order_relaxed);
        read();
        width.setCurrentAndTargetValue(0.8f);
        feedback.setCurrentAndTargetValue((0.12f + 0.48f * macro(Bloom)));
        lowpass.setCurrentAndTargetValue(coefficient(4000, fs)), highpass.setCurrentAndTargetValue(coefficient(30, fs));
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
    float macro(int i) const { return jmin(1.0f, v[i] / 100 + control(i == Motion ? 0 : i == Texture ? 1 : i == Bloom ? 2 : 3)); }
    float control(int k) const { return jmax(0.0f, cc[k]); } // a controller's value, 0 until received
    static float lookup(const std::array<float, 1025>& t, float x)
    {
        const float at = jlimit(0.0f, 1024.0f, x * 1024);
        const int j = jmin(1023, (int) at);
        return t[(size_t) j] + (at - (float) j) * (t[(size_t) j + 1] - t[(size_t) j]);
    }

    double separation() const { return v[Scatter] / 25 + 4 * macro(Chaos) + macro(Motion); }
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

    // Starts one grain after `wait` samples; alternating streams have independent pan and a small size difference.
    void spawn(int wait, double bpm, bool frozen, int side)
    {
        if (voiceCount == silt::maxGrains) return; // all voices busy: skip this one, never steal (no clicks)
        auto* g = &grains[(size_t) voiceCount++];
        g->tag = ++nextGrain;
        const double stereo = 0.8 + 0.2 * macro(Bloom);
        const int noteIndex = syncIndex(sizeTravel.to(v[Size]));
        const auto note = [&](int k) { return 240.0 * silt::noteNum[k] / silt::noteDen[k] / bpm * fs; };
        double size = v[SizeSync] < .5f ? note(noteIndex) : v[Size] * fs / 1000;
        size *= (1 + (2 * random() - 1) * 0.65 * macro(Texture)) * (side == 1 ? 1 + 0.04 * stereo : 1.0);
        size = jlimit(0.0005 * fs, v[SizeSync] > .5f ? 4 * fs : jmax(4 * fs, note(silt::numNotes - 1)), size);
        // Randomisation precedes chromatic snapping. Zero chaos preserves fractional base pitch.
        double st = v[Pitch] + (2 * random() - 1) * 24 * macro(Chaos);
        if (macro(Chaos) > 0) st = std::round(st);
        st += heldCount > 0 ? held[(size_t) heldCount - 1] - 60 : 0;
        st = jlimit(-48.0, 48.0, st);
        const double r = std::exp2(st / 12);
        const int mip = r <= 1 ? 0 : jmin(levels - 1, (int) std::ceil(std::log2(r)));
        const int readMargin = jmax(margin, delay[mip] + 4 * (1 << mip) + Capture::batch);
        const bool reverse = random() < 0.65f * macro(Chaos), shifted = v[PitchMode] < 0.5f;
        const double dir = reverse ? -1 : 1;
        // Keep the tap modulation below 25 Hz even at +48 st. Grain length stays independent of pitch;
        // a tiny grain can still read through a longer, smooth pitch crossfade.
        // The read: how long it sounds and how much source it covers.
        const double dur = jmin(2.0e9, shifted ? size : size / r), span = shifted ? .04 * fs * jmax(1.0, r - 1) : 0;
        // How far back it starts: position (+ the scan) + spray, kept inside what the buffer holds for the whole grain
        // (live, the head moves on while it plays; frozen, it stands still). a: room needed ahead, b: behind.
        const double maxBack = silt::maxBackSeconds * fs;
        double back = position(bpm);
        const double spray = (v[Scatter] * 20 + 600 * macro(Chaos)) * fs / 1000;
        double u = random();
        u = std::pow(u, 1 + 3 * (1 - macro(Texture))); // slope: small distances more likely
        const int towards = macro(Motion) > .5f ? 1 : 0;
        back += spray * (towards == 1 ? u : towards == 2 ? -u : (random() < 0.5f ? u : -u)) + scanned;
        const double extent = shifted ? size : dur * r;
        double a = 0, b = 0;
        if (frozen) a = extent + (shifted && reverse ? span : 0), b = shifted && !reverse ? span : 0;
        else if (shifted) a = reverse ? size + span : 0, b = reverse ? size : span;
        else if (reverse) a = extent, b = dur;
        else a = jmax(0.0, dur * (r - 1)), b = jmax(0.0, dur * (1 - r));
        // Fit the whole segment when possible; longer notes retain their duration and use the capture's splice.
        back = a + b < maxBack - 2 * readMargin ? jlimit(readMargin + a, maxBack - readMargin - b, back) : jlimit((double) readMargin, maxBack - readMargin, back);
        const double start = (double) capture[source()].head + (frozen ? 0 : wait) - back;
        g->wait = wait, g->traceIn = 0, g->length = jmax(2, (int) dur), g->left = g->length, g->phase = 0, g->inc = 1.0 / g->left;
        g->shape = macro(Texture) < .33f ? 10 : macro(Texture) < .66f ? 7 : 3, g->peak = 0.5f - 0.3f * macro(Texture);
        g->attack = 0.5f / g->peak, g->decay = 0.5f / (1 - g->peak);
        g->shifted = shifted && std::abs(r - 1) > 1e-8, g->source = source(), g->pitch = (float) st;
        g->step = shifted ? dir : dir * r;
        g->pos = shifted ? (reverse ? start + size : start) : (reverse ? start + dur * r : start);
        g->span = (float) jmax(2.0, 2 * std::round(span / 2)), g->tap = 0, g->tapStep = (float) (1 - r);
        // The lower mip of a crossfade still aliases for fractional pitch ratios. A single band-limited
        // level rejects that fold and halves the read work; Hermite preserves smooth sub-sample motion.
        g->level = mip;
        // Pan: the stream's side of the width plus a random spread; equal power, unity in the middle.
        const double pan = jlimit(-1.0, 1.0, (side == 0 ? -0.6 : 0.6) * stereo + (2 * random() - 1) * jmin(1.0, 0.25 + 0.7 * macro(Motion)));
        const double angle = (pan + 1) * pi / 4;
        g->gl = (float) (velocity * std::cos(angle) * std::sqrt(2.0)), g->gr = (float) (velocity * std::sin(angle) * std::sqrt(2.0));
    }
    static int source() { return 0; }
    void burst(int at, double bpm, bool frozen) // a MIDI note or a transient: density grains over one tick
    {
        spawn(at, bpm, frozen, stream ^= 1);
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
            lastNote = m.getNoteNumber();
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
            // Fixed smoothed macro offsets, never writes automation: CC1/2/3/4 motion/texture/bloom/chaos.
            constexpr int map[] { 1, 2, 3, 4 };
            for (int k = 0; k < 4; ++k)
                if (map[k] == m.getControllerNumber())
                {
                    if (cc[k] < 0) cc[k] = 0.0f;
                    ccTarget[k] = (float) m.getControllerValue() / 127;
                }
            if (m.getControllerNumber() == 121)
                std::fill(std::begin(cc), std::end(cc), -1.0f), std::fill(std::begin(ccTarget), std::end(ccTarget), -1.0f);
        }
    }

    void trace(Grain& g, silt::Feed& feed, int64 frame, int64 head)
    {
        if (exactlyEqual(g.phase, 0.0))
        {
            feed.fired.fetch_add(1, std::memory_order_relaxed);
            feed.lastFire.store(frame, std::memory_order_relaxed);
            feed.lastPitch.store(g.pitch, std::memory_order_relaxed);
            feed.lastLength.store(g.left, std::memory_order_relaxed);
        }
        if (--g.traceIn <= 0)
        {
            g.traceIn = jmax(1, (int) (fs * .016));
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
    void cloud(int length, bool frozen, silt::Feed& feed, float out[2][Capture::batch], float* weights)
    {
        for (int c = 0; c < 2; ++c) std::fill_n(out[c], length, 0.0f);
        std::fill_n(weights, length, 0.0f);
        int frozenHeads[Capture::batch] {}, advance = 0;
        float futureGain = capture[0].gain;
        if (frozen)
            for (int j = 0; j < length; ++j)
            {
                frozenHeads[j] = advance;
                futureGain = jmax(0.0f, futureGain - freezeStep);
                if (futureGain > 0) ++advance;
            }
        for (int k = 0; k < voiceCount; ++k)
        {
            auto& g = grains[(size_t) k];
            if (g.wait > 0) { g.wait -= length; continue; }
            double positions[Capture::batch], positions2[Capture::batch];
            alignas(16) float w[Capture::batch], a[Capture::batch], audio[2][Capture::batch], other[2][Capture::batch];
            for (int j = 0; j < length; ++j)
            {
                trace(g, feed, now + j, capture[0].head + (frozen ? frozenHeads[j] : j));
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
            capture[0].readBatch(positions, g.level, audio[0], audio[1], length, frozen, freezeStep);
            if (g.shifted)
            {
                capture[0].readBatch(positions2, g.level, other[0], other[1], length, frozen, freezeStep);
                for (int c = 0; c < 2; ++c)
                    for (int j = 0; j < length; ++j) audio[c][j] = a[j] * audio[c][j] + (1 - a[j]) * other[c][j];
            }
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
        const bool synced = v[SizeSync] < 0.5f && playing;
        if (synced)
        {
            const double quarters = 1.0 / jmax(1, roundToInt(v[Density] / 2));
            const double target = ppq / quarters;
            if (!wasPlaying || std::abs(target - phase) > 1e-5) lastTick = std::ceil(target - 1e-9) - 1;
            phase = target;
        }
        else if (wasPlaying) phase -= std::floor(phase);
        wasPlaying = synced;
        const int trigger = roundToInt(v[Trigger]);
        const float threshold = Decibels::decibelsToGain(-30.0f);
        mix.setTargetValue(v[Mix] / 100), gain.setTargetValue(Decibels::decibelsToGain(v[Output]));
        // Fixed feedback band; bloom increases diffusion/feedback, leaving cutoff coefficients block-invariant.
        lowpass.setTargetValue(coefficient(4000, fs)), highpass.setTargetValue(coefficient(30, fs));
        const float dcCoef = coefficient(20, fs), outputDcCoef = coefficient(8, fs);
        // Similar grains add in amplitude; scattered/pitched grains add mostly in power. Interpolate the overlap
        // law using their source separation, preserving level without analysing or limiting the output.
        const int selectedQuality = roundToInt(v[Oversample]);
        if (quality != selectedQuality)
        {
            previousQuality = jmax(0, quality), previousLatency = latency;
            qualityFade = quality < 0 ? 0 : 128;
            quality = selectedQuality;
            oversamplers[(size_t) quality]->reset();
            latency = roundToInt(oversamplers[(size_t) quality]->getLatencyInSamples());
            p.setLatencySamples(latency);
        }
        const double spacing = period(bpm);
        const double quartersPerGrain = 1.0 / jmax(1, roundToInt(v[Density] / 2));
        verb.setTargetValue(v[ReverbOn] > .5f ? v[ReverbAmount] / 100 : 0.0f);
        const double scanRange = jmax(1.0, silt::maxBackSeconds * fs - v[Position] * fs / 1000);
        auto event = midiBuffer.cbegin();
        float cloudFrames[2][Capture::batch] {}, cloudWeights[Capture::batch] {};
        int cloudAt = 0, cloudLength = 0;
        for (int chunk = 0; chunk < n;)
        {
            const int count = jmin(K - (int) (now % K), n - chunk);
            const int64 chunkTime = now;
            float frames[4][K] {}, old[4][K] {}, dryFrames[2][K] {}, sideFrames[K] {}, bloomFrames[K] {};
            for (int i = 0; i < count; ++i)
            {
                const int t = chunk + i;
                bool frozen = v[Freeze] > 0.5f;
                for (; event != midiBuffer.cend() && (*event).samplePosition <= t; ++event)
                    if ((*event).numBytes <= 3) midi((*event).getMessage(), 0, bpm, frozen); // ignore SysEx without allocating
                for (int k = 0; k < 4; ++k)
                    if (ccTarget[k] >= 0) cc[k] += ccGlide * (ccTarget[k] - cc[k]);
                frozen = v[Freeze] > 0.5f;
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
                const float* selected = source() == 0 ? x : side;
                const float input = 0.5f * (std::abs(selected[0]) + std::abs(selected[1]));
                inputLevel += (input > inputLevel ? inputAttack : inputRelease) * (input - inputLevel);
                for (int c = 0; c < 1; ++c)
                {
                    auto& cap = capture[c];
                    cap.gain = jlimit(0.0f, 1.0f, cap.gain + (frozen ? -freezeStep : freezeStep));
                    const float* in = c == 0 ? x : side;
                    const float add = c == source() ? 1.0f : 0.0f;
                    if (cap.gain > 0) cap.write(in[0] + add * fbRing[0][now % K], in[1] + add * fbRing[1][now % K]);
                }
                const double scan = -0.25 * macro(Motion);
                if (std::abs(scan) < 1e-9) scanned = 0;
                else
                {
                    scanned -= scan;
                    if (scanned >= scanRange) scanned -= scanRange;
                }
                if (trigger == 0)
                {
                    bool tick = false;
                    if (synced)
                    {
                        phase = (ppq + t * bpm / 60 / fs) / quartersPerGrain;
                        const double whole = std::floor(phase + 1e-9);
                        tick = !exactlyEqual(whole, lastTick), lastTick = whole;
                        phase += 1 / spacing;
                    }
                    else if ((phase += 1 / spacing) >= 1 - 1e-10)
                        tick = true, phase -= std::floor(phase + 1e-10);
                    if (tick) spawn((int) (random() * (0.4f * macro(Motion) + 0.5f * macro(Chaos)) * spacing), bpm, frozen, stream ^= 1);
                }
                if ((hitIn && trigger == 1) || (hitSide && trigger == 2)) burst(0, bpm, frozen);
                feedback.setTargetValue(0.12f + 0.48f * macro(Bloom));
                width.setTargetValue(0.8f + 0.2f * macro(Bloom));
                float y[2] {}, weight = 0;
                if (cloudAt == cloudLength)
                {
                    cloudAt = cloudLength = 0;
                    if (trigger == 0 && voiceCount >= 16)
                    {
                        int span = jmin(n - t, (int) jlimit(1.0, (double) Capture::batch, std::floor((1 - (phase - std::floor(phase))) * spacing - 1e-6)));
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
                    y[0] = cloudFrames[0][cloudAt], y[1] = cloudFrames[1][cloudAt], weight = cloudWeights[cloudAt++];
                }
                else
                {
                    for (int k = 0; k < voiceCount; ++k)
                    {
                        auto& g = grains[(size_t) k];
                        if (g.wait > 0) { --g.wait; continue; }
                        trace(g, feed, now, capture[0].head);
                        const auto& cap = capture[g.source];
                        const float w = envelope(g);
                        g.phase += g.inc, --g.left;
                        float l, r;
                        if (!g.shifted) cap.read(g.pos, g.level, l, r, cap.head), g.pos += g.step;
                        else
                        {
                            float l2, r2;
                            const float second = g.tap + 0.5f * g.span >= g.span ? g.tap - 0.5f * g.span : g.tap + 0.5f * g.span;
                            const float a = lookup(taps, g.tap / g.span), b = 1 - a;
                            cap.read(g.pos - g.step * g.tap, g.level, l, r, cap.head), cap.read(g.pos - g.step * second, g.level, l2, r2, cap.head);
                            l = a * l + b * l2, r = a * r + b * r2;
                            g.pos += g.step, g.tap += g.tapStep;
                            if (g.tap < 0 || g.tap >= g.span) g.tap -= g.span * std::floor(g.tap / g.span);
                        }
                        y[0] += w * g.gl * l, y[1] += w * g.gr * r, weight += w;
                        if (g.left == 0) g = grains[(size_t) --voiceCount], --k;
                    }
                }
                // Actual window overlap, with a slow release between clouds. No compressor or limiter.
                overlap += (weight > overlap ? overlapAttack : overlapRelease) * (weight - overlap);
                power.setTargetValue((float) (1 - 0.5 * jlimit(0.0, 1.0, separation())));
                const float exponent = power.getNextValue();
                const float base = jmax(1.0f, weight, (float) overlap);
                const float norm = 0.85f / (exponent <= .500001f ? std::sqrt(base) : exponent >= .999999f ? base : std::pow(base, exponent));
                const float mid = 0.5f * (y[0] + y[1]) * norm, sd = 0.5f * (y[0] - y[1]) * norm * width.getNextValue();
                y[0] = mid + sd, y[1] = mid - sd;
                const float lp = lowpass.getNextValue(), hp = highpass.getNextValue(), fbAmount = feedback.getNextValue();
                float f[2];
                for (int c = 0; c < 2; ++c)
                {
                    y[c] -= dc[c].low(y[c], dcCoef);
                    const float low = lows[c].low(y[c], lp), high = low - highs[c].low(low, hp);
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
                {
                    auto* samplesUp = up.getChannelPointer(c);
                    const auto bounds = FloatVectorOperations::findMinAndMax(samplesUp, (int) up.getNumSamples());
                    if (jmax(std::abs(bounds.getStart()), std::abs(bounds.getEnd())) <= .5f)
                    {
                        // Padé tanh on [-1,1]: below float roundoff, and vectorisable across the FIR quantum.
                        for (size_t i = 0; i < up.getNumSamples(); ++i)
                        {
                            const float x = 2 * samplesUp[i], xx = x * x;
                            samplesUp[i] = .5f * x * (945 + xx * (105 + xx)) / (945 + xx * (420 + 15 * xx));
                        }
                    }
                    else
                        for (size_t i = 0; i < up.getNumSamples(); ++i) samplesUp[i] = .5f * std::tanh(2 * samplesUp[i]);
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
                for (int c = 0; c < 4; ++c) frame[c] = i < fading ? old[c][i] + blend * (frames[c][i] - old[c][i]) : frames[c][i];
                for (int c = 0; c < 2; ++c)
                {
                    fbRing[c][(chunkTime + i) % K] = frame[c + 2];
                    // Grain readers are already band-limited. Only the nonlinear feedback needs oversampling;
                    // keep the wet signal aligned with its declared quality latency using an exact delay.
                    wetDelay[c][dryAt] = frames[c][i];
                    const auto wet = [&](int offset) { return wetDelay[c][(dryAt + wetDelay[c].size() - (size_t) offset) % wetDelay[c].size()]; };
                    y[c] = qualityFade > 0 ? wet(previousLatency) + blend * (wet(latency) - wet(previousLatency)) : wet(latency);
                    dryDelay[c][dryAt] = dry[c];
                    const auto tap = [&](int offset) { return dryDelay[c][(dryAt + dryDelay[c].size() - (size_t) offset) % dryDelay[c].size()]; };
                    dry[c] = qualityFade > 0 ? tap(previousLatency) + blend * (tap(latency) - tap(previousLatency)) : tap(latency);
                }
                qualityFade = jmax(0, qualityFade - 1);
                dryAt = (dryAt + 1) % dryDelay[0].size();
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
                for (int c = 0; c < 2; ++c) meters[c] = jmax(std::abs(buffer.getSample(jmin(c, outs - 1), t)), meters[c] * .9995f);
                meters[2] = jmax(sideFrames[i], meters[2] * .9995f);
            }
            chunk += count;
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
        feed.captureHead.store(capture[0].head, std::memory_order_relaxed);
        feed.time.store(now, std::memory_order_relaxed), feed.sampleRate.store((float) fs, std::memory_order_relaxed);
        feed.note.store(lastNote, std::memory_order_relaxed);
        for (size_t i = 0; i < 3; ++i) feed.meter[i].store(meters[i], std::memory_order_relaxed);
    }
};

SiltProcessor::SiltProcessor(File directory)
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", AudioChannelSet::stereo(), false).withOutput("Output", AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "SILT", layout(*this)), presetDirectory(directory), engine(std::make_unique<Engine>())
{
    for (int i = 0; i < silt::numParams; ++i) engine->raw[(size_t) i] = apvts.getRawParameterValue(ids[i]);
    preset = 4;
    setLatencySamples(0);
}
SiltProcessor::~SiltProcessor() = default;
void SiltProcessor::prepareToPlay(double sr, int)
{
    engine->prepare(sr, feed), resetPending = true;
    setLatencySamples(roundToInt(engine->oversamplers[(size_t) roundToInt(apvts.getRawParameterValue("oversample")->load())]->getLatencyInSamples()));
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
            if (!slots[k].empty()) // "preset;v0,..,v17", normalised, 9 significant digits: every float reads back exactly
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

// Values in frozen ID order. Preset 4 (tide) equals the parameter defaults.
// All factory voicing is expressed by the same 18 visible parameters; no hidden preset values.
const silt::Preset silt::presets[] {
    {"sub dust", {100,0,90,1,16,140,12,-12,1,0,1,12,1,0,0,25,5,0}},
    {"grit",     {100,3,14,1,42,240,38,0,1,0,0,8,1,0,12,90,5,20}},
    {"glass",    {100,0,85,1,28,700,30,12,0,0,1,28,1,0,5,20,15,0}},
    {"halo",     {100,0,650,1,18,800,55,0,0,0,1,55,1,0,15,10,65,0}},
    {"tide",     {100,0,51.78f,0,24,1200,45,7,0,0,1,30,1,0,0,0,0,0}},
    {"choir",    {100,0,900,1,22,1600,32,0,0,0,1,48,1,0,20,15,45,5}},
    {"tines",    {100,6,35,1,14,450,18,7,1,0,1,22,1,0,5,75,10,0}},
    {"chime",    {100,0,160,1,12,900,25,12,0,0,1,42,1,0,10,45,45,0}},
    {"rain",     {100,0,55,1,48,1800,78,0,1,0,1,32,1,0,35,55,25,35}},
    {"drift",    {100,0,1200,1,15,2200,62,-7,0,0,1,45,1,0,65,20,55,15}}
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
    put(SizeSync, 1), put(Density, between(8, 40)), put(Position, between(80, 2000)), put(Scatter, between(5, 70));
    const float intervals[] { -12, -7, 0, 0, 0, 7, 12 };
    put(Pitch, intervals[r.nextInt(7)]), put(PitchMode, r.nextInt(2)), put(Trigger, 0), put(Freeze, 0);
    put(ReverbOn, 1), put(ReverbAmount, between(10, 45));
    setIdentity(-1, "random");
}
