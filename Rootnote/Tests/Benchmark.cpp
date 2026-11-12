// CPU cost of the engine in the plan's worst case: 32 boost + 16 cut + 16 ring
// bands, stereo, 96 kHz, 64-sample blocks. Prints the share of one core that
// real-time playback needs.
//
//   RootnoteBenchmark                  print the numbers
//   RootnoteBenchmark <max percent>    also fail (exit 1) when the worst case is above the limit

#include "TestSignals.h"

#include "dsp/Engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(__SSE__)
 #include <xmmintrin.h>
#endif

using namespace rootnote;

namespace
{
constexpr double kSampleRate = 96000.0;
constexpr int kBlockSize = 64;
constexpr double kSeconds = 20.0;

/** What the plug-in does at the top of processBlock (juce::ScopedNoDenormals). */
void disableDenormals()
{
#if defined(__SSE__)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#elif defined(__aarch64__)
    unsigned long long fpcr;
    __asm__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= 1ull << 24;
    __asm__("msr fpcr, %0" : : "r"(fpcr));
#endif
}

EngineParams worstCase()
{
    EngineParams p;
    p.boostGainDb = 12.0;
    p.boostQ = 40.0;
    p.bands = 32;
    p.rangeOct = 6.0;
    p.tiltDbPerOct = 0.0;
    p.cutOn = true;
    p.cutNote = 31.0;
    p.cutBands = 16;
    p.ring = 0.5;
    p.ringDecaySeconds = 2.0;
    return p;
}

enum class Input { noise, silenceAfterNoise };

/** Percent of one core: processing time over the duration of the audio processed. */
double measure(Input input, bool glide)
{
    Engine engine;
    engine.prepare(kSampleRate, kBlockSize);
    EngineParams params = worstCase();
    params.midiMode = MidiMode::channels12;
    params.glideSeconds = glide ? 0.2 : 0.0;
    engine.setParams(params);

    testsignals::Random random(1);
    std::vector<float> left(kBlockSize), right(kBlockSize);
    float* channels[2] = { left.data(), right.data() };
    const auto fill = [&](double amplitude) {
        for (int i = 0; i < kBlockSize; ++i)
        {
            left[static_cast<size_t>(i)] = static_cast<float>(amplitude * random.bipolar());
            right[static_cast<size_t>(i)] = static_cast<float>(amplitude * random.bipolar());
        }
    };

    // One second to fill every filter, outside the timed part.
    const int warmUp = static_cast<int>(kSampleRate) / kBlockSize;
    for (int block = 0; block < warmUp; ++block)
    {
        fill(0.25);
        engine.process(channels, 2, kBlockSize);
    }

    const int blocks = static_cast<int>(kSeconds * kSampleRate) / kBlockSize;
    const int blocksPerNote = static_cast<int>(0.1 * kSampleRate) / kBlockSize;
    std::chrono::steady_clock::duration spent {};
    for (int block = 0; block < blocks; ++block)
    {
        fill(input == Input::noise ? 0.25 : 0.0);
        const auto start = std::chrono::steady_clock::now();
        engine.setParams(params);
        if (glide && block % blocksPerNote == 0)
            engine.handleMidi({ MidiEvent::Type::noteOn, 1, 36 + (block / blocksPerNote) % 24 });
        engine.process(channels, 2, kBlockSize);
        spent += std::chrono::steady_clock::now() - start;
    }

    return 100.0 * std::chrono::duration<double>(spent).count() / kSeconds;
}
} // namespace

int main(int argc, char** argv)
{
    disableDenormals();

    const double steady = measure(Input::noise, false);
    const double gliding = measure(Input::noise, true);
    const double decaying = measure(Input::silenceAfterNoise, false);

    std::printf("Rootnote engine, 32 boost + 16 cut + 16 ring bands, stereo, 96 kHz, 64-sample blocks\n");
    std::printf("  steady settings:           %6.2f %% of one core\n", steady);
    std::printf("  note glide every 100 ms:   %6.2f %% of one core\n", gliding);
    std::printf("  silence after noise:       %6.2f %% of one core\n", decaying);

    const double worst = std::max(steady, std::max(gliding, decaying));
    if (argc > 1)
    {
        const double limit = std::atof(argv[1]);
        if (worst > limit)
        {
            std::printf("FAIL: %.2f %% is above the limit of %.2f %%\n", worst, limit);
            return 1;
        }
    }
    return 0;
}
