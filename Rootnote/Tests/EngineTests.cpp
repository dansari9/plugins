#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/Engine.h"

#include <string>
#include <vector>

using namespace rootnote;
using namespace testsignals;

namespace
{
using Signal = std::vector<float>;

Signal sine(double hz, double fs, size_t length, double amplitude)
{
    Signal x(length);
    for (size_t i = 0; i < length; ++i)
        x[i] = static_cast<float>(amplitude * std::sin(kTwoPi * hz * static_cast<double>(i) / fs));
    return x;
}

Signal noise(size_t length, double amplitude, uint32_t seed)
{
    Random random(seed);
    Signal x(length);
    for (auto& sample : x)
        sample = static_cast<float>(amplitude * random.bipolar());
    return x;
}

/** Processes [from, to) of a mono or stereo signal in place, in host blocks. */
void run(Engine& engine, Signal& left, Signal* right, size_t from, size_t to, int blockSize = 256)
{
    for (size_t position = from; position < to; position += static_cast<size_t>(blockSize))
    {
        const int n = static_cast<int>(std::min(static_cast<size_t>(blockSize), to - position));
        float* channels[2] = { left.data() + position, right != nullptr ? right->data() + position : nullptr };
        engine.process(channels, right != nullptr ? 2 : 1, n);
    }
}

Signal processed(const EngineParams& params, const Signal& input, double fs, int blockSize = 256)
{
    Engine engine;
    engine.prepare(fs, 512);
    engine.setParams(params);
    Signal out = input;
    run(engine, out, nullptr, 0, out.size(), blockSize);
    return out;
}

double peak(const Signal& x, size_t from, size_t to)
{
    double p = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i)
        p = std::max(p, std::abs(static_cast<double>(x[i])));
    return p;
}

/** Largest sample-to-sample step in [from, to). */
double largestStep(const Signal& x, size_t from, size_t to)
{
    double step = 0.0;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < x.size(); ++i)
        step = std::max(step, std::abs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1])));
    return step;
}

bool allFinite(const Signal& x)
{
    for (float sample : x)
        if (!std::isfinite(sample))
            return false;
    return true;
}

double amplitudeAt(const Signal& x, size_t from, size_t to, double hz, double fs)
{
    return toneAmplitude(std::vector<double>(x.begin(), x.end()), from, to, hz, fs);
}

/** Everything that makes sound, with the limiter off so the levels are exact. */
EngineParams neutral()
{
    EngineParams p;
    p.boostGainDb = 0.0;
    p.limiter = false;
    return p;
}

/** Every stage at its most extreme setting. */
EngineParams extreme()
{
    EngineParams p;
    p.boostGainDb = 24.0;
    p.boostQ = 200.0;
    p.bands = 32;
    p.rangeOct = 6.0;
    p.tiltDbPerOct = 6.0;
    p.cutOn = true;
    p.cutNote = 31.0;
    p.cutDepthDb = -30.0;
    p.cutQ = 60.0;
    p.cutBands = 16;
    p.cutOvertones = 1.0;
    p.ring = 1.0;
    p.ringDecaySeconds = 4.0;
    p.ringDropSemitones = 12.0;
    p.outputDb = 12.0;
    return p;
}
} // namespace

TEST_CASE("Engine: with nothing switched on the output is the input, bit for bit")
{
    const double fs = 48000.0;
    const Signal input = noise(20000, 0.5, 3);

    EngineParams limited = neutral();
    limited.limiter = true; // 0.5 is under the limiter threshold
    CHECK(processed(neutral(), input, fs) == input);
    CHECK(processed(limited, input, fs) == input);

    EngineParams dry;
    dry.boostGainDb = 18.0;
    dry.mix = 0.0;
    dry.limiter = false;
    CHECK(processed(dry, input, fs) == input);
}

TEST_CASE("Engine: zero latency")
{
    EngineParams params;
    params.limiter = true;
    Signal impulse(256, 0.0f);
    impulse[100] = 0.5f;
    const Signal out = processed(params, impulse, 48000.0);
    CHECK(peak(out, 0, 100) == 0.0);
    CHECK(out[100] != 0.0f);
}

