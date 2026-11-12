#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/BandBank.h"
#include "dsp/NoteMath.h"

#include <string>
#include <vector>

using namespace rootnote;
using namespace testsignals;

namespace
{
BandBank::Settings boostSettings(double gainDb, double q, int bands, SeriesType series = SeriesType::harmonic)
{
    BandBank::Settings s;
    s.series = series;
    s.bands = bands;
    s.gainDb = gainDb;
    s.q = q;
    s.blend = 0.5;
    s.focusLink = true;
    s.rangeOct = 1.5;
    s.tiltDbPerOct = -3.0;
    return s;
}

/** Steady-state gain of a prepared, settled bank at one frequency (mono). */
double bankGainDb(BandBank& bank, double hz, double fs, double settleSeconds)
{
    bank.reset();
    return measureGainDb([&](double x)
                         {
                             bank.process(&x, nullptr, 1);
                             return x;
                         },
                         hz, fs, settleSeconds);
}

/** Sum of the analytic bell responses of all bands the bank has switched on. */
double expectedGainDb(const BandBank& bank, const BandBank::Settings& s, double rootHz, double hz, double fs)
{
    double total = 0.0;
    for (int slot = 0; slot < kMaxSlots; ++slot)
        if (bank.slotActive(slot))
            total += bellResponseDb(hz, rootHz * bank.slotRatio(slot), slotQ(s.qMode, s.q, bank.slotRatio(slot)),
                                    bank.slotGainDb(slot), fs);
    return total;
}

void runSilence(BandBank& bank, int samples)
{
    std::vector<double> zeros(static_cast<size_t>(samples), 0.0);
    bank.process(zeros.data(), nullptr, samples);
}
} // namespace

TEST_CASE("BandBank: every band centre sits exactly on root x ratio")
{
    const double fs = 48000.0;
    const double root = noteToHz(36.0); // C2

    for (SeriesType series : { SeriesType::harmonic, SeriesType::drum, SeriesType::octaves })
    {
        // Narrow bells so that neighbours hardly overlap and each centre shows its own gain.
        BandBank::Settings s = boostSettings(12.0, 60.0, 6, series);
        s.tiltDbPerOct = 0.0;
        s.rangeOct = 6.0;

        BandBank bank;
        bank.prepare(fs, BandBank::Kind::boost);
        bank.setSettings(s);
        bank.setTargetFrequency(root, 0.0);
        runSilence(bank, 64);

        int checked = 0;
        for (int slot = 0; slot < kMaxSlots; ++slot)
        {
            if (!bank.slotActive(slot))
                continue;
            const double ratio = seriesRatio(series, slot);
            const double centre = root * ratio;
            if (centre > 6000.0)
                continue; // keep the test short; the high bands are covered by the Nyquist test
            CHECK_NEAR(bank.slotRatio(slot), ratio, 1.0e-10);

            const double settle = 4.0 * bellRingSeconds(centre, 60.0, 12.0);
            const double atCentre = bankGainDb(bank, centre, fs, settle);
            const double below = bankGainDb(bank, centre * 0.985, fs, settle);
            const double above = bankGainDb(bank, centre * 1.015, fs, settle);
            const std::string where = "series " + std::to_string(static_cast<int>(series)) + " slot " + std::to_string(slot);

            // The peak is at the centre, and its height is the band gain plus
            // the (small) skirts of the neighbours, as the analytic sum predicts.
            CHECK_MSG(atCentre > below && atCentre > above, where);
            CHECK_NEAR_MSG(atCentre, expectedGainDb(bank, s, root, centre, fs), 0.1, where);
            CHECK_NEAR_MSG(atCentre, bank.slotGainDb(slot), 0.6, where);
            ++checked;
        }
        CHECK(checked >= 5);
    }
}

