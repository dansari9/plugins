#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/Limiter.h"
#include "dsp/ModalRing.h"
#include "dsp/TransientGate.h"

#include <string>
#include <vector>

using namespace rootnote;
using namespace testsignals;

namespace
{
/** Peak of |x| in [from, to). */
double peakIn(const std::vector<double>& x, size_t from, size_t to)
{
    double peak = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i)
        peak = std::max(peak, std::abs(x[i]));
    return peak;
}

/** Frequency from the spacing of rising zero crossings inside [from, to). */
double zeroCrossingHz(const std::vector<double>& x, size_t from, size_t to, double fs)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (size_t i = from + 1; i < to && i < x.size(); ++i)
    {
        if (x[i - 1] < 0.0 && x[i] >= 0.0)
        {
            const double at = static_cast<double>(i - 1) + (-x[i - 1]) / (x[i] - x[i - 1]);
            if (first < 0.0)
                first = at;
            last = at;
            ++crossings;
        }
    }
    return crossings >= 2 ? (crossings - 1) * fs / (last - first) : 0.0;
}

ModalRing::Settings singleMode(double decaySeconds, double dropSemitones = 0.0)
{
    ModalRing::Settings s;
    s.shape.series = SeriesType::harmonic;
    s.shape.bands = 1;
    s.decaySeconds = decaySeconds;
    s.dropSemitones = dropSemitones;
    return s;
}

/** Runs the ring on a mono float signal and returns its output. */
std::vector<double> runRing(ModalRing& ring, const std::vector<float>& in, int blockSize = 256)
{
    std::vector<double> out(in.size(), 0.0);
    for (size_t position = 0; position < in.size(); position += static_cast<size_t>(blockSize))
    {
        const int n = static_cast<int>(std::min(static_cast<size_t>(blockSize), in.size() - position));
        ring.process(in.data() + position, nullptr, out.data() + position, nullptr, n);
    }
    return out;
}
} // namespace

TEST_CASE("Limiter: transparent below the threshold")
{
    Limiter limiter;
    limiter.prepare(48000.0);
    Random random(2);
    bool identical = true;
    for (int i = 0; i < 48000; ++i)
    {
        const double l = 0.85 * random.bipolar();
        const double r = 0.85 * random.bipolar();
        double yl = l, yr = r;
        limiter.tick(yl, yr);
        identical = identical && yl == l && yr == r;
    }
    CHECK(identical);
    CHECK(limiter.gainReduction() == 1.0);
}

TEST_CASE("Limiter: output never exceeds the ceiling")
{
    CHECK_NEAR(gainToDb(Limiter::kCeiling), -0.3, 1.0e-9);
    CHECK_NEAR(gainToDb(Limiter::kThreshold), -1.3, 1.0e-9);

    for (double fs : { 44100.0, 96000.0, 192000.0 })
    {
        Limiter limiter;
        limiter.prepare(fs);
        Random random(9);
        double worst = 0.0;
        const int n = static_cast<int>(fs);

        // White noise at +12 dBFS, square-ish bursts, impulses, DC.
        for (int i = 0; i < 4 * n; ++i)
        {
            double l = 0.0, r = 0.0;
            if (i < n)
            {
                l = 4.0 * random.bipolar();
                r = 4.0 * random.bipolar();
            }
            else if (i < 2 * n)
            {
                l = ((i / 300) % 2 == 0) ? 10.0 : -10.0;
                r = 0.0;
            }
            else if (i < 3 * n)
            {
                l = r = (i % 1000 == 0) ? 50.0 : 0.0;
            }
            else
            {
                l = 3.0;
                r = -3.0;
            }
            limiter.tick(l, r);
            worst = std::max({ worst, std::abs(l), std::abs(r) });
            CHECK(std::isfinite(l) && std::isfinite(r));
        }
        CHECK(worst <= Limiter::kCeiling);
    }
}

