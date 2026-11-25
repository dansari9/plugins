#pragma once

#include "Engine.h"
#include <atomic>

namespace vibrate
{
struct Frame
{
    uint32_t sequence = 0;
    unsigned enabled = 0;
    std::array<double, 2> pitchStep {}, phaseHz {};
    double panGain = 0, gapSeconds = 0;
    Controls energy {};
    double midEnergy = 0, leftEnergy = 0, rightEnergy = 0, cross = 0;
};

class Feed
{
public:
    static constexpr uint32_t capacity = 512;

    bool hasRoom() const
    {
        return write.load(std::memory_order_relaxed) - read.load(std::memory_order_acquire) < capacity;
    }

    bool push(const Frame& frame)
    {
        if (!hasRoom())
            return false;
        const auto index = write.load(std::memory_order_relaxed);
        ring[index % capacity] = frame;
        write.store(index + 1, std::memory_order_release);
        return true;
    }

    bool pop(Frame& frame)
    {
        const auto index = read.load(std::memory_order_relaxed);
        if (index == write.load(std::memory_order_acquire))
            return false;
        frame = ring[index % capacity];
        read.store(index + 1, std::memory_order_release);
        return true;
    }

    void discard() { read.store(write.load(std::memory_order_acquire), std::memory_order_release); }

    std::atomic<double> lastBlockMs { -1e9 };

private:
    static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<double>::is_always_lock_free);
    std::array<Frame, capacity> ring {};
    std::atomic<uint32_t> write { 0 }, read { 0 };
};

class Meter
{
public:
    void reset(double fs)
    {
        hop = std::max(1, static_cast<int>(std::round(fs / 200.0)));
        inverseHop = 1.0 / hop;
        inverseSampleRate = 1.0 / fs;
        ++sequence;
        used = 0;
        missing = false;
        sums = {};
    }

    void add(const Sample& sample, Feed& feed)
    {
        if (missing)
        {
            if (++used == hop)
            {
                ++sequence;
                used = 0;
                missing = false;
            }
            return;
        }
        if (used == 0)
            firstPitch = { sample.pitch1, sample.pitch2 };

        for (std::size_t i = 0; i < sample.terms.size(); ++i)
            sums.energy[i] += sample.terms[i] * sample.terms[i];
        sums.midEnergy += sample.mid * sample.mid;
        const double left = std::isfinite(sample.out.left) ? sample.out.left : 0;
        const double right = std::isfinite(sample.out.right) ? sample.out.right : 0;
        sums.leftEnergy += left * left;
        sums.rightEnergy += right * right;
        sums.cross += left * right;
        sums.panGain += sample.panGain;
        sums.gapSeconds += sample.delay2 - sample.delay1;
        sums.phaseHz[0] += sample.phase1;
        sums.phaseHz[1] += sample.phase2;
        if (++used < hop)
            return;

        Frame frame = sums;
        frame.sequence = sequence++;
        frame.enabled = sample.enabled;
        if (firstPitch[0] > 0 && sample.pitch1 > 0 && hop > 1)
            frame.pitchStep = { (sample.pitch1 - firstPitch[0]) / (hop - 1), (sample.pitch2 - firstPitch[1]) / (hop - 1) };
        for (auto& energy : frame.energy)
            energy *= inverseHop;
        for (auto& phaseHz : frame.phaseHz)
            phaseHz *= inverseHop;
        frame.panGain *= inverseHop;
        frame.gapSeconds *= inverseHop * inverseSampleRate;
        frame.midEnergy *= inverseHop;
        frame.leftEnergy *= inverseHop;
        frame.rightEnergy *= inverseHop;
        frame.cross *= inverseHop;
        feed.push(frame);
        used = 0;
        sums = {};
    }

    void skip(int samples)
    {
        if (samples == 0) return;
        const int elapsed = used + samples;
        sequence += static_cast<uint32_t>(elapsed / hop);
        used = elapsed % hop;
        missing = used != 0;
        sums = {};
    }

private:
    int hop = 240, used = 0;
    bool missing = false;
    uint32_t sequence = 0;
    double inverseHop = 1.0 / 240, inverseSampleRate = 1.0 / 48000;
    std::array<double, 2> firstPitch {};
    Frame sums;
};
} // namespace vibrate