TEST_CASE("Engine: boost, mix and output gain reach the levels they are set to")
{
    const double fs = 48000.0;
    const double rootHz = noteToHz(36.0);
    const size_t length = 96000;
    const Signal input = sine(rootHz, fs, length, 0.05);
    const auto gainDb = [&](const EngineParams& p) {
        return gainToDb(amplitudeAt(processed(p, input, fs), length / 2, length, rootHz, fs) / 0.05);
    };

    // One band, so the gain at the root is the Boost value itself.
    EngineParams p;
    p.bands = 1;
    p.boostGainDb = 12.0;
    p.limiter = false;
    CHECK_NEAR(gainDb(p), 12.0, 0.1);

    // A bell has no phase shift at its centre, so dry and wet add in phase.
    p.mix = 0.5;
    CHECK_NEAR(gainDb(p), gainToDb(0.5 + 0.5 * dbToGain(12.0)), 0.1);

    p.mix = 1.0;
    p.outputDb = -9.0;
    CHECK_NEAR(gainDb(p), 3.0, 0.1);
}

TEST_CASE("Engine: the cut stage removes the root and scales the overtones")
{
    const double fs = 48000.0;
    const size_t length = 96000;
    EngineParams p = neutral();
    p.cutOn = true;
    p.cutNote = 45.0; // 110 Hz
    p.cutDepthDb = -12.0;
    p.cutQ = 30.0;
    p.cutOvertones = 0.5;

    for (int harmonic : { 1, 3 })
    {
        const double hz = 110.0 * harmonic;
        const Signal out = processed(p, sine(hz, fs, length, 0.25), fs);
        CHECK_NEAR(gainToDb(amplitudeAt(out, length / 2, length, hz, fs) / 0.25), harmonic == 1 ? -12.0 : -6.0, 0.15);
    }

    p.cutOn = false;
    const Signal input = sine(110.0, fs, 4800, 0.25);
    CHECK(processed(p, input, fs) == input);
}

TEST_CASE("Engine: bypass crossfades in 20 ms, then passes the input untouched")
{
    const double fs = 48000.0;
    const size_t length = 48000;
    const size_t switchAt = 24000;
    const size_t fade = static_cast<size_t>(Engine::kLevelRampSeconds * fs);
    const Signal input = sine(noteToHz(36.0), fs, length, 0.1);

    EngineParams p;
    p.boostGainDb = 18.0;
    p.limiter = false;

    Engine engine;
    engine.prepare(fs, 512);
    engine.setParams(p);
    Signal out = input;
    run(engine, out, nullptr, 0, switchAt);
    const double stepBefore = largestStep(out, switchAt - 4800, switchAt);
    CHECK(peak(out, switchAt - 4800, switchAt) > 0.3);

    p.bypass = true;
    engine.setParams(p);
    run(engine, out, nullptr, switchAt, length);

    // The fade moves between two in-phase sines, so no step can be larger than before it.
    CHECK(largestStep(out, switchAt - 1, switchAt + fade + 1) <= stepBefore * 1.001);
    bool dry = true;
    for (size_t i = switchAt + fade; i < length; ++i)
        dry = dry && out[i] == input[i];
    CHECK(dry);

    // Coming back restarts from a clean state: no stale ringing, and the same fade length.
    Signal again = input;
    p.bypass = false;
    engine.setParams(p);
    run(engine, again, nullptr, 0, length);
    CHECK(again[0] == input[0]);
    CHECK(largestStep(again, 0, 2 * fade) <= stepBefore * 1.001);
    CHECK_NEAR(peak(again, switchAt - 4800, switchAt), peak(out, switchAt - 4800, switchAt), 0.01);
}

TEST_CASE("Engine: a bypassed engine started cold is transparent from the first sample")
{
    EngineParams p = extreme();
    p.bypass = true;
    const Signal input = noise(4096, 1.0, 9);
    CHECK(processed(p, input, 44100.0) == input);
}

