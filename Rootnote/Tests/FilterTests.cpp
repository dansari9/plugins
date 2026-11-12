#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/Series.h"
#include "dsp/SvfBell.h"
#include "dsp/Weighting.h"

#include <string>

using namespace rootnote;
using namespace testsignals;

namespace
{
std::string label(double hz, double fs, double q, double db)
{
    return std::to_string(hz) + " Hz @ " + std::to_string(static_cast<int>(fs)) + ", Q " + std::to_string(q)
         + ", " + std::to_string(db) + " dB";
}
} // namespace

TEST_CASE("SvfBell: gain at the centre equals the set gain within 0.1 dB")
{
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    const double centres[] = { 30.0, 46.25, 100.0, 440.0, 1000.0, 5000.0, 15000.0 };
    const double gains[] = { 24.0, 9.0, 3.0, -6.0, -12.0, -30.0 };
    const double qs[] = { 0.7, 12.0 };

    for (double fs : rates)
        for (double hz : centres)
            for (double db : gains)
                for (double q : qs)
                {
                    SvfBell bell;
                    bell.set(hz, q, db, fs);
                    const double settle = std::max(0.05, 3.0 * bellRingSeconds(hz, q, std::max(db, 0.0)));
                    const double measured = measureGainDb([&](double x) { return bell.process(x); }, hz, fs, settle);
                    CHECK_NEAR_MSG(measured, db, 0.1, label(hz, fs, q, db));
                }
}

TEST_CASE("SvfBell: narrow bells at the bottom of the range stay exact")
{
    for (double fs : { 44100.0, 192000.0 })
        for (double hz : { 20.0, 30.0, 41.2 })
        {
            const double q = 60.0;
            const double db = 12.0;
            SvfBell bell;
            bell.set(hz, q, db, fs);
            const double measured = measureGainDb([&](double x) { return bell.process(x); }, hz, fs,
                                                  3.0 * bellRingSeconds(hz, q, db));
            CHECK_NEAR_MSG(measured, db, 0.1, label(hz, fs, q, db));
        }
}

TEST_CASE("SvfBell: response off the centre follows the analytic curve")
{
    const double fs = 48000.0;
    const double centre = 200.0;
    const double q = 4.0;
    const double db = 12.0;
    for (double hz : { 50.0, 100.0, 150.0, 180.0, 220.0, 300.0, 800.0, 4000.0 })
    {
        SvfBell bell;
        bell.set(centre, q, db, fs);
        const double measured = measureGainDb([&](double x) { return bell.process(x); }, hz, fs, 0.3);
        CHECK_NEAR_MSG(measured, bellResponseDb(hz, centre, q, db, fs), 0.02, std::to_string(hz));
    }

    // Far from the centre a bell is flat, and boost and cut mirror each other.
    CHECK_NEAR(bellResponseDb(20.0, 2000.0, 12.0, 24.0, fs), 0.0, 0.05);
    for (double hz : { 150.0, 190.0, 200.0, 260.0 })
        CHECK_NEAR(bellResponseDb(hz, centre, q, 9.0, fs), -bellResponseDb(hz, centre, q, -9.0, fs), 1.0e-9);
}

TEST_CASE("SvfBell: 0 dB is transparent")
{
    Random random(7);
    for (double fs : { 44100.0, 192000.0 })
    {
        SvfBell bell;
        bell.set(100.0, 12.0, 0.0, fs);
        double worst = 0.0;
        for (int i = 0; i < 20000; ++i)
        {
            const double x = random.bipolar();
            worst = std::max(worst, std::abs(bell.process(x) - x));
        }
        CHECK(worst <= 1.0e-6);
        CHECK(worst == 0.0); // in fact exact: the band-pass term is multiplied by zero
    }
}