TEST_CASE("BandBank: band gains follow the weighting rules")
{
    const double fs = 48000.0;
    const double root = noteToHz(36.0);
    BandBank bank;
    bank.prepare(fs, BandBank::Kind::boost);

    // Range and tilt.
    BandBank::Settings s = boostSettings(9.0, 12.0, 8);
    bank.setSettings(s);
    bank.setTargetFrequency(root, 0.0);
    runSilence(bank, 64);
    CHECK(bank.activeBandCount() == 8);
    for (int slot = 0; slot < 8; ++slot)
    {
        const double octaves = std::log2(static_cast<double>(slot + 1));
        const double expected = 9.0 * std::exp(-0.5 * std::pow(octaves / 1.5, 2.0)) * std::pow(10.0, -3.0 * octaves / 20.0);
        CHECK_NEAR(bank.slotGainDb(slot), expected, 1.0e-9);
    }
    CHECK_NEAR(bank.slotGainDb(0), 9.0, 1.0e-12);

    // Blend, with range and tilt out of the way.
    s.tiltDbPerOct = 0.0;
    s.rangeOct = 6.0;
    for (double blend : { 0.0, 0.5, 1.0 })
    {
        s.blend = blend;
        bank.reset();
        bank.setSettings(s);
        runSilence(bank, 64);
        CHECK(bank.slotGainDb(0) > 8.9); // fundamental always full
        for (int slot = 1; slot < 8; ++slot)
        {
            const bool odd = ((slot + 1) % 2) == 1;
            const bool on = blend == 0.5 || (blend == 0.0 && odd) || (blend == 1.0 && !odd);
            if (on)
                CHECK(bank.slotGainDb(slot) > 7.0);
            else
                CHECK(bank.slotGainDb(slot) == 0.0);
            CHECK(bank.slotActive(slot) == on);
        }
    }
}

TEST_CASE("BandBank: no band above 0.45 fs, fade over the last sixth of an octave")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const double root = noteToHz(96.0); // C7, 2093 Hz: 32 harmonics reach 67 kHz
        BandBank::Settings s = boostSettings(12.0, 12.0, 32);
        s.tiltDbPerOct = 0.0;
        s.rangeOct = 6.0;

        BandBank bank;
        bank.prepare(fs, BandBank::Kind::boost);
        bank.setSettings(s);
        bank.setTargetFrequency(root, 0.0);
        runSilence(bank, 64);

        const double top = 0.45 * fs;
        const double fadeStart = top * std::exp2(-1.0 / 6.0);
        int inFade = 0;
        for (int slot = 0; slot < kMaxSlots; ++slot)
        {
            const double hz = root * (slot + 1);
            if (hz >= top)
            {
                CHECK(!bank.slotActive(slot));
                CHECK(bank.slotGainDb(slot) == 0.0);
            }
            else if (hz > fadeStart)
            {
                const double full = 12.0 * rangeWeight(hz, root, 6.0);
                CHECK(bank.slotGainDb(slot) < full);
                CHECK(bank.slotGainDb(slot) > 0.0);
                ++inFade;
            }
            else
            {
                CHECK_NEAR(bank.slotGainDb(slot), 12.0 * rangeWeight(hz, root, 6.0), 1.0e-9);
            }
        }
        CHECK(inFade >= (fs < 50000.0 ? 1 : 2));

        // And it stays finite when driven hard.
        Random random(3);
        double peak = 0.0;
        for (int i = 0; i < 20000; ++i)
        {
            double x = random.bipolar();
            bank.process(&x, nullptr, 1);
            CHECK(std::isfinite(x));
            peak = std::max(peak, std::abs(x));
        }
        CHECK(peak < 100.0);
    }
}