TEST_CASE("Engine: mix, output and limiter switches ramp without a step")
{
    const double fs = 48000.0;
    const Signal input = sine(100.0, fs, 9600, 0.5);
    const double inputStep = largestStep(input, 0, input.size());

    EngineParams from = neutral();
    EngineParams to = neutral();
    to.outputDb = -24.0;

    Engine engine;
    engine.prepare(fs, 512);
    engine.setParams(from);
    Signal out = input;
    run(engine, out, nullptr, 0, 4800);
    engine.setParams(to);
    run(engine, out, nullptr, 4800, 9600);

    CHECK(largestStep(out, 0, out.size()) <= inputStep * 1.01);
    CHECK_NEAR(peak(out, 4800 + 960 + 480, 9600), 0.5 * dbToGain(-24.0), 1.0e-4);
    // Halfway through the 20 ms ramp the gain is halfway, linearly.
    CHECK_NEAR(std::abs(out[4800 + 480 - 1]) / std::abs(input[4800 + 480 - 1]), 0.5 * (1.0 + dbToGain(-24.0)), 1.0e-3);
}

TEST_CASE("Engine: stays finite and under the ceiling with every stage at its extreme")
{
    for (double fs : { 44100.0, 192000.0 })
    {
        const size_t tenSeconds = static_cast<size_t>(10.0 * fs);
        const std::string rate = std::to_string(static_cast<int>(fs));

        Engine engine;
        engine.prepare(fs, 512);
        engine.setParams(extreme());

        // +6 dBFS white noise, different on the two sides.
        Signal left = noise(tenSeconds, 2.0, 11);
        Signal right = noise(tenSeconds, 2.0, 12);
        run(engine, left, &right, 0, tenSeconds);
        CHECK_MSG(allFinite(left) && allFinite(right), "noise " + rate);
        CHECK_MSG(peak(left, 0, tenSeconds) <= Limiter::kCeiling, "noise " + rate);
        CHECK_MSG(peak(right, 0, tenSeconds) <= Limiter::kCeiling, "noise " + rate);

        // Silence right after it: the tail decays instead of blowing up.
        Signal silence(tenSeconds, 0.0f);
        run(engine, silence, nullptr, 0, tenSeconds);
        CHECK_MSG(allFinite(silence), "silence " + rate);
        CHECK_MSG(peak(silence, tenSeconds - 1000, tenSeconds) < peak(silence, 0, 1000), "silence " + rate);

        Signal dc(static_cast<size_t>(fs), 2.0f);
        run(engine, dc, nullptr, 0, dc.size());
        CHECK_MSG(allFinite(dc) && peak(dc, 0, dc.size()) <= Limiter::kCeiling, "DC " + rate);

        // Full-scale impulses of alternating sign, 50 per second.
        Signal impulses(static_cast<size_t>(2.0 * fs), 0.0f);
        for (size_t i = 0, n = 0; i < impulses.size(); i += static_cast<size_t>(fs / 50.0), ++n)
            impulses[i] = (n % 2) == 0 ? 2.0f : -2.0f;
        run(engine, impulses, nullptr, 0, impulses.size());
        CHECK_MSG(allFinite(impulses) && peak(impulses, 0, impulses.size()) <= Limiter::kCeiling, "impulses " + rate);
    }
}

TEST_CASE("Engine: the limiter is what holds the ceiling")
{
    const double fs = 48000.0;
    const Signal input = sine(noteToHz(36.0), fs, 48000, 0.5);
    EngineParams p;
    p.boostGainDb = 24.0;

    Engine engine;
    engine.prepare(fs, 512);
    engine.setParams(p);
    Signal out = input;
    run(engine, out, nullptr, 0, out.size());
    CHECK(peak(out, 0, out.size()) <= Limiter::kCeiling);
    CHECK(peak(out, 24000, 48000) > 0.8);
    CHECK(engine.limiterGain() < 0.5);

    p.limiter = false;
    CHECK(peak(processed(p, input, fs), 24000, 48000) > 4.0);
}