TEST_CASE("SvfBell: ring time matches the formula")
{
    // Excite the bell at its centre, stop the input and time the 60 dB decay.
    const double fs = 48000.0;
    const double hz = 100.0;
    const double q = 30.0;
    const double db = 12.0;
    SvfBell bell;
    bell.set(hz, q, db, fs);

    const int drive = static_cast<int>(2.0 * fs);
    double peak = 0.0;
    for (int i = 0; i < drive; ++i)
        peak = std::max(peak, std::abs(bell.process(0.25 * std::sin(kTwoPi * hz * i / fs))));

    int lastAbove = 0;
    for (int i = 0; i < static_cast<int>(4.0 * fs); ++i)
        if (std::abs(bell.process(0.0)) > peak * 0.001)
            lastAbove = i;

    const double expected = bellRingSeconds(hz, q, db);
    CHECK_NEAR(lastAbove / fs, expected, 0.15 * expected);
}

TEST_CASE("Series: ratios")
{
    for (int slot = 0; slot < kMaxSlots; ++slot)
        CHECK(seriesRatio(SeriesType::harmonic, slot) == static_cast<double>(slot + 1));

    // The first twelve membrane modes, as listed in the build plan (rounded there to 3 places).
    const double plan[12] = { 1.000, 1.594, 2.136, 2.296, 2.653, 2.918, 3.156, 3.501, 3.600, 3.652, 4.060, 4.154 };
    for (int slot = 0; slot < 12; ++slot)
        CHECK_NEAR(seriesRatio(SeriesType::drum, slot), plan[slot], 0.006 * plan[slot]);
    for (int slot = 1; slot < kMaxSlots; ++slot)
        CHECK(kDrumRatios[slot] > kDrumRatios[slot - 1]);
    CHECK(kDrumRatios[0] == 1.0);

    CHECK(seriesRatio(SeriesType::octaves, kOctaveRootSlot) == 1.0);
    CHECK(seriesRatio(SeriesType::octaves, kOctaveRootSlot - 1) == 0.5);
    CHECK(seriesRatio(SeriesType::octaves, kOctaveRootSlot + 3) == 8.0);
    CHECK(seriesRatio(SeriesType::octaves, 0) == 1.0 / 64.0);
    CHECK(seriesRatio(SeriesType::octaves, kOctaveSlots - 1) == 4096.0);

    CHECK(seriesSlotEnabled(SeriesType::harmonic, 7, 8));
    CHECK(!seriesSlotEnabled(SeriesType::harmonic, 8, 8));
    CHECK(seriesSlotEnabled(SeriesType::drum, 0, 1));
    CHECK(!seriesSlotEnabled(SeriesType::drum, 1, 1));
    CHECK(seriesSlotEnabled(SeriesType::octaves, kOctaveSlots - 1, 1)); // Bands is ignored for octaves
}

TEST_CASE("Series: the octave slots cover every root at every sample rate")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (double note = 12.0 - 0.5; note <= 96.5; note += 0.5)
        {
            const double root = 440.0 * std::exp2((note - 69.0) / 12.0);
            // Every octave inside 20 Hz .. 0.45 fs must have a slot.
            for (int n = -12; n <= 16; ++n)
            {
                const double hz = root * std::exp2(static_cast<double>(n));
                if (hz < 20.0 || hz > 0.45 * fs)
                    continue;
                const int slot = n + kOctaveRootSlot;
                CHECK(slot >= 0 && slot < kOctaveSlots);
            }
        }
}

TEST_CASE("Weighting: range is a Gaussian over octaves")
{
    CHECK_NEAR(rangeWeight(100.0, 100.0, 1.5), 1.0, 1.0e-12);
    CHECK_NEAR(rangeWeight(200.0, 100.0, 1.0), std::exp(-0.5), 1.0e-12);
    CHECK_NEAR(rangeWeight(50.0, 100.0, 1.0), std::exp(-0.5), 1.0e-12);
    CHECK_NEAR(rangeWeight(400.0, 100.0, 1.0), std::exp(-2.0), 1.0e-12);
    CHECK_NEAR(rangeWeight(200.0, 100.0, 2.0), std::exp(-0.125), 1.0e-12);
    CHECK_NEAR(rangeWeight(800.0, 100.0, 1.5), std::exp(-0.5 * 4.0), 1.0e-12);
    CHECK(rangeWeight(6400.0, 100.0, 0.1) < 1.0e-100);
}

