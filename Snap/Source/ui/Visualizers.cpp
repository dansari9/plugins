#include "Visualizers.h"
#include "Braille.h"
#include "Riso.h"
#include "../PluginProcessor.h"
#include "../dsp/modules/Press.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

using namespace juce;

namespace snap::ui
{
namespace
{
using namespace colours;
constexpr float turn = MathConstants<float>::twoPi;
constexpr int fftOrder = 11, fftSize = 1 << fftOrder;
constexpr size_t columns = 34;
static_assert(fftSize == SnapProcessor::Display::scopeSize);

float unit(float value) { return jlimit(0.0f, 1.0f, value); }
float logNorm(float v, float lo, float hi) { return unit(std::log(jmax(lo, v) / lo) / std::log(hi / lo)); }
float randomUnit(std::uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;
    return (float) (seed >> 8) / 16777216.0f;
}

struct Spectrum
{
    explicit Spectrum(const SnapProcessor::Display& display)
    {
        dsp::WindowingFunction<float>::fillWindowingTables(window.data(), window.size(), dsp::WindowingFunction<float>::hann);
        for (size_t i = 0; i < columns; ++i)
            centres[i] = 20.0f * std::pow(1000.0f, (float) i / (float) (columns - 1));

        // Use the text helper's layout with cached outlines.
        const auto font = mono(11);
        for (size_t i = 0; i < labels.size(); ++i)
        {
            GlyphArrangement glyphs;
            glyphs.addLineOfText(font, i == 0 ? "sub" : "hiss", 0, 0);
            glyphs.justifyGlyphs(0, glyphs.getNumGlyphs(), 182, 11 + 26.0f * (float) i, 30, 14.3f, Justification::centredLeft);
            glyphs.createPath(labels[i]);
            hits[i] = display.hits[i].load(std::memory_order_relaxed);
        }
    }

    void read(const SnapProcessor::Display& display, bool active, bool hasInput, float rate, float elapsed)
    {
        const int write = display.scopeWrite.load(std::memory_order_acquire);
        bool signal = false;
        if (active && (hasInput || write != lastWrite))
        {
            for (int i = 0; i < fftSize; ++i)
            {
                const auto index = (size_t) ((write + i) & (fftSize - 1));
                const auto sample = display.scope[index].load(std::memory_order_relaxed);
                buffer[(size_t) i] = sample * window[(size_t) i];
                signal |= sample != 0;
            }
            if (signal)
                fft.performFrequencyOnlyForwardTransform(buffer.data(), true);
        }
        lastWrite = active ? write : -1;

        const float attack = std::exp(-elapsed / 0.012f), release = std::exp(-elapsed / 0.3f);
        const float binsPerHz = fftSize / rate;
        for (size_t i = 0; i < columns; ++i)
        {
            float target = 0;
            const float bin = centres[i] * binsPerHz;
            if (signal && bin <= fftSize / 2)
            {
                // Interpolate narrow bands and keep the peak in wider ones.
                const float centre = jmax(1.0f, bin);
                const auto left = (size_t) centre;
                float peak = std::lerp(buffer[left], buffer[jmin(left + 1, (size_t) fftSize / 2)], centre - (float) left);
                const int low = jmax(1, (int) std::ceil(bin / halfBand));
                const int high = jmin(fftSize / 2, (int) (bin * halfBand));
                for (int k = low; k <= high; ++k)
                    peak = jmax(peak, buffer[(size_t) k]);
                const float db = Decibels::gainToDecibels(peak * (2.0f / fftSize), -72.0f);
                target = 21 * unit((db + 72) / 72);
            }
            heights[i] = std::lerp(target, heights[i], target > heights[i] ? attack : release);
        }
    }

