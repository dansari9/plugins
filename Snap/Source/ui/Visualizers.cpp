#include "Visualizers.h"
#include "Braille.h"
#include "Riso.h"
#include "../PluginProcessor.h"
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
} // namespace

struct Visualizer::State
{
    struct Column
    {
        int dots = 0;
        bool active = false;
        bool operator==(const Column&) const = default;
    };
    struct Frame
    {
        float shift = 0, wobble = 0, phase = 0, extent = 0, predelay = 0, cloud = 0, squeeze = 0, drive = 0, swell = 0, push = 0;
        int mode = 0, threshold = 11;
        bool dim = false;
        std::array<Column, columns> bands {};
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
    const auto raw = [&](const char* id) { return proc.apvts.getRawParameterValue(id)->load(std::memory_order_relaxed); };
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
    const auto& display = proc.display;
    switch (module)
    {
        case Module::subHiss:
        {
            auto& spectrum = *s.spectrum;
            const double rate = proc.getSampleRate();
            const bool active = !dim && magnitude > 0 && (amount > 0 || raw("hiss_amt") > 0);
            spectrum.read(display, active && levels.flowing, levels.in > 0, (float) (rate > 0 ? rate : 48000), elapsed);
            const float low = raw(tab == 0 ? "sub_det_lo" : "hiss_det_lo"), high = raw(tab == 0 ? "sub_det_hi" : "hiss_det_hi");
            const auto [lo, hi] = std::minmax(low, high);
            next.threshold = roundToInt(0.22f * raw(tab == 0 ? "sub_sens" : "hiss_sens"));
            for (size_t i = 0; i < columns; ++i)
            {
                const int dots = jmax(1, roundToInt(spectrum.heights[i]));
                next.bands[i] = { dots, dots > 1 && lo <= spectrum.centres[i] && spectrum.centres[i] <= hi };
            }
            for (size_t i = 0; i < next.lit.size(); ++i)
            {
                const int count = display.hits[i].load(std::memory_order_relaxed);
                if (count > spectrum.hits[i] && !dim)
                    spectrum.hitUntil[i] = now + 150.0;
                spectrum.hits[i] = count;
                next.lit[i] = now < spectrum.hitUntil[i];
            }
            break;
        }
        case Module::shift:
            next.shift = amount;
            next.wobble = 3 * std::abs(amount) * level;
            next.phase = next.wobble > 0 ? std::fmod(next.phase + 0.19f * level, turn) : next.phase;
            break;
        case Module::space:
        {
            // The cloud is the reverb itself: it blooms with the wet signal and fades with its tail.
            const float decay = 0.01f * raw("space_decay");
            const float wet = follow(s.envelope, live ? std::sqrt(unit(audible(levels.spaceWet))) : 0.0f, 0.08f);
            next.cloud = unit(1.8f * wet);
            next.extent = next.cloud > 0 ? 0.30f + 0.70f * decay : 0.0f;
            next.predelay = next.cloud > 0 ? 0.064f * raw("space_predelay") : 0.0f;
            break;
        }
        case Module::press:
            next.squeeze = live ? 0.85f * std::sqrt(amount * unit(-display.pressDownDb.load(std::memory_order_relaxed) / 24)) : 0.0f;
            break;
        case Module::drive:
        {
            // The wave swells with the drive's input and thickens with what the saturation adds to it.
            next.mode = (int) raw("drive_mode");
            const float in = follow(s.envelope, live ? std::sqrt(unit(audible(levels.driveIn))) : 0.0f, 0.12f);
            const float added = follow(s.second, live ? std::sqrt(unit(audible(levels.driveAdded))) : 0.0f, 0.12f);
            next.swell = in;
            next.drive = in > 0 ? unit(2.5f * added / jmax(in, 0.05f)) : 0.0f;
            next.phase = in > 0 ? std::fmod(next.phase + 0.075f * in, 1.0f) : next.phase;
            break;
        }
        case Module::push:
        {
            const float in = follow(s.envelope, live ? std::sqrt(unit(audible(levels.pushIn))) : 0.0f, 0.12f);
            const float added = follow(s.second, live ? std::sqrt(unit(audible(levels.pushAdded))) : 0.0f, 0.12f);
            next.push = in > 0 ? unit(std::sqrt(amount) * (0.4f * in + 1.6f * added)) : 0.0f;
            next.phase = next.push > 0 ? std::fmod(next.phase + 0.06f * in, turn) : next.phase;
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
    switch (module)
    {
        case Module::subHiss:
        {
            vmeter(g, 1, 6, 23, (float) (23 - f.threshold) / 23, false, accent, dim);
            for (float x : { 1.0f, 4.0f, 7.0f })
                drawDot(x, 8 + 4.0f * (float) f.threshold, primary);
            for (size_t i = 0; i < columns; ++i)
            {
                const auto [dots, active] = f.bands[i];
                const Colour body = active ? accent : i > 18 ? secondary : muted;
                for (int k = 0; k < dots; ++k)
                    drawDot(20 + 4.0f * (float) i, 96 - 4.0f * (float) k, k + 1 == dots ? primary : body);
            }
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
                for (float y = 10; y <= 94; y += 4)
                    drawDot(8 + 12.0f * (float) i, y, colours::dim);
            const float curve = 1 + 0.45f * std::abs(f.shift);
            for (int i = 0; i < 18; ++i)
            {
                const float t = (float) i / 17;
                const float shifted = f.shift < 0 ? std::pow(t, curve) : 1 - std::pow(1 - t, curve);
                const float x = 8 + (f.shift == 0 ? 12.0f * (float) i : 204 * shifted);
                for (float y = 10; y <= 94; y += 4)
                    drawDot(x, y + f.wobble * std::sin(0.9f * (float) i + y / 14 + f.phase), i == 4 ? accent : primary);
            }
            break;
        }
        case Module::space:
        {
            for (float y = 12; y <= 92; y += 4)
                drawDot(6, y, primary);
            const float scale = f.extent * (1 - f.predelay / 201);
            for (auto [x, y, rank] : cloudDots)
            {
                if (rank >= f.cloud || y < 6 || y > 98)
                    continue;
                const Colour colour = x < 30 ? accent : x < 85 ? primary : x < 145 ? secondary : muted;
                b.dot(10 + f.predelay + (x - 10) * scale, y, shade(colour).withAlpha(0.45f + 0.55f * f.cloud));
            }
            break;
        }
        case Module::press:
            for (float x = 6; x <= 211; x += 4)
            {
                const float first = (x - 118) / 38, second = (x - 42) / 14;
                const float squeeze = f.squeeze * (std::exp(-first * first) + std::exp(-second * second) / 3);
                for (int i = -4; i <= 4; ++i)
                {
                    const int distance = std::abs(i);
                    const bool red = squeeze > 0.22f && distance >= 3;
                    drawDot(x, 52 + 10.0f * (float) i * (1 - squeeze), red ? accent : distance == 0 ? primary : distance <= 2 ? secondary : muted);
                }
            }
            break;
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
                const int layers = 1 + jmin(4, (int) (5 * f.drive * x / 211));
                for (int k = layers - 1; k >= 0; --k)
                    drawDot(x, 52 + 30 * f.swell * wave + 3.0f * (float) k - 1.5f * (float) (layers - 1), k == 0 ? primary : accent);
            }
            break;
        case Module::push:
        {
            const auto loopDot = [&](float angle, float r, float aspect, Colour c) { drawDot(108 + r * std::cos(angle), 52 + aspect * r * std::sin(angle), c); };
            for (int i = 0; i < 56; ++i)
                loopDot(turn * (float) i / 56, 34, 1, colours::dim);
            std::uint32_t seed = 5;
            for (int i = 0; i < 80; ++i)
            {
                const float t = turn * (float) i / 80, angle = t + f.phase;
                const float r = 34 * (1 + f.push * (0.2f * std::sin(3 * t + 0.6f) + 0.1f * std::sin(5 * t) + 0.04f * randomUnit(seed)));
                loopDot(angle + 0.3f, r * 0.55f, 0.95f, muted);
                loopDot(angle, r, 0.95f, r > 39 ? accent : primary);
            }
            break;
        }
    }
}
} // namespace snap::ui