TEST_CASE("Engine: a two-octave MIDI jump every 10 ms does not click")
{
    const double fs = 48000.0;
    const size_t length = 96000;
    const size_t settle = 24000;
    const Signal input = sine(110.0, fs, length, 0.1);

    EngineParams p;
    p.boostGainDb = 12.0;
    p.boostQ = 12.0;
    p.limiter = false;

    // Reference: every note of the jump held still. The moving filter passes
    // through all of these responses (the input sits on note 45), so its
    // steepest slope should stay close to the steepest of theirs.
    double staticStep = 0.0;
    for (double note = 33.0; note <= 57.0; note += 1.0)
    {
        EngineParams still = p;
        still.boostNote = note;
        staticStep = std::max(staticStep, largestStep(processed(still, input, fs), settle, length));
    }

    for (double glide : { 0.0, 0.004, 0.050 })
    {
        p.glideSeconds = glide;
        Engine engine;
        engine.prepare(fs, 512);
        engine.setParams(p);

        Signal out = input;
        const size_t every = 480;
        for (size_t position = 0, n = 0; position < length; position += every, ++n)
        {
            engine.handleMidi({ MidiEvent::Type::noteOn, 1, (n % 2) == 0 ? 33 : 57 });
            run(engine, out, nullptr, position, position + every, 64);
        }

        const double moving = largestStep(out, settle, length);
        CHECK(allFinite(out));
        CHECK_MSG(moving <= staticStep * 1.25, "glide " + std::to_string(glide) + ": step " + std::to_string(moving)
                                                   + " against " + std::to_string(staticStep));
    }
}

TEST_CASE("Engine: output does not depend on the host block size")
{
    const double fs = 48000.0;
    Signal input(24000, 0.0f);
    Random random(31);
    for (size_t start : { size_t(100), size_t(9000), size_t(17000) })
        addKick(input, start, KickSpec {}, fs, random);

    EngineParams p = extreme();
    p.boostQ = 30.0;
    p.ringDecaySeconds = 0.4;

    Signal reference;
    for (int blockSize : { 512, 1, 7, 64, 300 })
    {
        Engine engine;
        engine.prepare(fs, 512);
        engine.setParams(p);
        Signal left = input;
        Signal right = input;
        run(engine, left, &right, 0, 5000, blockSize);
        engine.handleMidi({ MidiEvent::Type::noteOn, 1, 43 });
        run(engine, left, &right, 5000, input.size(), blockSize);

        CHECK(left == right);
        if (reference.empty())
            reference = left;
        else
            CHECK_MSG(left == reference, "block size " + std::to_string(blockSize));
    }
}

TEST_CASE("Engine: mono is the left channel of a stereo run")
{
    const double fs = 44100.0;
    const Signal input = noise(20000, 0.3, 17);
    EngineParams p = extreme();

    Engine mono, stereo;
    mono.prepare(fs, 512);
    stereo.prepare(fs, 512);
    mono.setParams(p);
    stereo.setParams(p);

    Signal m = input, l = input, r = input;
    run(mono, m, nullptr, 0, m.size());
    run(stereo, l, &r, 0, l.size());
    CHECK(m == l);
}

TEST_CASE("Engine: the ring adds a tail to a hit, and only when it is turned up")
{
    const double fs = 48000.0;
    Signal input(96000, 0.0f);
    Random random(41);
    KickSpec kick;
    kick.hz = noteToHz(36.0);
    kick.decayTau = 0.03;
    addKick(input, 1000, kick, fs, random);

    EngineParams p = neutral();
    p.ringDecaySeconds = 1.0;

    Engine engine;
    engine.prepare(fs, 512);
    engine.setParams(p);
    Signal dry = input;
    run(engine, dry, nullptr, 0, dry.size());
    CHECK(dry == input);
    CHECK(engine.ringHitCount() == 0);

    p.ring = 1.0;
    engine.setParams(p);
    Signal wet = input;
    run(engine, wet, nullptr, 0, wet.size());
    CHECK(engine.ringHitCount() == 1);

    // 0.5 s after the hit the dry kick is gone (16 time constants) and the ring is halfway through its T60.
    const size_t at = 1000 + 24000;
    CHECK(peak(input, at, at + 2400) < 1.0e-6);
    const double ringing = amplitudeAt(wet, at, at + 4800, kick.hz, fs);
    CHECK(ringing > 0.01);
    const double later = amplitudeAt(wet, at + 24000, at + 28800, kick.hz, fs);
    CHECK_NEAR(gainToDb(later / ringing), -30.0, 2.0);
}

