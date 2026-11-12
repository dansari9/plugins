#include "AnalysisWorker.h"

#include <chrono>
#include <vector>

namespace rootnote
{
AnalysisWorker::AnalysisWorker() = default;

AnalysisWorker::~AnalysisWorker()
{
    {
        const std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    if (thread.joinable())
        thread.join();
}

void AnalysisWorker::setSampleRate(double sampleRate)
{
    rate.store(sampleRate, std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_release);

    const std::lock_guard<std::mutex> lock(mutex);
    if (!thread.joinable() && !stopping)
        thread = std::thread([this] { run(); });
}

void AnalysisWorker::enable(bool shouldRun) noexcept
{
    if (shouldRun)
    {
        if (users.fetch_add(1, std::memory_order_relaxed) == 0)
            resync.store(true, std::memory_order_relaxed);
    }
    else
    {
        users.fetch_sub(1, std::memory_order_relaxed);
    }
}

void AnalysisWorker::push(const float* left, const float* right, int numSamples) noexcept
{
    if (numSamples <= 0 || users.load(std::memory_order_relaxed) <= 0)
        return;

    const bool written = right != nullptr
        ? fifo.write(static_cast<size_t>(numSamples), [=](size_t i) { return 0.5f * (left[i] + right[i]); })
        : fifo.write(static_cast<size_t>(numSamples), [=](size_t i) { return left[i]; });

    if (!written)
        overflowed.store(true, std::memory_order_relaxed);
}

void AnalysisWorker::publish(const TunerReading& r)
{
    const std::lock_guard<std::mutex> lock(readingMutex);
    published = r;
}

TunerReading AnalysisWorker::reading() const
{
    const std::lock_guard<std::mutex> lock(readingMutex);
    return published;
}

void AnalysisWorker::run()
{
    std::vector<float> block(4096);
    Tuner tuner;
    uint32_t seenGeneration = 0;
    uint32_t publishedOnsets = 0;
    bool prepared = false;

    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            const auto pause = std::chrono::milliseconds(isEnabled() ? 10 : 100);
            if (wake.wait_for(lock, pause, [this] { return stopping; }))
                return;
        }

        const uint32_t currentGeneration = generation.load(std::memory_order_acquire);
        if (currentGeneration != seenGeneration)
        {
            seenGeneration = currentGeneration;
            tuner.prepare(rate.load(std::memory_order_relaxed));
            prepared = true;
            publishedOnsets = 0;
            fifo.discard();
            overflowed.store(false, std::memory_order_relaxed);
            publish(tuner.reading());
            continue;
        }

        if (!prepared)
            continue;

        if (overflowed.exchange(false, std::memory_order_relaxed) || resync.exchange(false, std::memory_order_relaxed))
        {
            // Samples were lost or analysis was paused: what is buffered no
            // longer joins up with what the tuner has seen.
            fifo.discard();
            tuner.resetStream();
            continue;
        }

        size_t count = 0;
        while ((count = fifo.read(block.data(), block.size())) > 0)
        {
            const bool newReading = tuner.process(block.data(), count);
            if (newReading || tuner.reading().onsets != publishedOnsets)
            {
                publishedOnsets = tuner.reading().onsets;
                publish(tuner.reading());
            }
        }
    }
}
} // namespace rootnote
