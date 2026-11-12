#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace rootnote
{
/**
    Lock-free single-producer / single-consumer ring of floats.

    The audio thread writes, the analysis thread reads. Neither side blocks or
    allocates; a write that does not fit is refused as a whole so the reader
    never sees a torn block.
*/
class SpscFifo
{
public:
    /** capacity must be a power of two. Allocates; call off the audio thread. */
    explicit SpscFifo(size_t capacityPowerOfTwo)
        : buffer(capacityPowerOfTwo, 0.0f), mask(capacityPowerOfTwo - 1)
    {
    }

    size_t capacity() const noexcept { return buffer.size(); }

    size_t readAvailable() const noexcept
    {
        return writeIndex.load(std::memory_order_acquire) - readIndex.load(std::memory_order_relaxed);
    }

    /**
        Producer side. generate(i) returns sample i of the block. Returns false,
        writing nothing, when there is not enough room.
    */
    template <typename Generator>
    bool write(size_t count, Generator&& generate) noexcept
    {
        const size_t w = writeIndex.load(std::memory_order_relaxed);
        const size_t r = readIndex.load(std::memory_order_acquire);
        if (count > buffer.size() - (w - r))
            return false;

        for (size_t i = 0; i < count; ++i)
            buffer[(w + i) & mask] = generate(i);

        writeIndex.store(w + count, std::memory_order_release);
        return true;
    }

    /** Consumer side. Returns the number of samples copied to dest. */
    size_t read(float* dest, size_t maxCount) noexcept
    {
        const size_t r = readIndex.load(std::memory_order_relaxed);
        const size_t w = writeIndex.load(std::memory_order_acquire);
        const size_t count = (w - r) < maxCount ? (w - r) : maxCount;

        for (size_t i = 0; i < count; ++i)
            dest[i] = buffer[(r + i) & mask];

        readIndex.store(r + count, std::memory_order_release);
        return count;
    }

    /** Consumer side: drops everything that is waiting. */
    void discard() noexcept
    {
        readIndex.store(writeIndex.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::vector<float> buffer;
    const size_t mask;
    std::atomic<size_t> writeIndex { 0 };
    std::atomic<size_t> readIndex { 0 };
};
} // namespace rootnote