TEST_CASE("BandBank: octave series includes the octaves below the root down to 20 Hz")
{
    const double fs = 48000.0;
    BandBank::Settings s = boostSettings(9.0, 12.0, 1, SeriesType::octaves);
    s.tiltDbPerOct = 0.0;
    s.rangeOct = 6.0;

    BandBank bank;
    bank.prepare(fs, BandBank::Kind::boost);
    bank.setSettings(s);

    // C3 = 130.8 Hz: 65.4 and 32.7 Hz are in, 16.35 Hz is below 18 Hz and out.
    const double root = noteToHz(48.0);
    bank.setTargetFrequency(root, 0.0);
    runSilence(bank, 64);
    CHECK(bank.slotActive(kOctaveRootSlot));
    CHECK(bank.slotActive(kOctaveRootSlot - 1));
    CHECK(bank.slotActive(kOctaveRootSlot - 2));
    CHECK(!bank.slotActive(kOctaveRootSlot - 3));

    int active = 0;
    for (int slot = 0; slot < kMaxSlots; ++slot)
    {
        if (!bank.slotActive(slot))
            continue;
        ++active;
        const double hz = root * bank.slotRatio(slot);
        CHECK(hz >= 20.0 && hz <= 0.45 * fs);
        const double octave = std::log2(bank.slotRatio(slot));
        CHECK_NEAR(octave, std::round(octave), 1.0e-12);
    }
    // 32.7 Hz .. 16.7 kHz: ten octaves; the Bands count (1) is ignored.
    CHECK(active == 10);

    // The sub-octave really boosts.
    CHECK(bankGainDb(bank, root * 0.25, fs, 2.0) > 5.0);
}

TEST_CASE("BandBank: cut bank")
{
    const double fs = 48000.0;
    const double root = noteToHz(38.0); // D2
    BandBank::Settings s;
    s.series = SeriesType::harmonic;
    s.bands = 6;
    s.gainDb = -12.0;
    s.q = 8.0;
    s.overtones = 0.5;

    BandBank bank;
    bank.prepare(fs, BandBank::Kind::cut);
    bank.setSettings(s);
    bank.setTargetFrequency(root, 0.0);
    runSilence(bank, 64);

    CHECK(bank.activeBandCount() == 6);
    CHECK_NEAR(bank.slotGainDb(0), -12.0, 1.0e-12);
    for (int slot = 1; slot < 6; ++slot)
        CHECK_NEAR(bank.slotGainDb(slot), -6.0, 1.0e-12);

    CHECK_NEAR(bankGainDb(bank, root, fs, 1.0), expectedGainDb(bank, s, root, root, fs), 0.1);
    CHECK(bankGainDb(bank, root, fs, 1.0) < -11.5);
    CHECK(bankGainDb(bank, root * 3.0, fs, 1.0) < -5.5);

    // Overtones at 0 leaves only the root band.
    s.overtones = 0.0;
    bank.reset();
    bank.setSettings(s);
    runSilence(bank, 64);
    CHECK(bank.activeBandCount() == 1);
}

TEST_CASE("BandBank: a neutral bank is bit-transparent and does no work")
{
    const double fs = 48000.0;
    BandBank bank;
    bank.prepare(fs, BandBank::Kind::boost);
    bank.setSettings(boostSettings(0.0, 12.0, 32));
    bank.setTargetFrequency(100.0, 0.0);

    Random random(11);
    std::vector<double> in(4096), out;
    for (auto& x : in)
        x = random.bipolar();
    out = in;
    bank.process(out.data(), nullptr, static_cast<int>(out.size()));

    bool identical = true;
    for (size_t i = 0; i < in.size(); ++i)
        identical = identical && in[i] == out[i];
    CHECK(identical);
    CHECK(bank.activeBandCount() == 0);
    CHECK(bank.isNeutral());
}

TEST_CASE("BandBank: constant-width mode narrows the upper bands")
{
    const double fs = 48000.0;
    const double root = 100.0;
    BandBank::Settings s = boostSettings(12.0, 10.0, 8);
    s.tiltDbPerOct = 0.0;
    s.rangeOct = 6.0;

    BandBank bank;
    bank.prepare(fs, BandBank::Kind::boost);
    bank.setTargetFrequency(root, 0.0);

    // 3 % above the 8th harmonic: inside a Q 10 bell, well outside a Q 80 bell.
    const double probe = root * 8.0 * 1.03;

    s.qMode = QMode::constantQ;
    bank.setSettings(s);
    runSilence(bank, 64);
    const double wide = bankGainDb(bank, probe, fs, 1.0);

    s.qMode = QMode::constantWidth;
    bank.reset();
    bank.setSettings(s);
    runSilence(bank, 64);
    const double narrow = bankGainDb(bank, probe, fs, 1.0);

    CHECK(wide > narrow + 4.0);
    // The centre itself keeps its gain (12 dB x the range weight three octaves up).
    CHECK_NEAR(bank.slotGainDb(7), 12.0 * rangeWeight(800.0, 100.0, 6.0), 1.0e-9);
    CHECK_NEAR(bankGainDb(bank, root * 8.0, fs, 1.5), bank.slotGainDb(7), 0.3);
}

