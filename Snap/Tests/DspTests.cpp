// Unit tests for the shared DSP primitives in Source/dsp/common.
#include "Check.h"
#include "dsp/common/Ducker.h"
#include "dsp/common/FocusSplitter.h"
#include "dsp/common/HitDetector.h"
#include "dsp/common/LoudnessMeter.h"
#include "dsp/common/Oversampler.h"
#include "dsp/common/Smoother.h"
#include "dsp/common/TransientMask.h"
#include <random>

using namespace snap;
namespace
{
constexpr double fs = 48000, twoPi = juce::MathConstants<double>::twoPi;

float sine(int i, double hz, float amp = 1) { return amp * (float) std::sin(twoPi * hz * i / fs); }
float noise(std::mt19937& rng) { return std::uniform_real_distribution<float>(-1, 1)(rng); }
float db(double ratio) { return (float) (10 * std::log10(ratio)); } // of an energy ratio
double energy(const std::vector<float>& x, size_t from)
{
    double e = 0;
    for (size_t i = from; i < x.size(); ++i)
        e += (double) x[i] * x[i];
    return e;
}
float maxStep(const std::vector<float>& x) // largest sample-to-sample jump
{
    float m = 0;
    for (size_t i = 1; i < x.size(); ++i)
        m = std::max(m, std::abs(x[i] - x[i - 1]));
    return m;
}
int firstAbove(const std::vector<float>& x, float level)
{
    for (size_t i = 0; i < x.size(); ++i)
        if (x[i] > level)
            return (int) i;
    return -1;
}

void testSmoother()
{
    Smoother s;
    s.prepare(fs, 0.01); // 480 steps
    s.reset(0);
    s.setTarget(1);
    for (int i = 0; i < 240; ++i)
        s.next();
    CHECK(std::abs(s.value() - 0.5f) < 1.0e-4f && s.isSmoothing());
    for (int i = 0; i < 240; ++i)
        s.next();
    CHECK(s.value() == 1.0f && !s.isSmoothing()); // lands exactly
    s.setTarget(1.0e-30f);                         // a denormal-sized target is flushed
    for (int i = 0; i < 500; ++i)
        s.next();
    CHECK(s.value() == 0.0f && s.targetValue() == 0.0f);
    s.setTarget(0);
    CHECK(!s.isSmoothing()); // no ramp when the target does not change
}

void testEnvelopeFollower()
{
    EnvelopeFollower e;
    e.prepare(fs, 1, 100);
    float v = 0;
    for (int i = 0; i < 48; ++i) // one attack time constant
        v = e.next(1);
    CHECK(std::abs(v - (1 - std::exp(-1.0f))) < 0.01f);
    for (int i = 0; i < 4800; ++i)
        v = e.next(1);
    for (int i = 0; i < 4800; ++i) // one release time constant
        v = e.next(0);
    CHECK(std::abs(v - std::exp(-1.0f)) < 0.01f);
    for (int i = 0; i < 48000 * 4; ++i)
        v = e.next(0);
    CHECK(v == 0.0f); // decays to exact zero, never through denormals
}

// Runs a crossover over `x` with a cutoff schedule; returns low and high.
void runCrossover(LRCrossover& xo, const std::vector<float>& x, std::vector<float>& low, std::vector<float>& high,
                  std::function<float(int)> cutoff = nullptr)
{
    low.resize(x.size()), high.resize(x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (cutoff)
            xo.setCutoff(cutoff((int) i));
        xo.step();
        xo.split(0, x[i], low[i], high[i]);
    }
}

void testCrossover()
{
    const int n = 48000;
    std::vector<float> x(n), low, high, sum(n);
    for (double hz : { 50.0, 400.0, 1000.0, 2500.0, 12000.0 }) // LR4: low + high is an allpass, -6 dB each at the cutoff
    {
        LRCrossover xo;
        xo.setCutoff(1000);
        xo.prepare(fs, 1);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = sine(i, hz);
        runCrossover(xo, x, low, high);
        for (int i = 0; i < n; ++i)
            sum[(size_t) i] = low[(size_t) i] + high[(size_t) i];
        CHECK(std::abs(db(energy(sum, n / 2) / energy(x, n / 2))) < 0.01f);
        if (hz == 1000.0)
            CHECK(std::abs(db(energy(low, n / 2) / energy(x, n / 2)) + 6.02f) < 0.1f
                  && std::abs(db(energy(high, n / 2) / energy(x, n / 2)) + 6.02f) < 0.1f);
        if (hz == 50.0)
            CHECK(db(energy(high, n / 2) / energy(x, n / 2)) < -50);
    }

    std::mt19937 rng(1);
    for (auto& v : x)
        v = noise(rng);
    for (float edge : { constants::focusMinHz, 10.0f, constants::focusMaxHz, 22000.0f }) // range ends are exact
    {
        LRCrossover xo;
        xo.setCutoff(edge);
        xo.prepare(fs, 1);
        runCrossover(xo, x, low, high);
        const bool toHigh = edge <= constants::focusMinHz;
        CHECK((toHigh ? high : low) == x);
        CHECK(std::all_of((toHigh ? low : high).begin(), (toHigh ? low : high).end(), [](float v) { return v == 0.0f; }));
        CHECK(xo.isBypassed());
    }

    // Zipper-free: sweeping the cutoff fast (and in from the bypassed end) moves the sum no faster than the sine itself.
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = sine(i, 300);
    for (float start : { 100.0f, constants::focusMinHz, constants::focusMaxHz })
    {
        LRCrossover xo;
        xo.setCutoff(start);
        xo.prepare(fs, 1);
        runCrossover(xo, x, low, high, [&](int i) { return i < n / 4 ? start : (i / 2400) % 2 ? 5000.0f : 150.0f; });
        for (int i = 0; i < n; ++i)
            sum[(size_t) i] = low[(size_t) i] + high[(size_t) i];
        CHECK(maxStep(sum) < 1.3f * maxStep(x));
    }
}

void testFocusSplitter()
{
    const int n = 48000, block = 333, channels = 2;
    std::mt19937 rng(2);
    juce::AudioBuffer<float> in(channels, n);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
            in.setSample(c, i, noise(rng));

    // process(band) runs on each block between split and merge; weight is the per-sample Mix x Target.
    auto run = [&](FocusSplitter& f, const juce::AudioBuffer<float>& src, std::function<void(juce::AudioBuffer<float>&, int)> process,
                   float weight) {
        juce::AudioBuffer<float> out(src), buf(channels, block);
        std::vector<float> w((size_t) block, weight);
        for (int start = 0; start < n; start += block)
        {
            const int len = std::min(block, n - start);
            for (int c = 0; c < channels; ++c)
                buf.copyFrom(c, 0, src, c, start, len);
            f.split(buf, len);
            process(buf, len);
            f.merge(buf, w.data(), len);
            for (int c = 0; c < channels; ++c)
                out.copyFrom(c, start, buf, c, 0, len);
        }
        return out;
    };
    auto same = [&](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int delay) {
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                if (a.getSample(c, i) != (i < delay ? 0.0f : b.getSample(c, i - delay)))
                    return false;
        return true;
    };
    auto prepared = [&](float lo, float hi, int delay) {
        auto f = std::make_unique<FocusSplitter>();
        f->setEdges(lo, hi);
        f->prepare(fs, channels, block, 512);
        f->setDelay(delay);
        return f;
    };
    auto identity = [](juce::AudioBuffer<float>&, int) {};
    auto doubled = [](juce::AudioBuffer<float>& b, int len) {
        for (int c = 0; c < b.getNumChannels(); ++c)
            juce::FloatVectorOperations::multiply(b.getWritePointer(c), 2.0f, len);
    };

    // Full range: bit-exact for any weight, a processed band is the whole signal, and the dry is delay-aligned.
    for (float w : { 0.0f, 0.37f, 1.0f })
        CHECK(same(run(*prepared(constants::focusMinHz, constants::focusMaxHz, 0), in, identity, w), in, 0));
    {
        auto twice = run(*prepared(constants::focusMinHz, constants::focusMaxHz, 0), in, doubled, 1);
        juce::AudioBuffer<float> expected(in);
        expected.applyGain(2);
        CHECK(same(twice, expected, 0));
    }
    {
        Delay latency; // a module with 100 samples of latency
        latency.prepare(channels, 100);
        latency.setDelay(100);
        auto delayed = [&](juce::AudioBuffer<float>& b, int len) {
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < len; ++i)
                    b.setSample(c, i, latency.next(c, b.getSample(c, i)));
        };
        CHECK(same(run(*prepared(constants::focusMinHz, constants::focusMaxHz, 100), in, delayed, 0.5f), in, 100));
    }