TEST_CASE("Limiter: recovers within half a second of an absurd spike")
{
    const double fs = 48000.0;
    Limiter limiter;
    limiter.prepare(fs);
    double l = 1.0e30, r = -1.0e30;
    limiter.tick(l, r);
    CHECK(std::abs(l) <= Limiter::kCeiling && std::abs(r) <= Limiter::kCeiling);
    for (int i = 0; i < 24000; ++i)
    {
        l = r = 0.0;
        limiter.tick(l, r);
    }
    CHECK(limiter.gainReduction() == 1.0);
}

TEST_CASE("Limiter: a loud steady tone settles at the threshold and recovers after it")
{
    const double fs = 48000.0;
    Limiter limiter;
    limiter.prepare(fs);

    // +6 dBFS sine at 1 kHz for half a second.
    double peak = 0.0;
    for (int i = 0; i < 24000; ++i)
    {
        double l = 2.0 * std::sin(kTwoPi * 1000.0 * i / fs);
        double r = l;
        limiter.tick(l, r);
        if (i > 4800)
            peak = std::max(peak, std::abs(l));
    }
    CHECK(peak <= Limiter::kCeiling);
    CHECK(peak > 0.8 * Limiter::kThreshold);
    CHECK_NEAR(gainToDb(limiter.gainReduction()), -7.3, 1.0);

    // Stereo link: a quiet right channel is turned down with the loud left one.
    double l = 2.0, r = 0.1;
    limiter.tick(l, r);
    CHECK(r < 0.06);

    // Release is 80 ms: after 0.5 s of quiet signal the gain is back at unity.
    for (int i = 0; i < 24000; ++i)
    {
        double a = 0.1, b = 0.1;
        limiter.tick(a, b);
    }
    CHECK(limiter.gainReduction() == 1.0);
    double a = 0.5, b = -0.5;
    limiter.tick(a, b);
    CHECK(a == 0.5 && b == -0.5);
}

TEST_CASE("Limiter: soft clip is continuous, monotonic and bounded")
{
    double previous = -1.0;
    for (double x = 0.0; x < 20.0; x += 0.001)
    {
        const double y = Limiter::softClip(x);
        CHECK(y >= previous);
        CHECK(y <= Limiter::kCeiling);
        CHECK_NEAR(Limiter::softClip(-x), -y, 1.0e-15);
        previous = y;
    }
    const double knee = Limiter::kThreshold;
    CHECK_NEAR(Limiter::softClip(knee + 1.0e-7) - Limiter::softClip(knee - 1.0e-7), 2.0e-7, 1.0e-9); // slope 1 at the knee
}

TEST_CASE("TransientGate: one window per hit")
{
    const double fs = 48000.0;
    TransientGate gate;
    gate.prepare(fs);
    Random random(4);

    // Eight kicks, 250 ms apart.
    std::vector<float> loop(static_cast<size_t>(2.2 * fs), 0.0f);
    for (int hit = 0; hit < 8; ++hit)
    {
        KickSpec kick;
        kick.hz = 55.0;
        kick.decayTau = 0.12;
        addKick(loop, static_cast<size_t>((0.05 + 0.25 * hit) * fs), kick, fs, random);
    }

    int triggers = 0;
    double area = 0.0;
    for (float x : loop)
    {
        area += gate.tick(x);
        triggers += gate.takeTrigger() ? 1 : 0;
    }
    CHECK(triggers == 8);
    CHECK_NEAR(area / fs / 8.0, TransientGate::kEffectiveSeconds, 0.002);
}

TEST_CASE("TransientGate: window shape is 1 ms up, 30 ms hold, 20 ms down")
{
    const double fs = 48000.0;
    TransientGate gate;
    gate.prepare(fs);

    std::vector<double> level;
    for (int i = 0; i < 4800; ++i)
        level.push_back(gate.tick(i >= 100 ? 0.5 : 0.0));

    const auto at = [&](double ms) { return level[static_cast<size_t>(100 + ms * 0.001 * fs)]; };
    CHECK(level[99] == 0.0);
    CHECK(at(0.5) > 0.3 && at(0.5) < 0.7);
    CHECK(at(1.1) == 1.0);
    CHECK(at(30.0) == 1.0);
    CHECK(at(41.0) > 0.4 && at(41.0) < 0.6); // half way down
    CHECK(at(52.0) == 0.0);
    CHECK(!gate.isOpen());

    // A constant level after the hit does not open it again.
    int later = 0;
    gate.takeTrigger();
    for (int i = 0; i < 48000; ++i)
    {
        gate.tick(0.5);
        later += gate.takeTrigger() ? 1 : 0;
    }
    CHECK(later == 0);
}