TEST_CASE("Weighting: odd/even blend")
{
    // Fundamental is always full.
    for (double b : { 0.0, 0.25, 0.5, 0.75, 1.0 })
        CHECK(blendWeight(1, b) == 1.0);

    // Centre: both full.
    for (int k = 2; k <= 32; ++k)
        CHECK(blendWeight(k, 0.5) == 1.0);

    // Fully left: odd only. Fully right: even only.
    for (int k = 2; k <= 32; ++k)
    {
        CHECK(blendWeight(k, 0.0) == (k % 2 == 1 ? 1.0 : 0.0));
        CHECK(blendWeight(k, 1.0) == (k % 2 == 0 ? 1.0 : 0.0));
    }

    CHECK_NEAR(blendWeight(2, 0.25), 0.5, 1.0e-12);
    CHECK_NEAR(blendWeight(3, 0.25), 1.0, 1.0e-12);
    CHECK_NEAR(blendWeight(3, 0.75), 0.5, 1.0e-12);
    CHECK_NEAR(blendWeight(2, 0.75), 1.0, 1.0e-12);
}

TEST_CASE("Weighting: tilt")
{
    CHECK_NEAR(tiltWeight(100.0, 100.0, -3.0), 1.0, 1.0e-12);
    CHECK_NEAR(tiltWeight(200.0, 100.0, -3.0), std::pow(10.0, -3.0 / 20.0), 1.0e-12);
    CHECK_NEAR(tiltWeight(400.0, 100.0, -6.0), std::pow(10.0, -12.0 / 20.0), 1.0e-12);
    CHECK_NEAR(tiltWeight(200.0, 100.0, 6.0), std::pow(10.0, 6.0 / 20.0), 1.0e-12);
    CHECK_NEAR(tiltWeight(50.0, 100.0, -3.0), std::pow(10.0, 3.0 / 20.0), 1.0e-12);
    CHECK_NEAR(tiltWeight(800.0, 100.0, 0.0), 1.0, 1.0e-12);
}

TEST_CASE("Weighting: spectrum edges")
{
    const double fs = 48000.0;
    CHECK(edgeWeight(17.9, fs) == 0.0);
    CHECK(edgeWeight(18.0, fs) == 0.0);
    CHECK(edgeWeight(19.0, fs) > 0.0 && edgeWeight(19.0, fs) < 1.0);
    CHECK(edgeWeight(20.0, fs) == 1.0);
    CHECK(edgeWeight(1000.0, fs) == 1.0);

    const double top = 0.45 * fs;
    const double fadeStart = top * std::exp2(-1.0 / 6.0);
    CHECK(edgeWeight(fadeStart * 0.999, fs) == 1.0);
    CHECK(edgeWeight(fadeStart * 1.02, fs) < 1.0);
    CHECK(edgeWeight(top * 0.999, fs) > 0.0);
    CHECK(edgeWeight(top * 0.999, fs) < 0.01);
    CHECK(edgeWeight(top, fs) == 0.0);
    CHECK(edgeWeight(top * 1.01, fs) == 0.0);

    // Monotonic through both fades.
    double previous = 0.0;
    for (double hz = 18.0; hz <= 20.0; hz += 0.01)
    {
        CHECK(edgeWeight(hz, fs) >= previous);
        previous = edgeWeight(hz, fs);
    }
    previous = 1.0;
    for (double hz = fadeStart; hz <= top; hz += 5.0)
    {
        CHECK(edgeWeight(hz, fs) <= previous);
        previous = edgeWeight(hz, fs);
    }
}