    // In range: the unprocessed split sums flat; muting the band removes it and leaves the outside alone.
    auto level = [&](float lo, float hi, double hz, std::function<void(juce::AudioBuffer<float>&, int)> process) {
        juce::AudioBuffer<float> s(channels, n);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                s.setSample(c, i, sine(i, hz));
        auto out = run(*prepared(lo, hi, 0), s, process, 1);
        std::vector<float> a(out.getReadPointer(1), out.getReadPointer(1) + n), b(s.getReadPointer(1), s.getReadPointer(1) + n);
        return db(energy(a, n / 2) / energy(b, n / 2));
    };
    auto mute = [](juce::AudioBuffer<float>& b, int len) { b.clear(0, len); };
    for (double hz : { 40.0, 200.0, 1000.0, 5000.0, 15000.0 })
        CHECK(std::abs(level(200, 2000, hz, identity)) < 0.01f);
    CHECK(level(200, 2000, 630, mute) < -20 && std::abs(level(200, 2000, 40, mute)) < 0.2f && std::abs(level(200, 2000, 10000, mute)) < 0.2f);
    CHECK(std::abs(level(constants::focusMinHz, 1000, 40, doubled) - 6.02f) < 0.2f && std::abs(level(constants::focusMinHz, 1000, 10000, doubled)) < 0.2f);
}