TEST_CASE("Engine: reported tail covers the ringing and follows Ring decay and Q")
{
    const double fs = 48000.0;

    EngineParams p;
    CHECK(Engine::tailSeconds(p, fs) > 0.0);

    EngineParams narrow = p;
    narrow.boostQ = 200.0;
    CHECK(Engine::tailSeconds(narrow, fs) > Engine::tailSeconds(p, fs));

    EngineParams ringing = p;
    ringing.boostGainDb = 0.0;
    ringing.ring = 0.5;
    for (double decay : { 0.5, 1.0, 4.0 })
    {
        ringing.ringDecaySeconds = decay;
        CHECK_NEAR(Engine::tailSeconds(ringing, fs), decay + 0.1, 1.0e-9);
    }

    // After the input stops, the output is 60 dB down by the end of the reported tail.
    for (const EngineParams& params : { p, narrow, ringing })
    {
        const double tail = Engine::tailSeconds(params, fs);
        const size_t burst = 24000;
        const size_t tailSamples = static_cast<size_t>(tail * fs);
        Signal signal(burst + tailSamples + 4800, 0.0f);
        Random random(51);
        addKick(signal, 0, KickSpec { noteToHz(36.0), 0.0, 0.015, 0.05, 0.3, 0.5, 0.0005 }, fs, random);
        for (size_t i = burst; i < signal.size(); ++i)
            signal[i] = 0.0f;

        EngineParams unlimited = params;
        unlimited.limiter = false;
        const Signal out = processed(unlimited, signal, fs);
        const double loud = peak(out, 0, burst);
        CHECK_MSG(peak(out, burst + tailSamples, out.size()) < loud * 1.0e-3, "tail " + std::to_string(tail));
    }
}

TEST_CASE("Engine: the limiter holds the ceiling and keeps the signal when the EQ output is astronomically loud")
{
    // 32 wide bands at +24 dB stack up to hundreds of dB. Blending the limited
    // signal in as "y + (limited - y)" once cancelled it to zeros and ones.
    const double fs = 48000.0;
    EngineParams p;
    p.bands = 32;
    p.boostQ = 0.5;
    p.boostGainDb = 24.0;
    p.rangeOct = 6.0;
    p.tiltDbPerOct = 6.0;

    const Signal out = processed(p, sine(196.0, fs, 48000, 0.5), fs);
    CHECK(allFinite(out));
    CHECK(peak(out, 0, out.size()) <= Limiter::kCeiling);

    // What comes out is the limited signal at full level, not silence with spikes.
    size_t zeros = 0;
    double sumOfSquares = 0.0;
    for (size_t i = 24000; i < out.size(); ++i)
    {
        zeros += out[i] == 0.0f ? 1u : 0u;
        sumOfSquares += static_cast<double>(out[i]) * static_cast<double>(out[i]);
    }
    CHECK(zeros < 10);
    CHECK(std::sqrt(sumOfSquares / 24000.0) > 0.2);
}

TEST_CASE("Engine: the reported tail covers slow low bells, notes played by MIDI and bells near Nyquist")
{
    const auto decayAtTail = [](const EngineParams& params, double fs, int midiNote) {
        EngineParams p = params;
        p.limiter = false;
        Engine engine;
        engine.prepare(fs, 512);
        engine.setParams(p);
        if (midiNote >= 0)
            engine.handleMidi({ MidiEvent::Type::noteOn, 1, midiNote });

        const size_t burst = static_cast<size_t>(fs);
        const size_t tail = static_cast<size_t>(Engine::tailSeconds(p, fs) * fs);
        Signal signal = noise(burst + tail + 4800, 0.25, 77);
        std::fill(signal.begin() + static_cast<std::ptrdiff_t>(burst), signal.end(), 0.0f);
        run(engine, signal, nullptr, 0, signal.size());
        return gainToDb(peak(signal, burst + tail, signal.size()) / peak(signal, burst - 4800, burst));
    };

    // E0 at Q 200 and +24 dB rings for well over a minute.
    EngineParams low;
    low.midiMode = MidiMode::off;
    low.boostNote = 16.0;
    low.boostQ = 200.0;
    low.boostGainDb = 24.0;
    low.bands = 1;
    CHECK(Engine::tailSeconds(low, 48000.0) > 80.0);
    CHECK(decayAtTail(low, 48000.0, -1) < -60.0);

    // The knob says C6, MIDI plays C1: the tail has to cover the note that sounds.
    EngineParams played;
    played.boostNote = 84.0;
    played.boostQ = 200.0;
    played.boostGainDb = 24.0;
    played.bands = 1;
    CHECK(decayAtTail(played, 48000.0, 24) < -60.0);
    EngineParams knobOnly = played;
    knobOnly.midiMode = MidiMode::off;
    CHECK(Engine::tailSeconds(knobOnly, 48000.0) < 0.1 * Engine::tailSeconds(played, 48000.0));

    // Close to Nyquist the digital bell rings several times longer than the analog one.
    for (QMode mode : { QMode::constantQ, QMode::constantWidth })
    {
        EngineParams high;
        high.midiMode = MidiMode::off;
        high.boostNote = 96.0;
        high.boostQ = 200.0;
        high.boostGainDb = 24.0;
        high.bands = 9; // the ninth harmonic of C7 is 18.8 kHz
        high.rangeOct = 6.0;
        high.tiltDbPerOct = 0.0;
        high.qMode = mode;
        CHECK(decayAtTail(high, 44100.0, -1) < -60.0);
    }

    // The cut bank follows MIDI on channel 2.
    EngineParams cutting;
    cutting.boostGainDb = 0.0;
    cutting.cutOn = true;
    cutting.cutNote = 84.0;
    cutting.cutQ = 60.0;
    CHECK(Engine::tailSeconds(cutting, 48000.0) > bellRingSeconds(kLowSkipHz, 60.0, 0.0));
    cutting.midiMode = MidiMode::omniBoost;
    CHECK(Engine::tailSeconds(cutting, 48000.0) < 0.3);
}