    dsp::FFT fft { fftOrder };
    std::array<float, fftSize * 2> buffer {};
    std::array<float, fftSize> window {};
    std::array<float, columns> centres {}, heights {};
    const float halfBand = std::pow(1000.0f, 0.5f / (float) (columns - 1));
    std::array<int, 2> hits {};
    std::array<double, 2> hitUntil {};
    std::array<Path, 2> labels;
    int lastWrite = -1;
};

struct CloudDot { float x, y, rank; };
const auto cloudDots = []
{
    std::array<CloudDot, 260> points {};
    std::uint32_t seed = 21, densitySeed = 22;
    for (auto& p : points)
    {
        const float x = 10 + 201 * std::pow(randomUnit(seed), 1.9f);
        const float u = jmax(1.0e-6f, randomUnit(seed)), v = randomUnit(seed);
        p = { x, 52 + std::sqrt(-2 * std::log(u)) * std::cos(turn * v) * (5 + x * 0.17f), randomUnit(densitySeed) };
    }
    std::sort(points.begin(), points.end(), [](const CloudDot& a, const CloudDot& b) { return a.x > b.x; });
    return points;
}();
// press's squeeze profile per mode (punch glue slam warm smooth multi): two bumps, centre and width in px, the second's weight
constexpr float pressProfiles[6][5] { { 118, 38, 42, 14, 1.0f / 3 }, { 108, 70, 0, 1, 0 }, { 118, 30, 60, 30, 0.8f },
                                      { 130, 55, 50, 20, 0.5f }, { 108, 90, 0, 1, 0 }, { 50, 16, 108, 16, 1 } };
// the bottom strip: a hit's envelope at the left (x 6..62), the frequency axis at the right (x 70..210)
float hitEnvelope(float t) { return t < 0.08f ? t / 0.08f : std::exp(-(t - 0.08f) * 4.5f); }
float axisX(float hz) { return 70 + 4.0f * (float) roundToInt(35 * logNorm(hz, 20, 20000)); }
} // namespace

struct Visualizer::State
{
    struct Column
    {
        int dots = 0;
        bool active = false;
        bool operator==(const Column&) const = default;
    };
    // What a picture shows: what the audio did, and every one of the module's controls (a change redraws it still).
    struct Frame
    {
        float shift = 0, wobble = 0, phase = 0, extent = 0, predelay = 0, cloud = 0, squeeze = 0, drive = 0, swell = 0, push = 0;
        int mode = 0, threshold = 11, tab = 0, type = 0;
        bool dim = false, solo = false, post = false, acid = false, invert = false;
        float target = 0, lo = 20, hi = 20000;                          // shift, drive, push (space: lo, hi; sub/hiss: detect)
        float amt = 0, decay = 300, duck = 0, pitch = 0, stereo = 0, color = 0, env = 0, tone = 0, res = 0; // sub/hiss, space duck
        float balance = 0, stretch = 0, mix = 1, pan = 0, width = 1, downup = 0, bands = 0, attackMs = 10, amount = 0, flavor = 0;
        std::array<Column, columns> bands_ {};
        std::array<bool, 2> lit {};
        bool operator==(const Frame&) const = default;
    } frame;

    State(Module m, const SnapProcessor::Display& display)
        : amountSpec(spec(modules[(size_t) m].amountIds[0])),
          spectrum(m == Module::subHiss ? std::make_unique<Spectrum>(display) : nullptr)
    {
    }