void testTransientMask()
{
    const int n = 24000, hit = 12000;
    juce::AudioBuffer<float> io(2, n);
    for (int i = 0; i < n; ++i) // a quiet bed, then a hard hit
        for (int c = 0; c < 2; ++c)
            io.setSample(c, i, i < hit ? sine(i, 200, 0.01f) : sine(i, 200, 0.8f) * std::exp(-(i - hit) / 2400.0f));
    for (int lookahead : { (int) std::lround(fs * constants::maskLookaheadMs * 0.001), constants::lowLatencyMaskLookahead })
    {
        TransientMask m;
        m.prepare(fs, 2, 512);
        m.setLookahead(lookahead);
        juce::AudioBuffer<float> out(io);
        std::vector<float> mask((size_t) n);
        for (int start = 0; start < n; start += 100)
        {
            juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, 100);
            m.process(view, 100, mask.data() + start);
        }
        bool aligned = true; // the audio is the input delayed by exactly the lookahead
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                aligned = aligned && out.getSample(c, i) == (i < lookahead ? 0.0f : io.getSample(c, i - lookahead));
        CHECK(aligned);
        CHECK(*std::max_element(mask.begin() + hit / 2, mask.begin() + hit) < 0.05f); // steady bed: sustain
        const std::vector<float> after(mask.begin() + hit / 2, mask.end()); // the bed has settled
        CHECK(firstAbove(after, 0.05f) + hit / 2 >= hit && firstAbove(after, 0.05f) + hit / 2 < hit + lookahead); // opens before the delayed hit
        if (lookahead > constants::lowLatencyMaskLookahead)
            CHECK(firstAbove(after, 0.5f) + hit / 2 < hit + lookahead);
        CHECK(*std::max_element(mask.begin() + hit, mask.begin() + hit + 480) > 0.9f);
        CHECK(mask[(size_t) n - 1] < 0.05f); // the decaying tail reads as sustain again
    }
    CHECK(TransientMask::weight(0.3f, 0) == 1.0f && TransientMask::weight(1, -1) == 1.0f && TransientMask::weight(0, -1) == 0.0f);
    CHECK(TransientMask::weight(0, 1) == 1.0f && TransientMask::weight(1, 1) == 0.0f && std::abs(TransientMask::weight(0, -0.5f) - 0.5f) < 1e-6f);
}

