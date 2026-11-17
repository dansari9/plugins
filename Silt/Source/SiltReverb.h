#pragma once
// SILT_REVERB: removable - delete this file, the verbSize / verbMix handling marked SILT_REVERB in Silt.cpp and the
// reverb.process call there. The two parameters stay in the frozen list (inert once removed).
//
// A small stereo feedback delay network: two allpass diffusers per side, four lines mixed by a 4 x 4 Hadamard
// matrix, a one-pole damping filter in each line. size 0..1 sets the line lengths (glided, so automation never clicks)
// and the decay (0.4 .. 6 s); the output is scaled so the reverb sits near the level of what goes in.
#include <array>
#include <cmath>
#include <vector>

class SiltReverb
{
public:
    void prepare(double sampleRate)
    {
        fs = sampleRate;
        for (auto& l : lines) l.assign((size_t) (base[3] * 1.3 * fs / 48000) + 4, 0.0f);
        for (size_t k = 0; k < 4; ++k) diffusers[k].assign((size_t) (diffuse[k] * fs / 48000) + 1, 0.0f);
        damp = (float) (1 - std::exp(-2 * 3.14159265358979 * 6500 / fs));
        reset();
    }
    void reset()
    {
        for (auto& l : lines) std::fill(l.begin(), l.end(), 0.0f);
        for (auto& d : diffusers) std::fill(d.begin(), d.end(), 0.0f);
        state = {}, dAt = {}, at = 0, glide = -1, tuned = -1;
    }
    // In place: l and r become the reverb of what came in.
    void process(float& l, float& r, float size)
    {
        glide = glide < 0 ? size : glide + 0.0002f * (size - glide);
        if (std::abs(glide - tuned) > 1e-4f) // the decay per line and the level, for the glided size
        {
            tuned = glide;
            const double scale = 0.3 + glide, rt60 = 0.4 + 5.6 * glide * glide;
            for (size_t k = 0; k < 4; ++k)
                len[k] = (float) (base[k] * scale * fs / 48000), gain[k] = (float) std::pow(10.0, -3 * len[k] / fs / rt60);
            norm = (float) (0.75 * std::sqrt(1 - gain[1] * gain[1]));
        }
        const float in[2] { allpass(1, allpass(0, l)), allpass(3, allpass(2, r)) };
        const size_t n = lines[0].size();
        float out[4];
        for (size_t k = 0; k < 4; ++k)
        {
            float p = (float) at - len[k];
            p += p < 0 ? (float) n : 0.0f;
            const auto i = (size_t) p;
            const float f = p - (float) i, a = lines[k][i], b = lines[k][(i + 1) % n];
            state[k] += damp * (a + f * (b - a) - state[k]);
            out[k] = gain[k] * state[k];
        }
        const float h[4] { out[0] + out[1] + out[2] + out[3], out[0] - out[1] + out[2] - out[3],
                           out[0] + out[1] - out[2] - out[3], out[0] - out[1] - out[2] + out[3] };
        for (size_t k = 0; k < 4; ++k)
        {
            const float v = 0.5f * h[k] + in[k % 2];
            lines[k][at] = std::abs(v) < 1e-15f ? 0.0f : v;
        }
        at = (at + 1) % n;
        l = norm * (out[0] + out[2]), r = norm * (out[1] + out[3]);
    }

private:
    float allpass(size_t k, float x)
    {
        auto& d = diffusers[k];
        const float delayed = d[dAt[k]], v = x + 0.6f * delayed;
        d[dAt[k]] = std::abs(v) < 1e-15f ? 0.0f : v;
        dAt[k] = (dAt[k] + 1) % d.size();
        return delayed - 0.6f * v;
    }
    static constexpr double base[4] { 1931, 2459, 2903, 3449 }, diffuse[4] { 211, 337, 283, 409 };
    double fs = 48000;
    std::array<std::vector<float>, 4> lines, diffusers;
    std::array<float, 4> state {}, len {}, gain {};
    std::array<size_t, 4> dAt {};
    size_t at = 0;
    float damp = 0.5f, glide = -1, tuned = -1, norm = 0;
};
