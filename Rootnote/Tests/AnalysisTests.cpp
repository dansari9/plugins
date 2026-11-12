#include "TestFramework.h"
#include "TestSignals.h"

#include "analysis/Decimator.h"
#include "analysis/Fft.h"
#include "analysis/OnsetDetector.h"
#include "analysis/PitchDetector.h"
#include "analysis/SpscFifo.h"
#include "dsp/NoteMath.h"

#include <atomic>
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

/** The part of a hit the tuner hands to the pitch detector: from 15 ms after the onset, up to 0.36 s. */
std::vector<float> hitSegment(const std::vector<float>& hit, double fs)
{
    const size_t skip = static_cast<size_t>(0.015 * fs);
    const size_t length = std::min(hit.size() - skip, static_cast<size_t>(0.360 * fs));
    return { hit.begin() + static_cast<std::ptrdiff_t>(skip), hit.begin() + static_cast<std::ptrdiff_t>(skip + length) };
}

struct OnsetScore
{
    int hits = 0;
    int detected = 0;
    int matched = 0;
    double worstErrorSeconds = 0.0;

    double f1() const
    {
        if (matched == 0)
            return 0.0;
        const double precision = static_cast<double>(matched) / detected;
        const double recall = static_cast<double>(matched) / hits;
        return 2.0 * precision * recall / (precision + recall);
    }
};

/** Matches detected onsets to the true ones, each used once, within 25 ms. */
OnsetScore score(const std::vector<double>& truth, const std::vector<double>& detected)
{
    OnsetScore s;
    s.hits = static_cast<int>(truth.size());
    s.detected = static_cast<int>(detected.size());
    std::vector<bool> used(detected.size(), false);
    for (double t : truth)
    {
        int best = -1;
        for (size_t i = 0; i < detected.size(); ++i)
            if (!used[i] && std::abs(detected[i] - t) <= 0.025
                && (best < 0 || std::abs(detected[i] - t) < std::abs(detected[static_cast<size_t>(best)] - t)))
                best = static_cast<int>(i);
        if (best >= 0)
        {
            used[static_cast<size_t>(best)] = true;
            ++s.matched;
            s.worstErrorSeconds = std::max(s.worstErrorSeconds, std::abs(detected[static_cast<size_t>(best)] - t));
        }
    }
    return s;
}

std::vector<double> detectOnsets(const std::vector<float>& signal, double fs)
{
    OnsetDetector detector;
    detector.prepare(fs);
    std::vector<int64_t> onsets;
    for (size_t position = 0; position < signal.size(); position += 700)
        detector.process(signal.data() + position, std::min<size_t>(700, signal.size() - position), onsets);

    std::vector<double> seconds;
    for (int64_t onset : onsets)
        seconds.push_back(static_cast<double>(onset) / fs);
    return seconds;
}
} // namespace

TEST_CASE("Fft: a sine lands in its bin, an impulse is flat")
{
    const Fft fft(10);
    const size_t n = fft.length();
    CHECK(n == 1024u);

    std::vector<double> re(n), im(n, 0.0);
    for (size_t i = 0; i < n; ++i)
        re[i] = std::cos(kTwoPi * 37.0 * static_cast<double>(i) / static_cast<double>(n));
    fft.forward(re.data(), im.data());
    for (size_t k = 0; k < n; ++k)
    {
        const double magnitude = std::hypot(re[k], im[k]);
        CHECK_NEAR(magnitude, (k == 37 || k == n - 37) ? 512.0 : 0.0, 1.0e-9);
    }

    std::fill(re.begin(), re.end(), 0.0);
    std::fill(im.begin(), im.end(), 0.0);
    re[5] = 1.0;
    fft.forward(re.data(), im.data());
    for (size_t k = 0; k < n; ++k)
        CHECK_NEAR(std::hypot(re[k], im[k]), 1.0, 1.0e-12);
}