void testHitDetector()
{
    const int n = 48000, lookahead = 144;
    // Kick-like 60 Hz bursts at known onsets; the third is quieter, the fourth comes 20 ms after the third (retrigger).
    const std::vector<int> onsets { 4800, 14400, 24000, 24960, 33600 };
    const std::vector<float> amps { 0.9f, 0.3f, 0.1f, 0.9f, 0.9f };
    auto make = [&](double hz) {
        juce::AudioBuffer<float> b(2, n);
        b.clear();
        for (size_t k = 0; k < onsets.size(); ++k)
            for (int i = onsets[k]; i < std::min(n, onsets[k] + 4000); ++i)
                for (int c = 0; c < 2; ++c)
                    b.addSample(c, i, sine(i - onsets[k], hz, amps[k]) * std::exp(-(i - onsets[k]) / 600.0f));
        return b;
    };
    auto detect = [&](const juce::AudioBuffer<float>& b) {
        HitDetector d;
        d.prepare(fs, lookahead);
        d.setRange(30, 150), d.setThreshold(-30);
        std::vector<float> trig((size_t) n);
        for (int start = 0; start < n; start += 64)
            d.process(juce::AudioBuffer<float>(const_cast<float* const*>(b.getArrayOfReadPointers()), 2, start, 64), 64, trig.data() + start);
        std::vector<std::pair<int, float>> hits;
        for (int i = 0; i < n; ++i)
            if (trig[(size_t) i] > 0)
                hits.emplace_back(i, trig[(size_t) i]);
        return hits;
    };
    const auto hits = detect(make(60));
    CHECK(hits.size() == 4); // the burst 20 ms after another is inside the retrigger interval
    const std::vector<size_t> expected { 0, 1, 2, 4 };
    for (size_t k = 0; k < std::min(hits.size(), expected.size()); ++k)
    {
        // Lands on the hit in the delayed audio, give or take the band-pass delay: 1.5 ms for hits well above the
        // threshold, 2.5 ms for one that barely crosses it.
        const int late = hits[k].first - (onsets[expected[k]] + lookahead);
        CHECK(late >= 0 && late <= (amps[expected[k]] >= 0.3f ? 72 : 120));
        CHECK(hits[k].second > 0 && hits[k].second <= 1);
    }
    if (hits.size() == 4)
        CHECK(hits[0].second > hits[1].second && hits[1].second > hits[2].second);
    CHECK(detect(make(5000)).empty()); // outside the detection range

    // Hysteresis: a level wobbling across the threshold triggers once, not on every crossing.
    juce::AudioBuffer<float> wobble(1, n);
    for (int i = 0; i < n; ++i)
        wobble.setSample(0, i, sine(i, 80, 0.0316f * (1 + 0.3f * (float) std::sin(twoPi * 5 * i / fs))));
    HitDetector d;
    d.prepare(fs, 0);
    d.setRange(30, 150), d.setThreshold(-30);
    std::vector<float> trig((size_t) n);
    d.process(wobble, n, trig.data());
    CHECK(std::count_if(trig.begin(), trig.end(), [](float v) { return v > 0; }) == 1);
}

void testDucker()
{
    Ducker d;
    d.prepare(fs);
    bool unity = true;
    for (int i = 0; i < 4800; ++i)
        unity = unity && d.next(1, 0) == 1.0f;
    CHECK(unity); // amount 0 never touches the added signal
    d.reset();
    float g = 1;
    for (int i = 0; i < 4800; ++i)
        g = d.next(1, 1);
    CHECK(std::abs(juce::Decibels::gainToDecibels(g) + constants::duckDepthDb) < 0.5f);
    float half = 1;
    d.reset();
    for (int i = 0; i < 4800; ++i)
        half = d.next(1, 0.5f);
    CHECK(half > g && half < 1);
    bool rising = true;
    for (int i = 0; i < 48000; ++i) // the added signal swells back after the hit
    {
        const float next = d.next(0, 1);
        rising = rising && next >= g;
        g = next;
    }
    CHECK(rising && g > 0.99f);
}

