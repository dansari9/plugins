#include "TestFramework.h"
#include "TestSignals.h"

#include "analysis/AnalysisWorker.h"
#include "analysis/Tuner.h"
#include "dsp/NoteMath.h"

#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace rootnote;
using namespace testsignals;

namespace
{
double centsBetween(double hz, double referenceHz)
{
    return 1200.0 * std::log2(hz / referenceHz);
}

void feed(Tuner& tuner, const std::vector<float>& signal, size_t blockSize = 480)
{
    for (size_t position = 0; position < signal.size(); position += blockSize)
        tuner.process(signal.data() + position, std::min(blockSize, signal.size() - position));
}

/** One kick after 0.2 s of silence, with a second of room to ring. */
std::vector<float> oneKick(const KickSpec& kick, double fs, Random& random)
{
    std::vector<float> signal(static_cast<size_t>(1.2 * fs), 0.0f);
    addKick(signal, static_cast<size_t>(0.2 * fs), kick, fs, random);
    return signal;
}

/** Kicks every half second, each at its own pitch. */
std::vector<float> kickRow(const std::vector<double>& pitches, double fs, Random& random)
{
    std::vector<float> signal(static_cast<size_t>((0.5 * static_cast<double>(pitches.size()) + 0.7) * fs), 0.0f);
    for (size_t i = 0; i < pitches.size(); ++i)
    {
        KickSpec kick;
        kick.hz = pitches[i];
        kick.decayTau = 0.08;
        addKick(signal, static_cast<size_t>((0.2 + 0.5 * static_cast<double>(i)) * fs), kick, fs, random);
    }
    return signal;
}

/** Feeds the worker the way an audio callback does: 512-sample blocks at about real-time speed x 8. */
void play(AnalysisWorker& worker, const std::vector<float>& signal, double fs, bool stereo = false)
{
    const auto blockTime = std::chrono::duration<double>(512.0 / fs / 8.0);
    for (size_t position = 0; position < signal.size(); position += 512)
    {
        const float* left = signal.data() + position;
        worker.push(left, stereo ? left : nullptr, static_cast<int>(std::min<size_t>(512, signal.size() - position)));
        std::this_thread::sleep_for(blockTime);
    }
}

bool waitFor(const AnalysisWorker& worker, uint32_t pitchedHits)
{
    for (int attempt = 0; attempt < 500; ++attempt)
    {
        if (worker.reading().pitchedHits >= pitchedHits)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

TEST_CASE("Tuner: settled pitch of kicks within 10 cents at every sample rate")
{
    double worst = 0.0;
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        Tuner tuner;
        tuner.prepare(fs);
        CHECK(tuner.onsetRate() >= 22050.0 && tuner.onsetRate() <= 24000.0);
        CHECK_NEAR(tuner.pitchRate(), tuner.onsetRate() / 6.0, 1.0e-9);

        Random random(static_cast<uint32_t>(fs));
        for (double hz : { 30.0, 41.2, 55.0, 73.4, 110.0, 200.0 })
            for (double drop : { 2.0, 12.0 })
            {
                KickSpec kick;
                kick.hz = hz;
                kick.dropSemitones = drop;

                tuner.reset();
                feed(tuner, oneKick(kick, fs, random));
                const TunerReading reading = tuner.reading();
                const std::string name = std::to_string(hz) + " Hz, drop " + std::to_string(drop) + " st at "
                                             + std::to_string(static_cast<int>(fs));
                CHECK_MSG(reading.valid && reading.onsets == 1u && reading.pitchedHits == 1u, name);
                CHECK_NEAR_MSG(centsBetween(reading.hz, hz), 0.0, 10.0, name);
                CHECK_NEAR_MSG(reading.hz, reading.lastHitHz, 1.0e-9, name); // a single hit is its own median
                worst = std::max(worst, std::abs(centsBetween(reading.hz, hz)));
            }
    }
    std::printf("    worst kick error through the whole tuner: %.2f cents\n", worst);
}

TEST_CASE("Tuner: toms and snares within 15 cents")
{
    const double fs = 48000.0;
    Tuner tuner;
    tuner.prepare(fs);
    Random random(12);

    for (double hz : { 65.4, 98.0, 146.8, 196.0 })
        for (double noise : { 0.0, 0.5 })
        {
            MembraneSpec drum;
            drum.hz = hz;
            drum.noise = noise;
            std::vector<float> signal(static_cast<size_t>(1.2 * fs), 0.0f);
            addMembrane(signal, static_cast<size_t>(0.2 * fs), drum, fs, random);

            tuner.reset();
            feed(tuner, signal);
            const std::string name = std::to_string(hz) + " Hz, noise " + std::to_string(noise);
            CHECK_MSG(tuner.reading().valid, name);
            CHECK_NEAR_MSG(centsBetween(tuner.reading().hz, hz), 0.0, 15.0, name);
        }
}

TEST_CASE("Tuner: the reading is the median of the last four hits")
{
    const double fs = 48000.0;
    Tuner tuner;
    tuner.prepare(fs);
    Random random(5);

    // Three hits at 55 Hz, one stray at 82.4 Hz, then 55 Hz again.
    feed(tuner, kickRow({ 55.0, 55.0, 55.0, 82.4 }, fs, random));
    TunerReading reading = tuner.reading();
    CHECK(reading.pitchedHits == 4u);
    CHECK_NEAR(centsBetween(reading.lastHitHz, 82.4), 0.0, 10.0);
    CHECK_NEAR(centsBetween(reading.hz, 55.0), 0.0, 10.0);

    // Once the drum really has changed, the reading follows: after three of
    // the last four hits are at the new pitch the median is the new pitch.
    tuner.resetStream();
    feed(tuner, kickRow({ 82.4, 82.4 }, fs, random));
    reading = tuner.reading();
    CHECK(reading.pitchedHits == 6u);
    CHECK_NEAR(centsBetween(reading.hz, 82.4), 0.0, 10.0);
}

TEST_CASE("Tuner: a hit is analysed up to the next one, so fast patterns still read true")
{
    const double fs = 48000.0;
    Tuner tuner;
    tuner.prepare(fs);
    tuner.keepOnsetTimes(true);
    Random random(6);

    // Sixteenth notes at 150 BPM: 100 ms apart, shorter than the 120 ms coarse window.
    std::vector<float> signal(static_cast<size_t>(3.0 * fs), 0.0f);
    std::vector<double> truth;
    for (int i = 0; i < 20; ++i)
    {
        KickSpec kick;
        kick.hz = 110.0;
        kick.decayTau = 0.03;
        const double t = 0.2 + 0.1 * i;
        addKick(signal, static_cast<size_t>(t * fs), kick, fs, random);
        truth.push_back(t);
    }
    feed(tuner, signal);

    const TunerReading reading = tuner.reading();
    CHECK(reading.onsets == 20u);
    CHECK(reading.pitchedHits >= 18u);
    CHECK_NEAR(centsBetween(reading.hz, 110.0), 0.0, 15.0);

    // Onset times are reported in seconds of input, through both decimators.
    REQUIRE(tuner.onsetTimes().size() == truth.size());
    for (size_t i = 0; i < truth.size(); ++i)
        CHECK_NEAR(tuner.onsetTimes()[i], truth[i], 0.010);
}

TEST_CASE("Tuner: the reading does not depend on how the audio is cut into blocks")
{
    const double fs = 44100.0;
    Random random(7);
    const std::vector<float> signal = kickRow({ 49.0, 49.0, 61.7 }, fs, random);

    double reference = 0.0;
    for (size_t blockSize : { size_t(64), size_t(1), size_t(333), size_t(4096), signal.size() })
    {
        Tuner tuner;
        tuner.prepare(fs);
        feed(tuner, signal, blockSize);
        CHECK(tuner.reading().pitchedHits == 3u);
        if (reference == 0.0)
            reference = tuner.reading().hz;
        CHECK_MSG(tuner.reading().hz == reference, "block size " + std::to_string(blockSize));
    }
}

TEST_CASE("Tuner: silence and noise give no reading; reset forgets, resetStream keeps the reading")
{
    const double fs = 48000.0;
    Tuner tuner;
    tuner.prepare(fs);
    Random random(9);

    feed(tuner, std::vector<float>(static_cast<size_t>(fs), 0.0f));
    CHECK(!tuner.reading().valid && tuner.reading().onsets == 0u);

    // Bursts of noise are hits (onsets) but have no pitch.
    std::vector<float> bursts(static_cast<size_t>(2.0 * fs), 0.0f);
    for (double t : { 0.3, 0.8, 1.3 })
        for (size_t i = 0; i < static_cast<size_t>(0.2 * fs); ++i)
            bursts[static_cast<size_t>(t * fs) + i] = static_cast<float>(0.5 * random.bipolar() * std::exp(-static_cast<double>(i) / (0.05 * fs)));
    feed(tuner, bursts);
    CHECK(tuner.reading().onsets == 3u);
    CHECK(!tuner.reading().valid && tuner.reading().pitchedHits == 0u);

    feed(tuner, kickRow({ 55.0 }, fs, random));
    CHECK(tuner.reading().valid);
    const double hz = tuner.reading().hz;

    tuner.resetStream();
    CHECK(tuner.reading().valid && tuner.reading().hz == hz);
    tuner.reset();
    CHECK(!tuner.reading().valid && tuner.reading().onsets == 0u && tuner.reading().hz == 0.0);
}

TEST_CASE("AnalysisWorker: reads the pitch on its own thread, only while enabled")
{
    const double fs = 48000.0;
    Random random(10);
    const std::vector<float> kicks = kickRow({ 55.0, 55.0, 55.0, 55.0 }, fs, random);

    AnalysisWorker worker;
    worker.setSampleRate(fs);
    CHECK(!worker.isEnabled());

    // Disabled: push() drops the audio and nothing is analysed.
    play(worker, kicks, fs);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(!worker.reading().valid && worker.reading().onsets == 0u);

    worker.enable(true);
    worker.enable(true); // a second user
    worker.enable(false);
    CHECK(worker.isEnabled());
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // the idle worker polls every 100 ms
    play(worker, kicks, fs);
    CHECK(waitFor(worker, 3));
    const TunerReading reading = worker.reading();
    CHECK(reading.valid);
    CHECK_NEAR(centsBetween(reading.hz, 55.0), 0.0, 10.0);
    CHECK(reading.onsets >= reading.pitchedHits);

    // The last reading stays up after the display closes.
    worker.enable(false);
    CHECK(!worker.isEnabled());
    CHECK(worker.reading().valid);
}

TEST_CASE("AnalysisWorker: non-finite input does not stop subsequent pitch readings")
{
    const double fs = 48000.0;
    for (bool stereo : { false, true })
    {
        AnalysisWorker worker;
        worker.setSampleRate(fs);
        worker.enable(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        Random random(12);
        auto kicks = kickRow({ 55.0, 55.0, 55.0, 55.0 }, fs, random);
        kicks[128] = std::numeric_limits<float>::quiet_NaN();
        kicks[129] = std::numeric_limits<float>::infinity();
        kicks[130] = -std::numeric_limits<float>::infinity();
        play(worker, kicks, fs, stereo);
        CHECK(waitFor(worker, 3));
        CHECK(worker.reading().valid);
        CHECK_NEAR(centsBetween(worker.reading().hz, 55.0), 0.0, 10.0);
        worker.enable(false);
    }
}

TEST_CASE("AnalysisWorker: recovers from a flooded FIFO and starts afresh on a new sample rate")
{
    Random random(11);
    AnalysisWorker worker;
    worker.setSampleRate(48000.0);
    worker.enable(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Far faster than real time, as an offline bounce does: blocks are dropped, never torn.
    const std::vector<float> flood = kickRow(std::vector<double>(40, 70.0), 48000.0, random);
    for (size_t position = 0; position + 512 <= flood.size(); position += 512)
        worker.push(flood.data() + position, flood.data() + position, 512);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Afterwards it reads true again, and stereo input is summed to mono.
    const uint32_t before = worker.reading().pitchedHits;
    const std::vector<float> kicks = kickRow({ 98.0, 98.0, 98.0, 98.0, 98.0, 98.0 }, 48000.0, random);
    const auto blockTime = std::chrono::duration<double>(512.0 / 48000.0 / 8.0);
    for (size_t position = 0; position + 512 <= kicks.size(); position += 512)
    {
        worker.push(kicks.data() + position, kicks.data() + position, 512);
        std::this_thread::sleep_for(blockTime);
    }
    CHECK(waitFor(worker, before + 4));
    CHECK_NEAR(centsBetween(worker.reading().lastHitHz, 98.0), 0.0, 10.0);

    // A new sample rate clears the reading; the same samples now mean a different pitch.
    worker.setSampleRate(96000.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!worker.reading().valid && worker.reading().pitchedHits == 0u);
    const std::vector<float> at96k = kickRow({ 60.0, 60.0, 60.0, 60.0 }, 96000.0, random);
    play(worker, at96k, 96000.0);
    CHECK(waitFor(worker, 3));
    CHECK_NEAR(centsBetween(worker.reading().hz, 60.0), 0.0, 10.0);
    worker.enable(false);
}

TEST_CASE("AnalysisWorker: every reading is one whole reading, never a mix of two")
{
    const double fs = 48000.0;
    Random random(12);
    const std::vector<float> kicks = kickRow({ 55.0, 58.3, 61.7, 65.4, 55.0, 58.3 }, fs, random);

    AnalysisWorker worker;
    worker.setSampleRate(fs);
    worker.enable(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // A reader as impatient as it gets, while readings appear and are wiped by sample-rate changes.
    std::atomic<bool> stop { false };
    std::atomic<int> torn { 0 };
    std::thread reader([&] {
        while (!stop.load())
        {
            const TunerReading r = worker.reading();
            const bool whole = r.valid == (r.hz > 0.0) && r.valid == (r.pitchedHits > 0u)
                && r.valid == (r.lastHitHz > 0.0) && r.onsets >= r.pitchedHits;
            torn += whole ? 0 : 1;
        }
    });

    for (int round = 0; round < 3; ++round)
    {
        play(worker, kicks, fs);
        CHECK(waitFor(worker, 3));
        worker.setSampleRate(fs);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    stop = true;
    reader.join();
    CHECK(torn.load() == 0);
    worker.enable(false);
}