TEST_CASE("BandBank: control rate scales with the sample rate")
{
    BandBank bank;
    bank.prepare(44100.0, BandBank::Kind::boost);
    CHECK(bank.controlIntervalSamples() == 16);
    bank.prepare(48000.0, BandBank::Kind::boost);
    CHECK(bank.controlIntervalSamples() == 16);
    bank.prepare(88200.0, BandBank::Kind::boost);
    CHECK(bank.controlIntervalSamples() == 32);
    bank.prepare(96000.0, BandBank::Kind::boost);
    CHECK(bank.controlIntervalSamples() == 32);
    bank.prepare(192000.0, BandBank::Kind::boost);
    CHECK(bank.controlIntervalSamples() == 64);
}

TEST_CASE("BandBank: gain changes ramp over 20 ms without steps")
{
    const double fs = 48000.0;
    const double root = 100.0;
    BandBank bank;
    bank.prepare(fs, BandBank::Kind::boost);
    BandBank::Settings s = boostSettings(0.0, 4.0, 1);
    bank.setSettings(s);
    bank.setTargetFrequency(root, 0.0);

    // Steady sine at the band centre; switch the boost in and watch the envelope.
    const int total = static_cast<int>(0.4 * fs);
    const int switchAt = static_cast<int>(0.1 * fs);
    std::vector<double> out(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i)
    {
        if (i == switchAt)
        {
            s.gainDb = 12.0;
            bank.setSettings(s);
        }
        double x = 0.25 * std::sin(kTwoPi * root * i / fs);
        bank.process(&x, nullptr, 1);
        out[static_cast<size_t>(i)] = x;
    }

    // Before the switch: unity. Long after: +12 dB.
    CHECK_NEAR(gainToDb(toneAmplitude(out, 0, static_cast<size_t>(switchAt), root, fs) / 0.25), 0.0, 0.01);
    CHECK_NEAR(gainToDb(toneAmplitude(out, out.size() - 4800, out.size(), root, fs) / 0.25), 12.0, 0.1);
    CHECK_NEAR(bank.slotGainDb(0), 12.0, 1.0e-12);

    // The band gain itself reaches its target after 20 ms.
    BandBank timing;
    timing.prepare(fs, BandBank::Kind::boost);
    timing.setSettings(boostSettings(0.0, 4.0, 1));
    timing.setTargetFrequency(root, 0.0);
    runSilence(timing, 64);
    timing.setSettings(boostSettings(12.0, 4.0, 1));
    runSilence(timing, 480);
    CHECK_NEAR(timing.slotGainDb(0), 6.0, 0.3); // half way after 10 ms
    runSilence(timing, 480 + 32);
    CHECK_NEAR(timing.slotGainDb(0), 12.0, 1.0e-12);

    // No step: the second difference of the output stays close to that of a
    // pure sine of the final amplitude.
    const double finalAmplitude = 0.25 * dbToGain(12.0);
    const double w = kTwoPi * root / fs;
    const double sineSecondDifference = finalAmplitude * w * w;
    double worst = 0.0;
    for (size_t i = 2; i < out.size(); ++i)
        worst = std::max(worst, std::abs(out[i] - 2.0 * out[i - 1] + out[i - 2]));
    CHECK(worst < 3.0 * sineSecondDifference);
}