TEST_CASE("SpscFifo: keeps order, wraps, and refuses a block that does not fit")
{
    SpscFifo fifo(16);
    CHECK(fifo.capacity() == 16u);
    float out[16] {};

    float next = 0.0f;
    const auto generate = [&next](size_t) { return next++; };

    float expected = 0.0f;
    for (int round = 0; round < 10; ++round) // 10 x 7 samples wraps the 16-sample ring several times
    {
        CHECK(fifo.write(7, generate));
        CHECK(fifo.readAvailable() == 7u);
        CHECK(fifo.read(out, 16) == 7u);
        for (int i = 0; i < 7; ++i)
            CHECK(out[i] == expected++);
    }

    CHECK(fifo.write(16, generate));
    CHECK(!fifo.write(1, generate)); // full: nothing is written
    CHECK(fifo.read(out, 4) == 4u);
    CHECK(!fifo.write(5, generate)); // only 4 free
    CHECK(fifo.write(4, generate));
    fifo.discard();
    CHECK(fifo.readAvailable() == 0u);
    CHECK(fifo.read(out, 16) == 0u);
}

TEST_CASE("SpscFifo: one writer thread and one reader thread lose and reorder nothing")
{
    SpscFifo fifo(1024);
    constexpr int kTotal = 2000000;
    std::atomic<bool> orderKept { true };

    std::thread reader([&] {
        std::vector<float> block(300);
        int expected = 0;
        while (expected < kTotal)
        {
            const size_t count = fifo.read(block.data(), block.size());
            for (size_t i = 0; i < count; ++i)
                if (block[i] != static_cast<float>(expected++ & 0xffff))
                    orderKept.store(false);
            if (count == 0)
                std::this_thread::yield();
        }
    });

    int written = 0;
    while (written < kTotal)
    {
        const int count = std::min(1 + written % 97, kTotal - written);
        if (fifo.write(static_cast<size_t>(count), [written](size_t i) { return static_cast<float>((written + static_cast<int>(i)) & 0xffff); }))
            written += count;
        else
            std::this_thread::yield();
    }
    reader.join();
    CHECK(orderKept.load());
    CHECK(fifo.readAvailable() == 0u);
}

TEST_CASE("Decimator: passes the band it keeps, rejects what would alias, reports its delay")
{
    for (int factor : { 2, 4, 6, 8 })
    {
        const double fs = 48000.0;
        const double outFs = fs / factor;
        const auto levelDb = [&](double hz) {
            Decimator decimator;
            decimator.prepare(factor);
            std::vector<float> in(48000), out;
            for (size_t i = 0; i < in.size(); ++i)
                in[i] = static_cast<float>(std::sin(kTwoPi * hz * static_cast<double>(i) / fs));
            decimator.process(in.data(), in.size(), out);
            CHECK(out.size() == in.size() / static_cast<size_t>(factor));
            double energy = 0.0;
            for (size_t i = out.size() / 2; i < out.size(); ++i)
                energy += static_cast<double>(out[i]) * static_cast<double>(out[i]);
            return gainToDb(std::sqrt(2.0 * energy / static_cast<double>(out.size() - out.size() / 2)));
        };

        const std::string name = "factor " + std::to_string(factor);
        CHECK_NEAR_MSG(levelDb(0.05 * outFs), 0.0, 0.05, name);
        CHECK_NEAR_MSG(levelDb(0.30 * outFs), 0.0, 0.1, name);
        CHECK_MSG(levelDb(0.60 * outFs) < -60.0, name); // would fold back to 0.40 of the output rate
        CHECK_MSG(levelDb(1.30 * outFs) < -70.0, name);

        // An impulse comes out delaySamples() input samples later.
        Decimator decimator;
        decimator.prepare(factor);
        std::vector<float> in(static_cast<size_t>(200 * factor), 0.0f), out;
        const int at = 50 * factor - 1; // the input sample that output sample 49 is taken at
        in[static_cast<size_t>(at)] = 1.0f;
        decimator.process(in.data(), in.size(), out);
        size_t loudest = 0;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::abs(out[i]) > std::abs(out[loudest]))
                loudest = i;
        CHECK_NEAR_MSG(static_cast<double>(loudest) * factor + factor - 1, at + decimator.delaySamples(), factor, name);
    }

    Decimator passThrough;
    passThrough.prepare(1);
    const std::vector<float> in { 1.0f, -2.0f, 3.0f };
    std::vector<float> out;
    passThrough.process(in.data(), in.size(), out);
    CHECK(out == in);
    CHECK(passThrough.delaySamples() == 0);
}