TEST_CASE("Engine: turning Ring up in the middle of a sound is not a hit, a hit on the first sample is")
{
    const double fs = 48000.0;
    EngineParams p = neutral();
    Engine engine;
    engine.prepare(fs, 512);
    engine.setParams(p);

    // A steady tone that started while Ring was at zero.
    Signal tone = sine(60.0, fs, 96000, 0.5);
    run(engine, tone, nullptr, 0, 48000);
    p.ring = 1.0;
    engine.setParams(p);
    run(engine, tone, nullptr, 48000, 96000);
    CHECK(engine.ringHitCount() == 0);

    // Playback that starts right on a hit.
    Engine fresh;
    fresh.prepare(fs, 512);
    fresh.setParams(p);
    Signal hit(48000);
    for (size_t i = 0; i < hit.size(); ++i)
        hit[i] = static_cast<float>(0.8 * std::exp(-static_cast<double>(i) / (0.1 * fs)) * std::cos(kTwoPi * 55.0 * static_cast<double>(i) / fs));
    run(fresh, hit, nullptr, 0, hit.size());
    CHECK(fresh.ringHitCount() == 1);
}

TEST_CASE("Engine: band lists for a display match the banks")
{
    const double fs = 48000.0;
    EngineParams p;
    p.cutOn = true;
    p.cutNote = 26.0;
    BandView bands[kMaxSlots];

    const double root = noteToHz(30.0);
    const int boosts = Engine::boostBands(p, root, fs, bands);
    CHECK(boosts == 8);
    CHECK_NEAR(bands[0].hz, root, 1.0e-9);
    CHECK_NEAR(bands[0].gainDb, 9.0, 1.0e-9);
    CHECK_NEAR(bands[7].hz, 8.0 * root, 1.0e-9);
    CHECK(bands[7].gainDb > 0.0 && bands[7].gainDb < bands[0].gainDb);

    const int cuts = Engine::cutBands(p, noteToHz(26.0), fs, bands);
    CHECK(cuts == 6);
    CHECK_NEAR(bands[0].gainDb, -12.0, 1.0e-9);
    CHECK_NEAR(bands[1].gainDb, -6.0, 1.0e-9);

    p.cutOn = false;
    p.boostGainDb = 0.0;
    CHECK(Engine::boostBands(p, root, fs, bands) == 0);
    CHECK(Engine::cutBands(p, root, fs, bands) == 0);

    // Octaves: only the octaves inside the usable band.
    p.boostGainDb = 9.0;
    p.series = SeriesType::octaves;
    const int octaves = Engine::boostBands(p, noteToHz(36.0), fs, bands);
    CHECK(octaves >= 9 && octaves <= 11);
    CHECK(bands[0].hz > kLowSkipHz);
}
