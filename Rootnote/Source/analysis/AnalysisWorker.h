#pragma once

#include "SpscFifo.h"
#include "Tuner.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace rootnote
{
/**
    Runs the tuner on its own thread, fed by the audio thread through a
    lock-free FIFO. The audio thread only copies samples: push() never
    allocates, locks or signals. The worker polls every 10 ms.

    Analysis is advisory (it drives the tuner chip and the Match buttons) and
    never touches the audio path. It only runs while enabled, which the editor
    switches on while it is open.
*/
class AnalysisWorker
{
public:
    AnalysisWorker();
    ~AnalysisWorker();

    /** Not for the audio thread. Starts the worker thread on first use. */
    void setSampleRate(double sampleRate);

    /** Counted: each enable(true) needs a matching enable(false). Any thread. */
    void enable(bool shouldRun) noexcept;
    bool isEnabled() const noexcept { return users.load(std::memory_order_relaxed) > 0; }

    /** Audio thread. right may be null. */
    void push(const float* left, const float* right, int numSamples) noexcept;

    /** Any thread. */
    TunerReading reading() const noexcept;

private:
    void run();
    void publish(const TunerReading& reading) noexcept;

    SpscFifo fifo { static_cast<size_t>(1) << 16 };

    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;

    std::atomic<double> rate { 0.0 };
    std::atomic<uint32_t> generation { 0 };
    std::atomic<int> users { 0 };
    std::atomic<bool> overflowed { false };
    std::atomic<bool> resync { false };

    // Published reading, guarded by a sequence counter (odd while being written).
    std::atomic<uint32_t> sequence { 0 };
    std::atomic<bool> valid { false };
    std::atomic<double> hz { 0.0 };
    std::atomic<double> lastHitHz { 0.0 };
    std::atomic<double> glide { 0.0 };
    std::atomic<double> clarity { 0.0 };
    std::atomic<uint32_t> pitchedHits { 0 };
    std::atomic<uint32_t> onsets { 0 };
};
} // namespace rootnote
