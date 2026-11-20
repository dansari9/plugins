#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

namespace snap
{
// Integer per-channel delay for latency alignment. A delay of 0 returns the input exactly.
class Delay
{
public:
    void prepare(int channels, int maxDelay)
    {
        ring.setSize(channels, maxDelay + 1);
        ring.clear();
        pos.assign((size_t) channels, 0);
        delay = std::min(delay, maxDelay);
    }
    void setDelay(int samples) { delay = juce::jlimit(0, ring.getNumSamples() - 1, samples); }
    int getDelay() const { return delay; }
    int getNumChannels() const { return ring.getNumChannels(); }
    void reset() { ring.clear(); }

    float next(int channel, float x)
    {
        const int size = ring.getNumSamples();
        int& p = pos[(size_t) channel];
        ring.setSample(channel, p, x);
        const float y = ring.getSample(channel, (p + size - delay) % size);
        p = (p + 1) % size;
        return y;
    }

private:
    juce::AudioBuffer<float> ring;
    std::vector<int> pos;
    int delay = 0;
};
} // namespace snap
