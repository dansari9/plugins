#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace rootnote
{
/**
    Integer-factor decimator with a Blackman-windowed sinc low-pass.
    Analysis thread only. A factor of 1 passes the signal through.
*/
class Decimator
{
public:
    void prepare(int decimationFactor)
    {
        factor = decimationFactor < 1 ? 1 : decimationFactor;
        taps.clear();
        history.clear();
        writePosition = 0;
        phase = 0;

        if (factor == 1)
            return;

        // 12 zero crossings each side; cutoff at 0.45 of the output rate.
        const int halfLength = 12 * factor;
        const int length = 2 * halfLength + 1;
        const double cutoff = 0.45 / static_cast<double>(factor); // cycles per input sample
        const double pi = 3.14159265358979323846;

        taps.resize(static_cast<size_t>(length));
        double sum = 0.0;
        for (int i = 0; i < length; ++i)
        {
            const double n = static_cast<double>(i - halfLength);
            const double sinc = n == 0.0 ? 2.0 * cutoff : std::sin(2.0 * pi * cutoff * n) / (pi * n);
            const double x = static_cast<double>(i) / static_cast<double>(length - 1);
            const double window = 0.42 - 0.5 * std::cos(2.0 * pi * x) + 0.08 * std::cos(4.0 * pi * x);
            taps[static_cast<size_t>(i)] = sinc * window;
            sum += sinc * window;
        }
        for (auto& tap : taps)
            tap /= sum;

        history.assign(static_cast<size_t>(length), 0.0);
    }

    void reset()
    {
        for (auto& h : history)
            h = 0.0;
        writePosition = 0;
        phase = 0;
    }

    int getFactor() const noexcept { return factor; }

    /** Delay of the filter in input samples. */
    int delaySamples() const noexcept { return factor == 1 ? 0 : static_cast<int>(taps.size() / 2); }

    /** Appends the decimated samples to out. */
    void process(const float* in, size_t count, std::vector<float>& out)
    {
        if (factor == 1)
        {
            out.insert(out.end(), in, in + count);
            return;
        }

        const size_t length = history.size();
        for (size_t i = 0; i < count; ++i)
        {
            history[writePosition] = static_cast<double>(in[i]);
            writePosition = (writePosition + 1) % length;

            if (++phase < factor)
                continue;
            phase = 0;

            double acc = 0.0;
            size_t index = writePosition; // oldest sample
            for (size_t t = 0; t < length; ++t)
            {
                acc += history[index] * taps[t];
                if (++index == length)
                    index = 0;
            }
            out.push_back(static_cast<float>(acc));
        }
    }

private:
    int factor = 1;
    int phase = 0;
    size_t writePosition = 0;
    std::vector<double> taps;
    std::vector<double> history;
};
} // namespace rootnote