void testOversampler()
{
    const int n = 8192, block = 512;
    std::vector<float> x(n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = sine(i, 1000, 0.5f);
    auto roundTrip = [&](Oversampler& os) {
        std::vector<float> y(x);
        for (int start = 0; start < n; start += block)
        {
            float* p = y.data() + start;
            juce::dsp::AudioBlock<float> b(&p, 1, (size_t) block);
            os.up(b);
            os.down(b);
        }
        return y;
    };
    Oversampler off;
    off.prepare(1, block, 0, false);
    CHECK(roundTrip(off) == x && off.latency() == 0 && off.factor() == 1);

    Oversampler fir, iir;
    fir.prepare(1, block, 1, false), iir.prepare(1, block, 1, true);
    CHECK(fir.factor() == 2 && iir.factor() == 2);
    CHECK(iir.latency() < fir.latency() && iir.latency() + constants::lowLatencyMaskLookahead <= constants::lowLatencySamples);
    const auto yf = roundTrip(fir), yi = roundTrip(iir);
    std::vector<float> residual;
    for (int i = n / 2; i < n; ++i)
        residual.push_back(yf[(size_t) i] - x[(size_t) (i - fir.latency())]);
    CHECK(db(energy(residual, 0) / energy(x, n / 2)) < -60); // linear phase: the input, delayed by the reported latency
    CHECK(std::abs(db(energy(yi, n / 2) / energy(x, n / 2))) < 0.1f);
}

void testLoudness()
{
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        auto measure = [&](float left, float right) {
            LoudnessMeter m;
            m.prepare(rate, 2);
            juce::AudioBuffer<float> b(2, 4410);
            for (int start = 0; start < (int) rate * 4; start += b.getNumSamples())
            {
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const float s = (float) std::sin(twoPi * 997 * (start + i) / rate);
                    b.setSample(0, i, left * s), b.setSample(1, i, right * s);
                }
                m.process(b, b.getNumSamples());
            }
            return m.lufs();
        };
        CHECK(std::abs(measure(1, 0) + 3.01f) < 0.05f); // the BS.1770 reference: 0 dBFS 997 Hz in one channel
        CHECK(std::abs(measure(1, 1) - 0.0f) < 0.05f);
        CHECK(std::abs(measure(0.1f, 0.1f) + 20.0f) < 0.05f);
        CHECK(measure(0, 0) == LoudnessMeter::silence);
    }
}

// Every primitive's audio path runs without touching the heap.
void testNoAllocation()
{
    const int block = 256;
    juce::AudioBuffer<float> buf(2, block);
    std::vector<float> scratch((size_t) block, 0.5f);
    FocusSplitter f;
    f.prepare(fs, 2, block, 256), f.setDelay(64);
    TransientMask m;
    m.prepare(fs, 2, 256), m.setLookahead(144);
    HitDetector h;
    h.prepare(fs, 144);
    Ducker d;
    d.prepare(fs);
    Oversampler os;
    os.prepare(2, block, 2, false);
    LoudnessMeter lm;
    lm.prepare(fs, 2);
    std::mt19937 rng(3);
    takeAudioAllocations();
    for (int k = 0; k < 40; ++k)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i)
                buf.setSample(c, i, noise(rng));
        setAudioThread(true);
        f.setEdges(100.0f + (float) k * 50, 8000.0f - (float) k * 100);
        f.split(buf, block);
        f.merge(buf, scratch.data(), block);
        m.process(buf, block, scratch.data());
        h.setRange(30, 150 + (float) k), h.process(buf, block, scratch.data());
        for (int i = 0; i < block; ++i)
            scratch[(size_t) i] = d.next(std::abs(buf.getSample(0, i)), 0.5f);
        juce::dsp::AudioBlock<float> b(buf);
        os.up(b), os.down(b);
        lm.process(buf, block);
        setAudioThread(false);
    }
    CHECK(takeAudioAllocations() == 0);
}
} // namespace

void testDsp()
{
    testSmoother();
    testEnvelopeFollower();
    testCrossover();
    testFocusSplitter();
    testTransientMask();
    testHitDetector();
    testDucker();
    testOversampler();
    testLoudness();
    testNoAllocation();
}
