#pragma once

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace rootnote
{
/**
    Plain radix-2 FFT for the analysis thread. Sizes are powers of two; the
    tables are built once in the constructor. Not used on the audio thread.
*/
class Fft
{
public:
    explicit Fft(int order)
        : size(static_cast<size_t>(1) << order)
    {
        cosTable.resize(size / 2);
        sinTable.resize(size / 2);
        for (size_t i = 0; i < size / 2; ++i)
        {
            const double angle = -2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(size);
            cosTable[i] = std::cos(angle);
            sinTable[i] = std::sin(angle);
        }

        reversed.resize(size);
        for (size_t i = 0; i < size; ++i)
        {
            size_t r = 0;
            for (int bit = 0; bit < order; ++bit)
                if ((i >> bit) & 1u)
                    r |= static_cast<size_t>(1) << (order - 1 - bit);
            reversed[i] = r;
        }
    }

    size_t length() const noexcept { return size; }

    /** In-place forward transform of re + j im. Both arrays hold length() values. */
    void forward(double* re, double* im) const noexcept
    {
        for (size_t i = 0; i < size; ++i)
        {
            const size_t j = reversed[i];
            if (j > i)
            {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }

        for (size_t half = 1; half < size; half *= 2)
        {
            const size_t step = size / (2 * half);
            for (size_t start = 0; start < size; start += 2 * half)
            {
                for (size_t k = 0; k < half; ++k)
                {
                    const double wr = cosTable[k * step];
                    const double wi = sinTable[k * step];
                    const size_t a = start + k;
                    const size_t b = a + half;
                    const double tr = re[b] * wr - im[b] * wi;
                    const double ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] += tr;
                    im[a] += ti;
                }
            }
        }
    }

private:
    size_t size;
    std::vector<double> cosTable;
    std::vector<double> sinTable;
    std::vector<size_t> reversed;
};
} // namespace rootnote