TEST_CASE("TransientGate: sustained tones and decaying tails do not retrigger")
{
    const double fs = 48000.0;
    for (double hz : { 30.0, 41.2, 55.0, 82.4, 110.0, 220.0, 1000.0 })
    {
        TransientGate gate;
        gate.prepare(fs);
        int triggers = 0;
        for (int i = 0; i < static_cast<int>(3.0 * fs); ++i)
        {
            const double t = i / fs;
            // Starts abruptly (one hit), then holds for a second and decays.
            const double envelope = t < 1.0 ? 1.0 : std::exp(-(t - 1.0) / 0.4);
            gate.tick(0.6 * envelope * std::sin(kTwoPi * hz * t));
            triggers += gate.takeTrigger() ? 1 : 0;
        }
        CHECK_MSG(triggers == 1, std::to_string(hz) + " Hz gave " + std::to_string(triggers));
    }

    // Silence and low noise: nothing.
    TransientGate gate;
    gate.prepare(fs);
    int triggers = 0;
    Random random(8);
    for (int i = 0; i < 48000; ++i)
    {
        gate.tick(0.001 * random.bipolar());
        triggers += gate.takeTrigger() ? 1 : 0;
    }
    CHECK(triggers == 0);
}

TEST_CASE("ModalRing: silent without a hit, rings at the root after one")
{
    const double fs = 48000.0;
    const double root = 55.0;
    ModalRing ring;
    ring.prepare(fs);
    ring.setSettings(singleMode(0.8));
    ring.setTargetFrequency(root, 0.0);

    std::vector<float> silence(4800, 0.0f);
    const auto quiet = runRing(ring, silence);
    CHECK(peakIn(quiet, 0, quiet.size()) == 0.0);

    Random random(6);
    std::vector<float> in(static_cast<size_t>(1.5 * fs), 0.0f);
    KickSpec kick;
    kick.hz = root;
    kick.dropSemitones = 0.0;
    kick.decayTau = 0.05;
    kick.level = 0.8;
    addKick(in, 480, kick, fs, random);

    const auto out = runRing(ring, in);
    CHECK(ring.hitCount() == 1);

    // After the gate has closed (51 ms) only the resonator sounds.
    const size_t from = static_cast<size_t>(0.15 * fs);
    const size_t to = static_cast<size_t>(0.55 * fs);
    CHECK_NEAR(zeroCrossingHz(out, from, to, fs), root, 0.05);
    CHECK(peakIn(out, from, to) > 0.05);
}

TEST_CASE("ModalRing: level is about that of the partial that excited it, at any sample rate")
{
    const double root = 60.0;
    double levels[3] {};
    int index = 0;
    for (double fs : { 44100.0, 96000.0, 192000.0 })
    {
        ModalRing ring;
        ring.prepare(fs);
        ring.setSettings(singleMode(30.0)); // long enough that the decay does not colour the reading
        ring.setTargetFrequency(root, 0.0);

        // A steady sine of amplitude 0.5 that starts abruptly: the gate lets 41 ms of it through.
        std::vector<float> in(static_cast<size_t>(0.4 * fs));
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = i < 100 ? 0.0f : static_cast<float>(0.5 * std::sin(kTwoPi * root * static_cast<double>(i - 100) / fs));

        const auto out = runRing(ring, in);
        levels[index++] = peakIn(out, static_cast<size_t>(0.1 * fs), static_cast<size_t>(0.15 * fs));
    }
    for (double level : levels)
        CHECK_NEAR(gainToDb(level / 0.5), 0.0, 1.5);
    CHECK_NEAR(gainToDb(levels[0] / levels[2]), 0.0, 0.3);
}

