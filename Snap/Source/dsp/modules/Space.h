#pragma once

#include "SpaceConstants.h"
#include "../common/Ducker.h"
#include "../common/LRCrossover.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace snap
{
class SpaceConvolver;

class Space
{
public:
    // Values use the host parameter units. The caller applies Magnitude to amount.
    struct Settings
    {
        bool enabled = true;
        int type = 0;
        float amount = 0, decay = 50, stretch = 0, predelayMs = 0, duck = 0, pan = 0, width = 100;
        float focusLoHz = spaceConstants::defaultFocusLoHz, focusHiHz = spaceConstants::defaultFocusHiHz;
    };

    Space();
    ~Space();

    // Prepare and destruction are lifecycle operations off the audio thread.
    void prepare(const juce::dsp::ProcessSpec&, const Settings& initial);
    void prepare(const juce::dsp::ProcessSpec& spec) { prepare(spec, {}); }
    void reset();
    void process(juce::AudioBuffer<float>&, const Settings&);
    float wetPeak() const { return wetMeter; } // audio thread: the largest wet sample the last process call added
    static constexpr int getLatencySamples() { return 0; }

private:
    float wetMeter = 0;
    using Request = std::uint64_t;
    struct Slot
    {
        std::unique_ptr<SpaceConvolver> engine;
        Request request = 0;
        std::atomic<bool> occupied { false };
    };

    static Request encode(const Settings&);
    void stopWorker();
    void buildResponses();
    void startTransition(Request);
    void finishTransition();
    void setContinuousTargets(const Settings&);
    void clearHistory();

    // The worker writes a free slot and publishes it with release. Processing
    // releases that slot only after its final read. The worker owns reclamation.
    std::array<Slot, 2> slots;
    std::atomic<Request> requested { 0 };
    std::atomic<bool> stopRequested { false };
    juce::WaitableEvent responseReady; // Only used by the worker and prepare().
    juce::dsp::ProcessSpec preparedSpec {};
    unsigned active = 0;
    int warmupLeft = 0, fadePosition = 0, warmupLength = 0, fadeLength = 0;
    bool transitioning = false, idle = true;

    LRCrossover highpass, lowpass;
    Ducker ducker;
    Smoother amount, duck, pan, width, delayBlend;
    juce::AudioBuffer<float> delayRing;
    int delayWrite = 0, delayFrom = 0, delayTo = 0, delayTarget = 0;
    std::thread worker;
};
} // namespace snap