TEST_CASE("OnsetDetector: finds the hits of drum loops from 60 to 180 BPM")
{
    const double fs = 24000.0;
    int totalHits = 0, totalDetected = 0, totalMatched = 0;

    for (int bpm : { 60, 90, 120, 150, 180 })
    {
        // Eighth notes: kick, tom, snare, kick ... with levels that vary by 12 dB.
        const double step = 60.0 / bpm / 2.0;
        std::vector<float> loop(static_cast<size_t>(fs * 12.0), 0.0f);
        std::vector<double> truth;
        Random random(static_cast<uint32_t>(bpm));
        int index = 0;
        for (double t = 0.25; t < 11.0; t += step, ++index)
        {
            const double level = 0.2 + 0.6 * random.uniform();
            const size_t start = static_cast<size_t>(std::llround(t * fs));
            if (index % 4 == 2)
            {
                MembraneSpec snare;
                snare.hz = 190.0;
                snare.noise = 0.6;
                snare.decayTau = 0.08;
                snare.level = level;
                addMembrane(loop, start, snare, fs, random);
            }
            else if (index % 4 == 1)
            {
                MembraneSpec tom;
                tom.hz = 110.0;
                tom.decayTau = 0.10;
                tom.level = level;
                addMembrane(loop, start, tom, fs, random);
            }
            else
            {
                KickSpec kick;
                kick.hz = 48.0;
                kick.decayTau = 0.08;
                kick.level = level;
                addKick(loop, start, kick, fs, random);
            }
            truth.push_back(static_cast<double>(start) / fs);
        }

        const OnsetScore result = score(truth, detectOnsets(loop, fs));
        const std::string name = std::to_string(bpm) + " BPM: " + std::to_string(result.matched) + " of "
                                     + std::to_string(result.hits) + ", " + std::to_string(result.detected) + " detected";
        CHECK_MSG(result.f1() > 0.95, name);
        CHECK_MSG(result.worstErrorSeconds <= 0.012, name + ", worst error " + std::to_string(result.worstErrorSeconds));
        totalHits += result.hits;
        totalDetected += result.detected;
        totalMatched += result.matched;
    }

    std::printf("    %d hits, %d detected, %d matched\n", totalHits, totalDetected, totalMatched);
    CHECK(totalHits > 150);
    CHECK(totalMatched >= totalHits - totalHits / 40);   // recall of at least 97.5 % over all loops
    CHECK(totalDetected <= totalHits + totalHits / 40);
}

TEST_CASE("OnsetDetector: soft low kicks without a click are found by the level detector")
{
    const double fs = 24000.0;
    std::vector<float> signal(static_cast<size_t>(fs * 6.0), 0.0f);
    std::vector<double> truth;
    Random random(3);
    for (double t = 0.5; t < 5.5; t += 0.5)
    {
        KickSpec kick;
        kick.hz = 35.0;
        kick.dropSemitones = 2.0;
        kick.click = 0.0;
        kick.attack = 0.004;
        kick.decayTau = 0.07;
        kick.level = 0.5;
        addKick(signal, static_cast<size_t>(t * fs), kick, fs, random);
        truth.push_back(t);
    }
    const OnsetScore result = score(truth, detectOnsets(signal, fs));
    CHECK(result.matched == result.hits);
    CHECK(result.detected == result.hits);
}

TEST_CASE("OnsetDetector: silence, steady tones and steady noise are not hits; hits are 60 ms apart")
{
    const double fs = 24000.0;
    const size_t length = static_cast<size_t>(fs * 5.0);

    CHECK(detectOnsets(std::vector<float>(length, 0.0f), fs).empty());

    // The start of a steady sound is one hit; nothing after it.
    std::vector<float> tone(length);
    for (size_t i = 0; i < length; ++i)
        tone[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 220.0 * static_cast<double>(i) / fs));
    CHECK(detectOnsets(tone, fs).size() <= 1u);

    Random random(8);
    std::vector<float> hiss(length);
    for (auto& x : hiss)
        x = static_cast<float>(0.3 * random.bipolar());
    CHECK(detectOnsets(hiss, fs).size() <= 1u);

    // A 30 ms flam is one hit.
    std::vector<float> flam(length, 0.0f);
    for (double t : { 1.0, 1.03, 3.0, 3.03 })
        addKick(flam, static_cast<size_t>(t * fs), KickSpec {}, fs, random);
    const auto found = detectOnsets(flam, fs);
    CHECK(found.size() == 2u);
    for (size_t i = 1; i < found.size(); ++i)
        CHECK(found[i] - found[i - 1] >= OnsetDetector::kMinGapSeconds);
}