TEST_CASE("ModalRing: decay time follows the Decay setting, independent of pitch")
{
    const double fs = 48000.0;
    for (double root : { 41.2, 110.0 })
        for (double t60 : { 0.2, 0.6, 2.0 })
        {
            ModalRing ring;
            ring.prepare(fs);
            ring.setSettings(singleMode(t60));
            ring.setTargetFrequency(root, 0.0);

            std::vector<float> in(static_cast<size_t>((t60 + 0.4) * fs), 0.0f);
            for (size_t i = 100; i < 100 + static_cast<size_t>(0.06 * fs); ++i)
                in[i] = static_cast<float>(0.5 * std::sin(kTwoPi * root * static_cast<double>(i) / fs));

            const auto out = runRing(ring, in);

            // Envelope at two points after the gate has closed; the slope gives T60.
            const double period = fs / root;
            const size_t a = static_cast<size_t>(0.08 * fs);
            const size_t b = a + static_cast<size_t>(0.5 * t60 * fs);
            const double levelA = peakIn(out, a, a + static_cast<size_t>(2.0 * period));
            const double levelB = peakIn(out, b, b + static_cast<size_t>(2.0 * period));
            const double measured = 60.0 * (0.5 * t60) / (gainToDb(levelA) - gainToDb(levelB));
            CHECK_NEAR_MSG(measured, t60, 0.1 * t60, std::to_string(root) + " Hz, " + std::to_string(t60) + " s");
        }
}

TEST_CASE("ModalRing: higher modes decay faster")
{
    const double fs = 48000.0;
    const double root = 80.0;

    // Excite modes 1 and 4 with one click; compare their levels early and late.
    ModalRing ring;
    ring.prepare(fs);
    ModalRing::Settings s;
    s.shape.series = SeriesType::harmonic;
    s.shape.bands = 4;
    s.shape.tiltDbPerOct = 0.0;
    s.shape.rangeOct = 6.0;
    s.decaySeconds = 1.0;
    ring.setSettings(s);
    ring.setTargetFrequency(root, 0.0);

    std::vector<float> in(static_cast<size_t>(1.2 * fs), 0.0f);
    in[100] = 1.0f;
    const auto out = runRing(ring, in);

    const size_t window = static_cast<size_t>(0.1 * fs);
    const size_t early = static_cast<size_t>(0.06 * fs);
    const size_t late = static_cast<size_t>(0.26 * fs);
    const double fallRoot = gainToDb(toneAmplitude(out, early, early + window, root, fs))
                          - gainToDb(toneAmplitude(out, late, late + window, root, fs));
    const double fallFourth = gainToDb(toneAmplitude(out, early, early + window, 4.0 * root, fs))
                            - gainToDb(toneAmplitude(out, late, late + window, 4.0 * root, fs));

    // T60_k = T60 / k^0.7: the 4th harmonic falls 4^0.7 = 2.64 times as fast.
    CHECK_NEAR(fallRoot, 60.0 * 0.20, 1.0);
    CHECK_NEAR(fallFourth / fallRoot, std::pow(4.0, ModalRing::kDamping), 0.15);
}