TEST_CASE("Weighting: boost slot gain combines range, blend and tilt and is clamped")
{
    const double fs = 48000.0;
    BoostShape shape;
    shape.series = SeriesType::harmonic;
    shape.bands = 8;
    shape.blend = 0.5;
    shape.focusLink = true;
    shape.rangeOct = 1.5;
    shape.tiltDbPerOct = -3.0;

    const double root = 65.4064;
    for (int slot = 0; slot < 8; ++slot)
    {
        const double octaves = std::log2(static_cast<double>(slot + 1));
        const double expected = 9.0 * std::exp(-0.5 * (octaves / 1.5) * (octaves / 1.5)) * std::pow(10.0, -3.0 * octaves / 20.0);
        CHECK_NEAR(boostSlotGainDb(shape, 9.0, slot, root, fs), expected, 1.0e-9);
    }
    for (int slot = 8; slot < kMaxSlots; ++slot)
        CHECK(boostSlotGainDb(shape, 9.0, slot, root, fs) == 0.0);

    // Unlinked focus: the peak of the weighting moves to the focus.
    shape.focusLink = false;
    shape.focusHz = root * 4.0;
    shape.tiltDbPerOct = 0.0;
    CHECK_NEAR(boostSlotGainDb(shape, 9.0, 3, root, fs), 9.0, 1.0e-9);
    CHECK(boostSlotGainDb(shape, 9.0, 0, root, fs) < boostSlotGainDb(shape, 9.0, 1, root, fs));

    // Positive tilt cannot push a band past +24 dB.
    shape.focusLink = true;
    shape.rangeOct = 6.0;
    shape.tiltDbPerOct = 6.0;
    for (int slot = 0; slot < 8; ++slot)
        CHECK(boostSlotGainDb(shape, 24.0, slot, root, fs) <= 24.0);
    CHECK_NEAR(boostSlotGainDb(shape, 24.0, 7, root, fs), 24.0, 1.0e-9);

    // Blend only acts on the harmonic series.
    shape.tiltDbPerOct = 0.0;
    shape.blend = 0.0;
    CHECK(boostSlotGainDb(shape, 9.0, 1, root, fs) == 0.0);
    shape.series = SeriesType::drum;
    CHECK(boostSlotGainDb(shape, 9.0, 1, root, fs) > 0.0);
}

TEST_CASE("Weighting: cut slot gain")
{
    const double fs = 48000.0;
    const double root = 73.4162; // D2
    CHECK_NEAR(cutSlotGainDb(SeriesType::harmonic, 6, -12.0, 0.5, 0, root, fs), -12.0, 1.0e-12);
    for (int slot = 1; slot < 6; ++slot)
        CHECK_NEAR(cutSlotGainDb(SeriesType::harmonic, 6, -12.0, 0.5, slot, root, fs), -6.0, 1.0e-12);
    CHECK(cutSlotGainDb(SeriesType::harmonic, 6, -12.0, 0.5, 6, root, fs) == 0.0);
    CHECK(cutSlotGainDb(SeriesType::harmonic, 6, -12.0, 0.0, 3, root, fs) == 0.0);
    CHECK_NEAR(cutSlotGainDb(SeriesType::harmonic, 6, -12.0, 1.0, 3, root, fs), -12.0, 1.0e-12);

    // Octaves: the root slot takes the full depth, the other octaves the overtone share.
    CHECK_NEAR(cutSlotGainDb(SeriesType::octaves, 1, -18.0, 0.5, kOctaveRootSlot, root, fs), -18.0, 1.0e-12);
    CHECK_NEAR(cutSlotGainDb(SeriesType::octaves, 1, -18.0, 0.5, kOctaveRootSlot + 2, root, fs), -9.0, 1.0e-12);
    CHECK_NEAR(cutSlotGainDb(SeriesType::octaves, 1, -18.0, 0.5, kOctaveRootSlot - 1, root, fs), -9.0, 1.0e-12);
    CHECK(cutSlotGainDb(SeriesType::octaves, 1, -18.0, 0.5, kOctaveRootSlot - 3, root, fs) == 0.0); // 9.2 Hz: below 18 Hz
}

TEST_CASE("Weighting: Q modes")
{
    CHECK(slotQ(QMode::constantQ, 12.0, 5.0) == 12.0);
    CHECK(slotQ(QMode::constantWidth, 12.0, 1.0) == 12.0);
    CHECK(slotQ(QMode::constantWidth, 12.0, 5.0) == 60.0);
    CHECK(slotQ(QMode::constantWidth, 12.0, 0.5) == 6.0);
    CHECK(slotQ(QMode::constantWidth, 0.5, 1.0 / 64.0) == 0.25);

    // Constant width: every band rings as long as the root.
    const double root = 50.0;
    const double reference = bellRingSeconds(root, 12.0, 9.0);
    for (int k = 1; k <= 32; ++k)
        CHECK_NEAR(bellRingSeconds(root * k, slotQ(QMode::constantWidth, 12.0, k), 9.0), reference, 1.0e-9);
}