TEST_CASE("BandBank: pitch ramp is linear in pitch and lands on time")
{
    const double fs = 48000.0;
    BandBank bank;
    bank.prepare(fs, BandBank::Kind::boost);
    bank.setSettings(boostSettings(9.0, 12.0, 4));
    bank.setTargetFrequency(100.0, 0.0);
    runSilence(bank, 64);
    CHECK_NEAR(bank.currentRootHz(), 100.0, 1.0e-9);

    bank.setTargetFrequency(400.0, 0.100); // two octaves in 100 ms
    CHECK(bank.isRootMoving());
    runSilence(bank, 2400);
    CHECK_NEAR(std::log2(bank.currentRootHz() / 100.0), 1.0, 0.02); // one octave after 50 ms
    runSilence(bank, 2400 - 64);
    CHECK(bank.isRootMoving());
    runSilence(bank, 64 + 16);
    CHECK(!bank.isRootMoving());
    CHECK_NEAR(bank.currentRootHz(), 400.0, 1.0e-9);
    CHECK_NEAR(bank.targetRootHz(), 400.0, 1.0e-9);

    // A zero-length ramp jumps.
    bank.setTargetFrequency(50.0, 0.0);
    runSilence(bank, 1);
    CHECK_NEAR(bank.currentRootHz(), 50.0, 1.0e-9);
}

TEST_CASE("BandBank: output does not depend on the host block size, even while moving")
{
    const double fs = 48000.0;
    Random random(5);
    std::vector<double> in(6000);
    for (auto& x : in)
        x = 0.5 * random.bipolar();

    std::vector<double> reference;
    for (int blockSize : { 4096, 1, 7, 64, 513 })
    {
        BandBank bank;
        bank.prepare(fs, BandBank::Kind::boost);
        bank.setSettings(boostSettings(12.0, 20.0, 8));
        bank.setTargetFrequency(80.0, 0.0);

        std::vector<double> left = in, right = in;
        for (auto& x : right)
            x = -x;

        // 1000 samples static, then a pitch ramp and a gain change start together.
        int position = 0;
        bool started = false;
        while (position < static_cast<int>(in.size()))
        {
            if (!started && position >= 1000)
            {
                bank.setTargetFrequency(200.0, 0.050);
                bank.setSettings(boostSettings(18.0, 35.0, 12));
                started = true;
            }
            int n = std::min(blockSize, static_cast<int>(in.size()) - position);
            if (!started)
                n = std::min(n, 1000 - position);
            bank.process(left.data() + position, right.data() + position, n);
            position += n;
        }

        // Stereo channels are independent and identical filters.
        bool mirrored = true;
        for (size_t i = 0; i < left.size(); ++i)
            mirrored = mirrored && left[i] == -right[i];
        CHECK(mirrored);

        if (reference.empty())
        {
            reference = left;
        }
        else
        {
            bool identical = true;
            for (size_t i = 0; i < left.size(); ++i)
                identical = identical && left[i] == reference[i];
            CHECK_MSG(identical, "block size " + std::to_string(blockSize));
        }
    }
}

TEST_CASE("BandBank: switching the series fades bands out and back in without a click")
{
    const double fs = 48000.0;
    const double root = 110.0;
    BandBank bank;
    bank.prepare(fs, BandBank::Kind::boost);
    BandBank::Settings s = boostSettings(12.0, 12.0, 8, SeriesType::harmonic);
    bank.setSettings(s);
    bank.setTargetFrequency(root, 0.0);

    // A low sine well below the first band: any click shows up against it.
    const double probe = 30.0;
    const int total = static_cast<int>(0.5 * fs);
    std::vector<double> out(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i)
    {
        if (i == total / 3)
        {
            s.series = SeriesType::drum;
            bank.setSettings(s);
        }
        if (i == 2 * total / 3)
        {
            s.series = SeriesType::octaves;
            bank.setSettings(s);
        }
        double x = 0.5 * std::sin(kTwoPi * probe * i / fs);
        bank.process(&x, nullptr, 1);
        out[static_cast<size_t>(i)] = x;
    }

    for (int slot = 0; slot < kOctaveSlots; ++slot)
        CHECK_NEAR(bank.slotRatio(slot), seriesRatio(SeriesType::octaves, slot), 1.0e-10);

    const double w = kTwoPi * probe / fs;
    double worst = 0.0;
    for (size_t i = 2; i < out.size(); ++i)
        worst = std::max(worst, std::abs(out[i] - 2.0 * out[i - 1] + out[i - 2]));
    // A pure 30 Hz sine at this level has a second difference of 0.5 w^2 = 7.7e-6.
    // The boosted octave at 27.5 Hz raises the level; a click would be orders larger.
    CHECK(worst < 40.0 * 0.5 * w * w);
}