TEST_CASE("ModalRing: Drop starts above the root, settles on it, and keeps the amplitude smooth")
{
    const double fs = 48000.0;
    const double root = 50.0;
    const double t60 = 1.2;
    ModalRing ring;
    ring.prepare(fs);
    ring.setSettings(singleMode(t60, 12.0));
    ring.setTargetFrequency(root, 0.0);

    CHECK_NEAR(ModalRing::dropSeconds(t60), 0.18, 1.0e-12);
    CHECK_NEAR(ModalRing::dropSeconds(0.05), 0.020, 1.0e-12);
    CHECK_NEAR(ModalRing::dropSeconds(4.0), 0.300, 1.0e-12);

    std::vector<float> in(static_cast<size_t>(1.0 * fs), 0.0f);
    Random random(12);
    for (size_t i = 100; i < 100 + 96; ++i)
        in[i] = static_cast<float>(random.bipolar()); // 2 ms noise burst
    const auto out = runRing(ring, in);

    // One octave up at the start, on the root once the drop has settled.
    const double early = zeroCrossingHz(out, 110, 110 + static_cast<size_t>(0.012 * fs), fs);
    const double late = zeroCrossingHz(out, static_cast<size_t>(0.4 * fs), static_cast<size_t>(0.8 * fs), fs);
    CHECK(early > root * 1.6);
    CHECK_NEAR(late, root, 0.05);
    CHECK(ring.currentDropSemitones() == 0.0);

    // The envelope (peak per cycle) only ever falls: a glide must not pump the level.
    double previous = 1.0e9;
    bool monotonic = true;
    size_t position = 400;
    while (position + 2000 < out.size())
    {
        const double peak = peakIn(out, position, position + 1000);
        monotonic = monotonic && peak <= previous * 1.02;
        previous = peak;
        position += 1000;
    }
    CHECK(monotonic);

    // And no clicks: the output never jumps by more than a 100 Hz full-level sine could.
    const double level = peakIn(out, 0, out.size());
    double worstStep = 0.0;
    for (size_t i = 300; i < out.size(); ++i) // after the noise burst itself
        worstStep = std::max(worstStep, std::abs(out[i] - out[i - 1]));
    CHECK(worstStep < level * kTwoPi * 100.0 / fs * 1.5);
}

TEST_CASE("ModalRing: follows the boost series and weights, stereo keeps its sides")
{
    const double fs = 48000.0;
    const double root = 100.0;
    ModalRing ring;
    ring.prepare(fs);
    ModalRing::Settings s;
    s.shape.series = SeriesType::drum;
    s.shape.bands = 3;
    s.shape.tiltDbPerOct = 0.0;
    s.shape.rangeOct = 6.0;
    s.decaySeconds = 6.0;
    ring.setSettings(s);
    ring.setTargetFrequency(root, 0.0);

    // Click on the left only. A long decay and a long window keep the partials apart.
    std::vector<float> left(static_cast<size_t>(2.2 * fs), 0.0f), right(left.size(), 0.0f);
    left[50] = 1.0f;
    std::vector<double> outL(left.size()), outR(left.size());
    ring.process(left.data(), right.data(), outL.data(), outR.data(), static_cast<int>(left.size()));

    CHECK(peakIn(outR, 0, outR.size()) == 0.0);
    CHECK(peakIn(outL, 0, outL.size()) > 0.0);

    const size_t a = static_cast<size_t>(0.06 * fs);
    const size_t b = static_cast<size_t>(2.06 * fs);
    const double atRoot = toneAmplitude(outL, a, b, root, fs);
    const double atSecond = toneAmplitude(outL, a, b, root * kDrumRatios[1], fs);
    const double atThird = toneAmplitude(outL, a, b, root * kDrumRatios[2], fs);
    const double atHarmonic = toneAmplitude(outL, a, b, root * 2.0, fs);
    const double atFourth = toneAmplitude(outL, a, b, root * kDrumRatios[3], fs);
    CHECK(atSecond > 0.3 * atRoot);
    CHECK(atThird > 0.2 * atRoot);
    CHECK(atHarmonic < 0.05 * atRoot); // 2.0 is not a membrane mode
    CHECK(atFourth < 0.12 * atRoot);   // Bands = 3 switches the fourth mode off (2.296 leaks from 2.136)
}