    const ParamSpec& amountSpec;
    std::unique_ptr<Spectrum> spectrum;
    double lastTick = Time::getMillisecondCounterHiRes();
    float envelope = 0, second = 0; // followers of the module's levels
};

Visualizer::Visualizer(SnapProcessor& p, Module m) : proc(p), module(m), state(std::make_unique<State>(m, p.display))
{
    update({});
}

Visualizer::~Visualizer() = default;

void Visualizer::update(const Levels& levels)
{
    auto& s = *state;
    auto next = s.frame;
    next.dim = dim;
    const double now = Time::getMillisecondCounterHiRes();
    const float elapsed = (float) ((now - s.lastTick) * 0.001);
    s.lastTick = now;
    const auto raw = [&](const String& id) { return proc.apvts.getRawParameterValue(id)->load(std::memory_order_relaxed); };
    const float magnitude = 0.01f * raw("magnitude");
    const float amount = 0.01f * applyMagnitude(s.amountSpec, raw(s.amountSpec.id), magnitude);
    // Only what the audio does moves a picture: off, bypassed or silent, every level here is 0 and the frame settles.
    const bool live = !dim && levels.flowing;
    const auto audible = [](float v) { return v > 1.0e-4f ? v : 0.0f; }; // below -80 dB counts as silence
    const float level = live ? std::sqrt(unit(audible(levels.out))) : 0.0f;
    const auto follow = [&](float& envelope, float target, float releaseSeconds) {
        envelope = target >= envelope ? target : target + (envelope - target) * std::exp(-elapsed / releaseSeconds);
        if (envelope < 0.002f)
            envelope = 0;
        return envelope;
    };
    // the controls every picture shares: the target (-1 transients .. +1 sustain) and the focus band
    const auto strip = [&](const String& pre) { next.target = raw(pre + "_target"), next.lo = raw(pre + "_focus_lo"), next.hi = raw(pre + "_focus_hi"); };
    const auto& display = proc.display;
    switch (module)
    {
        case Module::subHiss:
        {
            auto& spectrum = *s.spectrum;
            const double rate = proc.getSampleRate();
            const bool active = !dim && magnitude > 0 && (amount > 0 || raw("hiss_amt") > 0);
            spectrum.read(display, active && levels.flowing, levels.in > 0, (float) (rate > 0 ? rate : 48000), elapsed);
            const String L = tab == 0 ? "sub" : "hiss";
            const float low = raw(L + "_det_lo"), high = raw(L + "_det_hi"), lo = jmin(low, high), hi = jmax(low, high);
            next.threshold = roundToInt(0.22f * raw(L + "_sens"));
            for (size_t i = 0; i < columns; ++i)
            {
                const int dots = jmax(1, roundToInt(spectrum.heights[i]));
                next.bands_[i] = { dots, dots > 1 && lo <= spectrum.centres[i] && spectrum.centres[i] <= hi };
            }
            for (size_t i = 0; i < next.lit.size(); ++i)
            {
                const int count = display.hits[i].load(std::memory_order_relaxed);
                if (count > spectrum.hits[i] && !dim)
                    spectrum.hitUntil[i] = now + 150.0;
                spectrum.hits[i] = count;
                next.lit[i] = now < spectrum.hitUntil[i];
            }
            next.tab = tab, next.lo = lo, next.hi = hi;
            next.amt = tab == 0 ? amount : 0.01f * raw("hiss_amt") * magnitude;
            next.decay = raw(L + "_decay"), next.duck = 0.01f * raw(L + "_duck"), next.solo = raw(L + "_solo") > 0.5f, next.post = raw(L + "_post") > 0.5f;
            next.pitch = raw(L + "_pitch"), next.stereo = 0.01f * raw("sub_stereo"), next.color = 0.01f * raw("sub_color"), next.acid = raw("sub_acid") > 0.5f;
            next.env = 0.01f * raw("hiss_env"), next.tone = 0.01f * raw("hiss_tone"), next.res = 0.01f * raw("hiss_res"), next.type = roundToInt(raw("hiss_type"));
            break;
        }
        case Module::shift:
            next.shift = amount;
            next.wobble = 3 * std::abs(amount) * level;
            next.phase = next.wobble > 0 ? std::fmod(next.phase + 0.19f * level, turn) : next.phase;
            next.balance = raw("shift_balance"), next.invert = raw("shift_invert") > 0.5f, next.stereo = 0.01f * raw("shift_stereo");
            next.stretch = 0.01f * raw("shift_stretch"), next.mix = 0.01f * raw("shift_fmix"), strip("shift");
            break;
        case Module::space:
        {
            // The cloud is the reverb itself: it blooms with the wet signal and fades with its tail; its settings shape it.
            const float wet = follow(s.envelope, live ? std::sqrt(unit(audible(levels.spaceWet))) : 0.0f, 0.08f);
            next.cloud = unit(0.12f + 0.3f * amount + 1.8f * wet);
            next.extent = (0.3f + 0.7f * 0.01f * raw("space_decay")) * (0.55f + 0.45f * (float) roundToInt(raw("space_type")) / 19);
            next.predelay = 0.064f * raw("space_predelay");
            next.stretch = 0.01f * raw("space_stretch"), next.pan = 0.01f * raw("space_pan"), next.width = 0.01f * raw("space_width");
            next.duck = 0.01f * raw("space_duck"), next.lo = raw("space_focus_lo"), next.hi = raw("space_focus_hi");
            break;
        }
        case Module::press:
            next.squeeze = jmin(0.95f, 0.15f + 0.35f * amount
                                           + (live ? 0.85f * std::sqrt(amount * unit(-display.pressDownDb.load(std::memory_order_relaxed) / 24)) : 0.0f));
            next.mode = roundToInt(raw("press_mode"));
            next.downup = raw("press_downup"), next.mix = 0.01f * raw("press_mix"), next.bands = raw("press_bands");
            next.attackMs = Press::attackMs(next.mode, raw("press_attack"));
            break;
        case Module::drive:
        {
            // The wave swells with the drive's input and thickens with what the saturation adds to it.
            next.mode = roundToInt(raw("drive_mode"));
            const float in = follow(s.envelope, live ? std::sqrt(unit(audible(levels.driveIn))) : 0.0f, 0.12f);
            const float added = follow(s.second, live ? std::sqrt(unit(audible(levels.driveAdded))) : 0.0f, 0.12f);
            next.swell = 0.35f + 0.65f * in;
            next.drive = unit(0.3f * amount + (in > 0 ? 2.5f * added / jmax(in, 0.05f) : 0.0f));
            next.phase = in > 0 ? std::fmod(next.phase + 0.075f * in, 1.0f) : next.phase;
            next.amount = amount, next.flavor = raw("drive_flavor"), next.mix = 0.01f * raw("drive_mix"), strip("drive");
            break;
        }
        case Module::push:
        {
            const float in = follow(s.envelope, live ? std::sqrt(unit(audible(levels.pushIn))) : 0.0f, 0.12f);
            const float added = follow(s.second, live ? std::sqrt(unit(audible(levels.pushAdded))) : 0.0f, 0.12f);
            next.push = unit(0.15f + 0.5f * amount + (in > 0 ? std::sqrt(amount) * (0.4f * in + 1.6f * added) : 0.0f));
            next.phase = in > 0 ? std::fmod(next.phase + 0.06f * in, turn) : next.phase;
            next.mode = roundToInt(raw("push_mult"));
            next.tone = raw("push_tone"), next.mix = 0.01f * raw("push_mix"), strip("push");
            break;
        }
    }
    if (next != s.frame)
    {
        s.frame = next;
        repaint();
    }
}

void Visualizer::paint(Graphics& g)
{
    const Lattice lattice(*this);
    {
        const Graphics::ScopedSaveState save(g); // the printed texture first, so it shows only between the dots
        g.setOpacity(riso::textureOpacity), g.setImageResamplingQuality(Graphics::lowResamplingQuality);
        g.drawImage(riso::texture(module), getLocalBounds().toFloat());
    }
    gridDots(g, 217, 104);
    const auto& f = state->frame;
    const auto shade = [&](Colour c) { return dimmed(c, dim); };
    Braille b(g);
    const auto drawDot = [&](float x, float y, Colour c) { b.dot(x, y, shade(c)); };
    // the hit's envelope, lit where the module acts: target -1 the transient only, 0 all of it, +1 the sustain only
    const auto envelope = [&](auto lit) {
        for (int k = 0; k <= 14; ++k)
        {
            const float t = (float) k / 14;
            drawDot(6 + 4.0f * (float) k, 100 - 12 * hitEnvelope(t), lit(t) ? accent : colours::dim);
        }
    };
    const auto targeted = [](float v) { return [v](float t) { return v <= 0 ? t <= 0.15f + 0.85f * (1 + v) : t >= 0.15f * v; }; };
    // the focus band on a 20 Hz .. 20 kHz axis, its two ends marked
    const auto axis = [&](float lo, float hi) {
        const float a = axisX(jmin(lo, hi)), z = axisX(jmax(lo, hi));
        for (float x = 70; x <= 210; x += 4)
            drawDot(x, 100, x >= a && x <= z ? accent : colours::dim);
        drawDot(a, 96, primary), drawDot(z, 96, primary);
    };
    switch (module)
    {
        case Module::subHiss:
        {
            vmeter(g, 1, 6, 23, (float) (23 - f.threshold) / 23, false, accent, dim);
            for (float x : { 1.0f, 4.0f, 7.0f })
                drawDot(x, 8 + 4.0f * (float) f.threshold, primary);
            for (size_t i = 0; i < columns; ++i)
            {
                const auto [dots, active] = f.bands_[i];
                const Colour body = f.solo ? colours::dim : active ? accent : i > 18 ? secondary : muted;
                for (int k = 0; k < dots; ++k)
                    drawDot(20 + 4.0f * (float) i, 92 - 4.0f * (float) k, k + 1 == dots && !f.solo ? primary : body);
            }
            // the detect band under the spectrum; the generated layer's pitch on it
            const auto& centres = state->spectrum->centres;
            const auto col = [](float hz) { return 20 + 4.0f * (float) roundToInt(33 * logNorm(hz, 20, 20000)); };
            for (size_t i = 0; i < columns; ++i)
                drawDot(20 + 4.0f * (float) i, 100, centres[i] >= f.lo && centres[i] <= f.hi ? accent : colours::dim);
            if (f.tab == 0)
            {
                const float hz = 440 * std::pow(2.0f, (f.pitch - 69) / 12);
                for (int h = 2; h <= 1 + roundToInt(4 * f.color); ++h)
                    drawDot(col(hz * (float) h), 100, secondary);
                drawDot(col(hz), 100, primary);
                if (f.acid)
                    drawDot(col(hz) + 4, 96, primary);
            }
            else
            {
                const float c = col(5000 * std::pow(2.0f, f.pitch / 12));
                const int w = roundToInt(1 + 4 * (1 - f.res)), tilt = roundToInt(2 * f.tone);
                for (int d = -w + jmin(0, tilt); d <= w + jmax(0, tilt); ++d) // the type raises its own few dots
                    drawDot(c + 4.0f * (float) d, (f.type * 7 + (d + 8) * 3) % 5 == 0 && d != 0 ? 96.0f : 100.0f, primary);
            }
            // the layer: its envelope (amount, decay, hiss env), sub stereo doubles it, duck dips the line above, post marks its end
            const float dn = logNorm(f.decay, 20, 2000), height = 0.5f + 0.5f * f.amt;
            const auto env = [&](float t) {
                const float e = std::exp(-t * (1.5f + 10 * (1 - dn)));
                return f.tab == 0 ? e : std::lerp(1.0f, e, f.env);
            };
            const float spread = f.tab == 0 ? 4.0f * (float) roundToInt(2 * f.stereo) : 0.0f;
            for (int k = 0; k <= 10; ++k)
            {
                const float t = (float) k / 10, x = 162 + 4.0f * (float) k;
                drawDot(x, 62 + 24 * f.duck * std::exp(-t * 6), muted);
                if (spread != 0)
                    drawDot(x + spread, 96 - 32 * height * env(t), secondary);
                drawDot(x, 96 - 32 * height * env(t), accent);
            }
            if (f.post)
                for (auto [x, y] : { std::pair { 206.0f, 86.0f }, { 210.0f, 90.0f }, { 206.0f, 94.0f } })
                    drawDot(x, y, primary);
            for (size_t i = 0; i < f.lit.size(); ++i)
            {
                const float y = 8 + 26.0f * (float) i;
                box(g, { 162, y, 50, 20 }, shade(rule));
                square(g, 168, y + 6, f.lit[i], accent, dim);
                riso::use(g, shade(f.lit[i] ? primary : muted));
                g.fillPath(state->spectrum->labels[i]);
            }
            break;
        }
        case Module::shift:
        {
            for (int i = 0; i < 18; ++i)
                for (float y = 10; y <= 78; y += 4)
                    drawDot(8 + 12.0f * (float) i, y, colours::dim);
            // the lower half follows the pitch (freq) part of the shift, the upper half the formant part; balance moves the
            // shift between them and lights the half it works on
            const float a = f.shift, bal = f.balance, freq = a * (1 - jmax(0.0f, bal)) * (f.invert ? -1.0f : 1.0f), form = a * (1 + jmin(0.0f, bal));
            const auto weightColour = [](float w) { return w > 0.66f ? primary : w > 0.33f ? secondary : w > 0 ? muted : colours::dim; };
            const float stretch = std::pow(2.0f, -0.6f * f.stretch), reach = 34 * f.mix;
            for (int i = 0; i < 18; ++i)
            {
                const float t = std::pow((float) i / 17, stretch);
                const auto place = [t](float sh) {
                    const float curve = 1 + 0.45f * std::abs(sh);
                    return 8 + 204 * (sh < 0 ? std::pow(t, curve) : 1 - std::pow(1 - t, curve));
                };
                const bool accented = i == (f.invert ? 13 : 4);
                int j = 0;
                for (float y = 10; y <= 78; y += 4, ++j)
                {
                    if (std::abs(y - 44) > reach)
                        continue;
                    const bool upper = y < 44;
                    const float x = place(upper ? form : freq) + (j % 2 ? 1.0f : -1.0f) * 3 * f.stereo;
                    drawDot(x + f.wobble * std::sin(0.9f * (float) i + y / 14 + f.phase), y,
                            accented ? accent : weightColour(upper ? 1 + jmin(0.0f, bal) : 1 - jmax(0.0f, bal)));
                }
            }
            envelope(targeted(f.target)), axis(f.lo, f.hi);
            break;
        }
        case Module::space:
        {
            for (float y = 8; y <= 80; y += 4)
                drawDot(6, y, primary);
            // time runs right (predelay, decay and the room's size), the stereo field down (pan moves it, width spreads it)
            const float scale = f.extent * (1 - f.predelay / 201), warp = std::pow(2.0f, -0.7f * f.stretch), spreadY = 0.8f * (0.25f + 0.75f * (f.width + 1) / 2);
            for (auto [x, y, rank] : cloudDots)
            {
                if (rank >= f.cloud)
                    continue;
                const float yy = 44 + 18 * f.pan + (y - 52) * spreadY;
                if (yy < 6 || yy > 84)
                    continue;
                const Colour colour = x < 30 ? accent : x < 85 ? primary : x < 145 ? secondary : muted;
                b.dot(10 + f.predelay + 201 * std::pow((x - 10) / 201, warp) * scale, yy, shade(colour).withAlpha(0.45f + 0.55f * f.cloud));
            }
            // duck: the tail dips under each hit
            for (int k = 0; k <= 14; ++k)
            {
                const float t = (float) k / 14;
                drawDot(6 + 4.0f * (float) k, 88 + 12 * f.duck * hitEnvelope(t), secondary);
            }
            axis(f.lo, f.hi);
            break;
        }
        case Module::press:
        {
            const auto& q = pressProfiles[(size_t) jlimit(0, 5, f.mode)];
            const float down = (1 - f.downup) / 2, up = (1 + f.downup) / 2;
            for (float x = 6; x <= 211; x += 4)
            {
                const float first = (x - q[0]) / q[1], second = (x - q[2]) / q[3], third = (x - 166) / 16;
                const float band = x < 76 ? 0.5f : x < 146 ? 1.0f : 1.5f;
                const float shape = jmin(1.0f, std::exp(-first * first) + q[4] * std::exp(-second * second) + (f.mode == 5 ? std::exp(-third * third) : 0.0f))
                                  * std::lerp(1.0f, band, f.bands);
                const float squeeze = jmin(0.95f, f.squeeze * shape);
                for (int i = -4; i <= 4; ++i)
                {
                    const int distance = std::abs(i);
                    const bool red = squeeze > 0.22f && distance >= 3;
                    const float wet = 44 + 9.0f * (float) i * (1 - squeeze * down + 0.3f * f.squeeze * (1 - shape) * up);
                    drawDot(x, std::lerp(44 + 9.0f * (float) i, wet, f.mix), red ? accent : distance == 0 ? primary : distance <= 2 ? secondary : muted);
                }
            }
            // attack: press acts from its attack on (slow lets more of the hit through)
            const float onset = unit(f.attackMs / 120) * 0.6f;
            envelope([onset](float t) { return t >= onset; });
            break;
        }
        case Module::drive:
            for (float x = 6; x <= 211; x += 3)
            {
                const float t = x / 28 + f.phase, tri = 4 * std::abs(t - std::floor(t) - 0.5f) - 1;
                float wave = tri;
                switch (f.mode)
                {
                    case 1: wave = std::lerp(tri, std::cos(turn * t), 0.4f); break;
                    case 2: wave = jlimit(-1.0f, 1.0f, 1.25f * tri); break;
                    case 3: wave = tri * (0.65f + 0.35f * std::abs(tri)); break;
                    case 4: wave = std::round(tri * 3) / 3; break;
                    case 5: wave = std::lerp(tri, std::cos(2 * turn * t), 0.3f); break;
                }
                // drive squares it, flavor adds a ripple (dark smooth, open bright), mix blends it with the dry wave
                wave = jlimit(-1.0f, 1.0f, wave * (1 + 2 * f.amount)) + 0.09f * (f.flavor + 1) * std::sin(4 * turn * t);
                drawDot(x, 44 + 28 * f.swell * tri, colours::dim);
                const float y = 44 + 28 * f.swell * std::lerp(tri, wave, f.mix);
                const int layers = 1 + jmin(4, (int) (5 * f.drive * x / 211));
                for (int k = layers - 1; k >= 0; --k)
                    drawDot(x, y + 3.0f * (float) k - 1.5f * (float) (layers - 1), k == 0 ? primary : accent);
            }
            envelope(targeted(f.target)), axis(f.lo, f.hi);
            break;
        case Module::push:
        {
            const auto loopDot = [&](float angle, float r, float aspect, Colour c) { drawDot(108 + r * std::cos(angle), 44 + aspect * r * std::sin(angle), c); };
            for (int i = 0; i < 56; ++i)
                loopDot(turn * (float) i / 56, 30, 1, colours::dim);
            // stages (1x 2x 4x) set the lobes, tone the inner loop's size and turn, mix how far the loop leaves the dry circle
            std::uint32_t seed = 5;
            const float lobes = (float) (3 + 2 * jlimit(0, 2, f.mode)), inner = 0.55f + 0.15f * f.tone;
            for (int i = 0; i < 80; ++i)
            {
                const float t = turn * (float) i / 80, angle = t + f.phase;
                const float r = 30 * (1 + f.mix * f.push * (0.2f * std::sin(lobes * t + 0.6f) + 0.1f * std::sin((lobes + 2) * t) + 0.04f * randomUnit(seed)));
                loopDot(angle + 0.3f + 0.5f * f.tone, r * inner, 0.95f, muted);
                loopDot(angle, r, 0.95f, r > 34.5f ? accent : primary);
            }
            envelope(targeted(f.target)), axis(f.lo, f.hi);
            break;
        }
    }
}
} // namespace snap::ui