TEST_CASE("PitchDetector: settled pitch of synthetic kicks within 10 cents, 30 to 200 Hz, drops of 2 to 12 semitones")
{
    const double fs = 4000.0;
    PitchDetector detector;
    Random random(1);
    double worst = 0.0;

    for (double hz : { 30.0, 36.7, 41.2, 46.25, 55.0, 65.4, 82.4, 110.0, 150.0, 200.0 })
        for (double drop : { 2.0, 6.0, 12.0 })
            for (double decayTau : { 0.08, 0.20 })
            {
                KickSpec kick;
                kick.hz = hz;
                kick.dropSemitones = drop;
                kick.decayTau = decayTau;
                std::vector<float> hit(static_cast<size_t>(fs), 0.0f);
                addKick(hit, 0, kick, fs, random);
                const std::vector<float> segment = hitSegment(hit, fs);

                const PitchResult result = detector.analyse(segment.data(), static_cast<int>(segment.size()), fs);
                const std::string name = std::to_string(hz) + " Hz, drop " + std::to_string(drop) + " st, decay "
                                             + std::to_string(decayTau);
                CHECK_MSG(result.valid, name);
                CHECK_NEAR_MSG(centsBetween(result.hz, hz), 0.0, 10.0, name);
                CHECK_MSG(result.clarity > 0.5, name);
                worst = std::max(worst, std::abs(centsBetween(result.hz, hz)));
            }
    std::printf("    worst kick error: %.2f cents\n", worst);
}

TEST_CASE("PitchDetector: reports how far a kick glides")
{
    const double fs = 4000.0;
    PitchDetector detector;
    Random random(2);
    const auto glideOf = [&](double drop) {
        KickSpec kick;
        kick.hz = 50.0;
        kick.dropSemitones = drop;
        kick.dropTau = 0.04;
        std::vector<float> hit(static_cast<size_t>(fs), 0.0f);
        addKick(hit, 0, kick, fs, random);
        const std::vector<float> segment = hitSegment(hit, fs);
        return detector.analyse(segment.data(), static_cast<int>(segment.size()), fs).glideSemitones;
    };

    CHECK_NEAR(glideOf(0.0), 0.0, 0.15);
    // The analysis starts 15 ms into the hit and averages over its first frame, so it sees part of the fall.
    const double six = glideOf(6.0);
    const double twelve = glideOf(12.0);
    CHECK(six > 1.0 && six < 6.0);
    CHECK(twelve > six + 1.0 && twelve < 12.0);
}

TEST_CASE("PitchDetector: toms and snares from membrane modes within 15 cents")
{
    const double fs = 4000.0;
    PitchDetector detector;
    Random random(4);
    double worst = 0.0;

    for (double hz : { 60.0, 82.4, 110.0, 146.8, 196.0, 220.0 })
        for (double noise : { 0.0, 0.5 })
        {
            MembraneSpec drum;
            drum.hz = hz;
            drum.noise = noise;
            std::vector<float> hit(static_cast<size_t>(fs), 0.0f);
            addMembrane(hit, 0, drum, fs, random);
            const std::vector<float> segment = hitSegment(hit, fs);

            const PitchResult result = detector.analyse(segment.data(), static_cast<int>(segment.size()), fs);
            const std::string name = std::to_string(hz) + " Hz, noise " + std::to_string(noise);
            CHECK_MSG(result.valid, name);
            CHECK_NEAR_MSG(centsBetween(result.hz, hz), 0.0, 15.0, name);
            worst = std::max(worst, std::abs(centsBetween(result.hz, hz)));
        }
    std::printf("    worst membrane error: %.2f cents\n", worst);
}

TEST_CASE("PitchDetector: silence, noise and too-short segments give no pitch")
{
    const double fs = 4000.0;
    PitchDetector detector;

    const std::vector<float> silence(1440, 0.0f);
    CHECK(!detector.analyse(silence.data(), static_cast<int>(silence.size()), fs).valid);

    Random random(6);
    std::vector<float> hiss(1440);
    for (auto& x : hiss)
        x = static_cast<float>(0.5 * random.bipolar());
    CHECK(!detector.analyse(hiss.data(), static_cast<int>(hiss.size()), fs).valid);

    std::vector<float> blip(40);
    for (size_t i = 0; i < blip.size(); ++i)
        blip[i] = static_cast<float>(std::sin(kTwoPi * 100.0 * static_cast<double>(i) / fs));
    CHECK(!detector.analyse(blip.data(), static_cast<int>(blip.size()), fs).valid);
    CHECK(!detector.analyse(blip.data(), 0, fs).valid);
}