TEST_CASE("TransientGate: a hit on the very first sample opens the gate")
{
    const double fs = 48000.0;
    for (int offset : { 0, 1, 100 })
    {
        TransientGate gate;
        gate.prepare(fs);
        int triggers = 0;
        for (int i = 0; i < 48000; ++i)
        {
            const int n = i - offset;
            gate.tick(n < 0 ? 0.0 : 0.8 * std::exp(-n / (0.1 * fs)) * std::cos(kTwoPi * 55.0 * n / fs));
            triggers += gate.takeTrigger() ? 1 : 0;
        }
        CHECK_MSG(triggers == 1, "offset " + std::to_string(offset));
    }
}

TEST_CASE("ModalRing: a hit with opposite polarity left and right rings on both sides")
{
    const double fs = 48000.0;
    ModalRing ring;
    ring.prepare(fs);
    ring.setSettings(singleMode(0.6));
    ring.setTargetFrequency(55.0, 0.0);

    std::vector<float> left(48000, 0.0f), right(48000, 0.0f);
    Random random(3);
    KickSpec kick;
    kick.hz = 55.0;
    addKick(left, 1000, kick, fs, random);
    for (size_t i = 0; i < left.size(); ++i)
        right[i] = -left[i];

    std::vector<double> outL(left.size()), outR(left.size());
    ring.process(left.data(), right.data(), outL.data(), outR.data(), static_cast<int>(left.size()));
    CHECK(ring.hitCount() == 1);
    CHECK(peakIn(outL, 6000, 12000) > 0.01);
    bool mirrored = true;
    for (size_t i = 0; i < outL.size(); ++i)
        mirrored = mirrored && outR[i] == -outL[i];
    CHECK(mirrored);
}

TEST_CASE("ModalRing: the octave series rings on every octave the EQ has")
{
    CHECK(ModalRing::modeCount(SeriesType::harmonic) == 16);
    CHECK(ModalRing::modeCount(SeriesType::octaves) == kOctaveSlots);

    // Root C7, focus five octaves down on C2 with a narrow range: only the 65 Hz octave is left.
    const double fs = 48000.0;
    const double root = 2093.005;
    ModalRing ring;
    ring.prepare(fs);
    ModalRing::Settings s;
    s.shape.series = SeriesType::octaves;
    s.shape.focusLink = false;
    s.shape.focusHz = root / 32.0;
    s.shape.rangeOct = 0.3;
    s.shape.tiltDbPerOct = 0.0;
    s.decaySeconds = 1.0;
    ring.setSettings(s);
    ring.setTargetFrequency(root, 0.0);

    std::vector<float> in(48000, 0.0f);
    Random random(4);
    KickSpec kick;
    kick.hz = root / 32.0;
    addKick(in, 1000, kick, fs, random);
    const std::vector<double> out = runRing(ring, in);
    CHECK(toneAmplitude(out, 6000, 30000, root / 32.0, fs) > 0.01);
}

TEST_CASE("ModalRing: a mode that Drop pushes past the top of the band fades out instead of piling up there")
{
    // Root C7 at 48 kHz: harmonics 6 to 10 start an octave up, above 0.45 fs,
    // and once all sat on the guard frequency at full level.
    const double fs = 48000.0;
    const double root = 2093.005;
    const auto guardLevel = [&](int bands) {
        ModalRing ring;
        ring.prepare(fs);
        ModalRing::Settings s;
        s.shape.bands = bands;
        s.shape.rangeOct = 6.0;
        s.shape.tiltDbPerOct = 0.0;
        s.decaySeconds = 2.0;
        s.dropSemitones = 12.0;
        ring.setSettings(s);
        ring.setTargetFrequency(root, 0.0);

        std::vector<float> in(24000, 0.0f);
        Random random(5);
        for (size_t i = 4800; i < 4800 + 2400; ++i)
            in[i] = static_cast<float>(0.5 * random.bipolar());
        const std::vector<double> out = runRing(ring, in);
        // 25 ms .. 40 ms after the hit the drop still holds those harmonics above the guard.
        return toneAmplitude(out, 4800 + 1200, 4800 + 1920, kNyquistGuard * fs, fs);
    };
    const double reference = guardLevel(5); // nothing reaches the guard
    CHECK(guardLevel(10) < 2.0 * reference + 1.0e-4);
}
