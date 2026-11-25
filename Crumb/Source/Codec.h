#pragma once
// crumb's lossy stage: a frame codec (MDCT) that throws away what it ranks lowest, coarsens the rest and loses whole
// frames. Plain C++, doubles only, no JUCE: the site's port (public/dsp/crumb-core.js) is this file line by line.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace crumb
{
// Hop sizes at 44.1 / 48 kHz, slow to fast; times 2 at 88.2 / 96 kHz and 4 above. A frame is two hops.
inline constexpr int numSpeeds = 9;
inline constexpr int hops[numSpeeds] = { 1024, 768, 512, 384, 256, 192, 128, 96, 64 };
inline int rateScale(double fs) { return fs >= 132300 ? 4 : fs >= 66150 ? 2 : 1; }
inline int speedIndex(double speed) { return std::clamp((int) std::floor(speed * (numSpeeds - 1) + 0.5), 0, numSpeeds - 1); } // speed 0..1

struct Lcg
{
    uint32_t seed = 1;
    double next() { return (double) ((seed = seed * 1664525u + 1013904223u) >> 8) * 0x1p-24; } // [0, 1)
};

// The orthonormal MDCT of hop n (a DCT-IV through an n/2-point complex FFT, radix 2 and 3).
struct Mdct
{
    int n = 0;
    std::vector<double> window, twR, twI, fftR, fftI; // the sine window, the DCT-IV twiddles, the FFT's
    std::vector<int> radices;

    void prepare(int hop)
    {
        n = hop;
        const double pi = 3.14159265358979323846;
        const size_t h = (size_t) n / 2;
        window.resize((size_t) n * 2), twR.resize(h), twI.resize(h), fftR.resize(h), fftI.resize(h);
        for (int i = 0; i < 2 * n; ++i)
            window[(size_t) i] = std::sin(pi / (2 * n) * (i + 0.5));
        for (int i = 0; i < n / 2; ++i)
        {
            const double a = -pi * (8 * i + 1) / (8 * n), b = -2 * pi * i / (n / 2);
            twR[(size_t) i] = std::cos(a), twI[(size_t) i] = std::sin(a), fftR[(size_t) i] = std::cos(b), fftI[(size_t) i] = std::sin(b);
        }
        radices.clear();
        for (int m = n / 2; m > 1; m /= radices.back())
            radices.push_back(m % 3 == 0 ? 3 : 2);
    }
    // x: n values in and out (in place); r, i, r2, i2: scratch of n / 2.
    void dct4(double* x, double* r, double* i, double* r2, double* i2) const
    {
        const int h = n / 2;
        for (int k = 0; k < h; ++k)
        {
            const double a = x[2 * k], b = x[n - 1 - 2 * k], c = twR[(size_t) k], s = twI[(size_t) k];
            r[k] = a * c - b * s, i[k] = a * s + b * c;
        }
        int len = h, stride = 1; // Stockham, self-sorting
        for (int radix : radices)
        {
            const int m = len / radix;
            for (int p = 0; p < m; ++p)
            {
                const double w1r = fftR[(size_t) (p * stride)], w1i = fftI[(size_t) (p * stride)];
                for (int q = 0; q < stride; ++q)
                {
                    const int from = q + stride * p, to = q + stride * radix * p, step = stride * m;
                    const double ar = r[from], ai = i[from], br = r[from + step], bi = i[from + step];
                    if (radix == 2)
                    {
                        const double dr = ar - br, di = ai - bi;
                        r2[to] = ar + br, i2[to] = ai + bi;
                        r2[to + stride] = dr * w1r - di * w1i, i2[to + stride] = dr * w1i + di * w1r;
                    }
                    else
                    {
                        const double cr = r[from + 2 * step], ci = i[from + 2 * step], half = 0.5, s60 = 0.86602540378443864676;
                        const double sr = br + cr, si = bi + ci, dr = (br - cr) * s60, di = (bi - ci) * s60;
                        const double mr = ar - half * sr, mi = ai - half * si;
                        const double ur = mr + di, ui = mi - dr, vr = mr - di, vi = mi + dr; // b w + c w^2, b w^2 + c w
                        const double w2r = w1r * w1r - w1i * w1i, w2i = 2 * w1r * w1i;
                        r2[to] = ar + sr, i2[to] = ai + si;
                        r2[to + stride] = ur * w1r - ui * w1i, i2[to + stride] = ur * w1i + ui * w1r;
                        r2[to + 2 * stride] = vr * w2r - vi * w2i, i2[to + 2 * stride] = vr * w2i + vi * w2r;
                    }
                }
            }
            std::swap(r, r2), std::swap(i, i2);
            len = m, stride *= radix;
        }
        const double scale = std::sqrt(2.0 / n);
        for (int k = 0; k < h; ++k)
        {
            const double c = twR[(size_t) k], s = twI[(size_t) k];
            x[2 * k] = scale * (r[k] * c - i[k] * s), x[n - 1 - 2 * k] = -scale * (r[k] * s + i[k] * c);
        }
    }
};

struct CodecSettings
{
    double loss = 0, speed = 1, drop = 0, jitter = 0, autoGain = 1; // 0..1
    bool hold = false, kept = false, flat = false;
    int stereo = 0; // stereo, joint, mono
};

// A frame coder runs in one of two slots (a speed change crossfades to the other); both read the same input history.
struct Codec
{
    static constexpr int maxBands = 26;
    static constexpr double bark[] { 100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000, 2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500 };
    enum Mark : uint8_t { clean, gap, held };

    struct Size // everything that depends on the hop only
    {
        Mdct mdct;
        int bands = 0, edge[maxBands + 1] {};
        double ath[maxBands] {}; // the threshold of hearing as a coefficient's energy
    };
    struct Coder
    {
        int size = 0;
        std::vector<double> sum[2]; // overlap-add, indexed by output time
        std::vector<double> last[2]; // the last good frame's coefficients
        std::vector<uint8_t> mark;
        bool bad[2] {}, hasLast = false;
        Lcg random, grain; // frame losses; the frame's own noise, seeded by its place in time so no frame's decisions shift another's
        double eIn = 0, eOut = 0, gain = 1; // what the codec removed, for auto
    };

    double fs = 48000;
    int latency = 2048, size = 8192, mask = 8191;
    std::array<Size, numSpeeds> sizes;
    std::vector<double> input[2], work[2], decoded[2], rank, sorted, scratch[4];
    Coder coders[2];
    int active = 0, fadeAt = -1, fadeLength = 960, warm = 0; // fadeAt: samples since the other slot started (-1: one slot runs)
    uint32_t now = 0;
    double makeup = 1, makeupCoef = 0, wobble = 0, wobbleCoef = 0, delayed[2][64] {};
    int wobbleAt = 0;

    void prepare(double sampleRate) // allocates
    {
        fs = sampleRate;
        const int scale = rateScale(fs);
        latency = 2048 * scale, size = 4 * latency, mask = size - 1;
        fadeLength = std::max(1, (int) (0.02 * fs)), makeupCoef = 1 - std::exp(-1 / (0.02 * fs)), wobbleCoef = 1 - std::exp(-2 * 3.14159265358979323846 * 6 / fs);
        for (int s = 0; s < numSpeeds; ++s)
        {
            auto& z = sizes[(size_t) s];
            const int n = hops[s] * scale;
            z.mdct.prepare(n), z.bands = 0, z.edge[0] = 0;
            for (double hz : bark)
                if (const int e = std::min(n, (int) std::floor(hz / (fs / 2) * n + 0.5)); e > z.edge[z.bands] && e < n)
                    z.edge[++z.bands] = e;
            z.edge[++z.bands] = n;
            for (int b = 0; b < z.bands; ++b)
            {
                const double f = std::max(0.02, 0.5 * (z.edge[b] + z.edge[b + 1]) * fs / (2.0 * n) / 1000); // kHz
                const double db = 3.64 * std::pow(f, -0.8) - 6.5 * std::exp(-0.6 * (f - 3.3) * (f - 3.3)) + 0.001 * f * f * f * f;
                z.ath[b] = std::pow(10.0, (std::min(db, 110.0) - 100) / 10) * n / 4; // full scale = 100 dB
            }
        }
        const auto most = (size_t) (1024 * scale);
        for (int c = 0; c < 2; ++c)
            input[c].resize((size_t) size), work[c].resize(2 * most), decoded[c].resize(most);
        rank.resize(2 * most), sorted.resize(2 * most);
        for (auto& s : scratch) s.resize(most);
        for (auto& k : coders)
        {
            for (int c = 0; c < 2; ++c)
                k.sum[c].resize((size_t) size), k.last[c].resize(most);
            k.mark.resize((size_t) size);
        }
        reset();
    }
    void reset()
    {
        for (auto& i : input) std::fill(i.begin(), i.end(), 0.0);
        active = 0, fadeAt = -1, warm = 0, now = 0, makeup = 1, wobble = 0, wobbleAt = 0;
        for (auto& d : delayed) std::fill(std::begin(d), std::end(d), 0.0);
        for (auto& k : coders) start(k, 0);
    }
    void start(Coder& k, int sizeIndex)
    {
        k.size = sizeIndex, k.hasLast = false, k.bad[0] = k.bad[1] = false, k.random.seed = 0x51f7c0deu, k.eIn = k.eOut = 0, k.gain = 1;
        for (int c = 0; c < 2; ++c)
            std::fill(k.sum[c].begin(), k.sum[c].end(), 0.0), std::fill(k.last[c].begin(), k.last[c].end(), 0.0);
        std::fill(k.mark.begin(), k.mark.end(), (uint8_t) clean);
    }
    // The stage idles (loss and drop 0): only the history is kept, so it can start at any time.
    void idle(const double* in, int channels)
    {
        for (int c = 0; c < channels; ++c) input[c][now & (uint32_t) mask] = in[c];
        ++now, warm = 0;
    }
    // One sample per channel: out and residual (what the codec took from its input) are `latency` samples late. Returns
    // the mark of the frame that sample belongs to.
    Mark process(const double* in, double* out, double* residual, int channels, const CodecSettings& s)
    {
        const int want = speedIndex(s.speed);
        if (warm == 0)
            start(coders[active], want), fadeAt = -1, makeup = 1;
        warm = std::min(warm + 1, 1 << 30);
        if (fadeAt < 0 && coders[active].size != want)
            start(coders[1 - active], want), fadeAt = 0;
        const auto at = now & (uint32_t) mask;
        double fade = 0;
        Mark m;
        if (fadeAt >= 0) // the new slot fills for one latency, then takes over
        {
            fade = std::clamp((double) (fadeAt - latency) / fadeLength, 0.0, 1.0);
            ++fadeAt;
        }
        auto& a = coders[active];
        auto& b = coders[1 - active];
        m = (Mark) (fade > 0.5 ? b : a).mark[at];
        const double target = std::exp(s.autoGain * std::log(a.gain + fade * (b.gain - a.gain)));
        makeup += makeupCoef * (target - makeup);
        wobble += wobbleCoef * (2 * a.random.next() - 1 - wobble);
        const double wow = s.jitter * 0.0004 * fs * std::clamp(0.5 + 4 * wobble, 0.0, 1.0);
        const int whole = std::min(62, (int) wow);
        const double part = std::min(1.0, wow - whole);
        for (int c = 0; c < channels; ++c)
        {
            double y = a.sum[c][at];
            a.sum[c][at] = 0;
            if (fadeAt >= 0)
                y += fade * (b.sum[c][at] - y), b.sum[c][at] = 0;
            delayed[c][wobbleAt] = y;
            if (wow > 0)
            {
                const double y0 = delayed[c][(wobbleAt - whole + 64) & 63], y1 = delayed[c][(wobbleAt - whole + 63) & 63];
                y = y0 + part * (y1 - y0);
            }
            const double aligned = input[c][(now - (uint32_t) latency) & (uint32_t) mask];
            residual[c] = aligned - y;
            out[c] = (s.kept ? residual[c] : y) * makeup;
            input[c][at] = in[c];
        }
        wobbleAt = (wobbleAt + 1) & 63;
        ++now;
        a.mark[at] = clean;
        if (fadeAt >= 0) b.mark[at] = clean;
        frame(a, channels, s);
        if (fadeAt >= 0) frame(b, channels, s);
        if (fadeAt >= latency + fadeLength)
            active = 1 - active, fadeAt = -1;
        return m;
    }
    void frame(Coder& k, int channels, const CodecSettings& s)
    {
        const auto& z = sizes[(size_t) k.size];
        const int n = z.mdct.n;
        if (now % (uint32_t) n != 0)
            return;
        const auto u = (uint32_t) mask;
        for (int c = 0; c < channels; ++c) // the window's fold, then the DCT-IV
        {
            const auto x = [&](int i) { return input[c][(now - (uint32_t) (2 * n - i)) & u] * z.mdct.window[(size_t) i]; };
            auto* d = decoded[c].data();
            for (int i = 0; i < n / 2; ++i)
                d[i] = -x(3 * n / 2 - 1 - i) - x(3 * n / 2 + i), d[n / 2 + i] = x(i) - x(n - 1 - i);
            z.mdct.dct4(d, scratch[0].data(), scratch[1].data(), scratch[2].data(), scratch[3].data());
        }
        k.grain.seed = (now / (uint32_t) n) * 2654435761u ^ 0x51f7c0deu;
        const int all = channels;
        if (s.stereo == 2) channels = 1; // mono: both sides are the same, coded once
        const bool joint = channels == 2 && s.stereo == 1;
        if (joint)
            for (int i = 0; i < n; ++i)
            {
                const double l = decoded[0][(size_t) i], r = decoded[1][(size_t) i], h = 0.70710678118654752440;
                decoded[0][(size_t) i] = (l + r) * h, decoded[1][(size_t) i] = (l - r) * h;
            }
        double eIn = 0, eOut = 0, eGone = 0;
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                work[c][(size_t) i] = decoded[c][(size_t) i], eIn += work[c][(size_t) i] * work[c][(size_t) i];
        if (s.loss > 0)
        {
            if (joint) code(k, z, 0, 2, s);
            else
                for (int c = 0; c < channels; ++c) code(k, z, c, c + 1, s);
        }
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
            {
                const double y = decoded[c][(size_t) i], g = work[c][(size_t) i] - y;
                eOut += y * y, eGone += g * g;
            }
        const double slow = 1 - std::exp(-n / (0.4 * fs));
        k.eIn += slow * (eIn - k.eIn), k.eOut += slow * ((s.kept ? eGone : eOut) - k.eOut);
        k.gain = k.eOut > 1e-12 && k.eIn > 1e-12 ? std::clamp(std::sqrt(k.eIn / k.eOut), 0.125, 8.0) : 1.0;
        // jitter: a badly clocked frame turns neighbouring coefficients into each other
        if (s.jitter > 0)
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i + 1 < n; i += 2)
                {
                    const double t = s.jitter * 1.57079632679489661923 * (2 * k.grain.next() - 1), cs = std::cos(t), sn = std::sin(t);
                    const double p = decoded[c][(size_t) i], q = decoded[c][(size_t) i + 1];
                    decoded[c][(size_t) i] = p * cs - q * sn, decoded[c][(size_t) i + 1] = p * sn + q * cs;
                }
        // Lost frames come in bursts (a two-state Gilbert chain); stereo loses them per side, joint and mono together.
        const double share = 0.9 * s.drop, burst = std::max(1.2, (0.018 + 0.042 * (1 - s.speed)) * fs / n);
        double recover = 1 / burst, fail = share < 1 ? recover * share / (1 - share) : 1;
        if (fail > 1) recover = (1 - share) / share, fail = 1;
        uint8_t mark = clean;
        for (int c = 0; c < 2; ++c)
        {
            const double r = k.random.next();
            k.bad[c] = s.drop > 0 && (k.bad[c] ? r >= recover : r < fail);
        }
        for (int c = 0; c < channels; ++c)
        {
            auto* d = decoded[c].data();
            if (k.bad[s.stereo == 0 ? c : 0])
            {
                mark = std::max(mark, (uint8_t) (s.hold ? held : gap));
                for (int i = 0; i < n; ++i) d[i] = s.hold ? k.last[c][(size_t) i] : 0;
            }
            else
                for (int i = 0; i < n; ++i) k.last[c][(size_t) i] = d[i];
        }
        if (joint)
            for (int i = 0; i < n; ++i)
            {
                const double m = decoded[0][(size_t) i], d = decoded[1][(size_t) i], h = 0.70710678118654752440;
                decoded[0][(size_t) i] = (m + d) * h, decoded[1][(size_t) i] = (m - d) * h;
            }
        const uint32_t first = now - (uint32_t) (2 * n) + (uint32_t) latency;
        for (int c = 0; c < all; ++c)
        {
            auto* d = decoded[std::min(c, channels - 1)].data();
            if (c < channels) z.mdct.dct4(d, scratch[0].data(), scratch[1].data(), scratch[2].data(), scratch[3].data());
            const auto add = [&](int i, double v) { k.sum[c][(first + (uint32_t) i) & u] += v * z.mdct.window[(size_t) i]; };
            for (int i = 0; i < n / 2; ++i)
                add(i, d[n / 2 + i]), add(n - 1 - i, -d[n / 2 + i]), add(3 * n / 2 - 1 - i, -d[i]), add(3 * n / 2 + i, -d[i]);
        }
        if (mark != clean) // the half of its window a frame owns
            for (int i = n / 2; i < 3 * n / 2; ++i)
                k.mark[(first + (uint32_t) i) & u] = mark;
    }
    // Throws away what matters least of channels [from, to) and coarsens the rest, in place.
    void code(Coder& k, const Size& z, int from, int to, const CodecSettings& s)
    {
        const int n = z.mdct.n, channels = to - from;
        const double l = s.loss, curve = std::pow(l, 0.8);
        const int limit = s.flat ? n : std::min(n, (int) std::floor(20000 * (1 - 0.6 * curve) / (fs / 2) * n + 0.5));
        double mean[2][maxBands] {}, root[2][maxBands] {};
        for (int c = 0; c < channels; ++c)
        {
            const auto* x = decoded[from + c].data();
            for (int b = 0; b < z.bands; ++b)
            {
                double e = 0;
                for (int i = z.edge[b]; i < z.edge[b + 1]; ++i) e += x[i] * x[i];
                mean[c][b] = e / (z.edge[b + 1] - z.edge[b]), root[c][b] = std::sqrt(mean[c][b]);
            }
            for (int b = 0; b < z.bands; ++b) // the ear: each band hides what is close to it, more upwards
            {
                double t = 1;
                if (!s.flat)
                {
                    t = z.ath[b];
                    for (int o = 0; o < z.bands; ++o)
                        t = std::max(t, mean[c][o] * spread(b - o));
                }
                for (int i = z.edge[b]; i < z.edge[b + 1]; ++i)
                    rank[(size_t) (c * n + i)] = i < limit ? x[i] * x[i] / t * (1 - 1e-6 * (i + 0.5 * c)) : 0; // ties (bands of one coefficient): the lower first
            }
        }
        const int total = channels * n, keep = std::clamp((int) std::floor(std::exp(-4.4228486291941369 * curve) * total + 0.5), 1, total); // 0.012 of them at 100 %
        std::copy(rank.begin(), rank.begin() + total, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + total);
        const double least = sorted[(size_t) (total - keep)], coarse = 0.03 + 1.9 * l * l, fill = 0.9 * l * (1 - l);
        for (int c = 0; c < channels; ++c)
        {
            auto* x = decoded[from + c].data();
            for (int b = 0; b < z.bands; ++b)
            {
                const double step = root[c][b] * coarse;
                bool any = false;
                for (int i = z.edge[b]; i < z.edge[b + 1]; ++i)
                {
                    const double r = rank[(size_t) (c * n + i)];
                    if (r >= least && r > 0 && step > 0)
                    {
                        const double q = std::floor(std::abs(x[i]) / step + 0.5) * step;
                        x[i] = x[i] < 0 ? -q : q, any = any || q > 0;
                    }
                    else
                        x[i] = 0;
                }
                if (!any && fill > 0) // an emptied band: a faint noise at its own level
                    for (int i = z.edge[b]; i < std::min(limit, z.edge[b + 1]); ++i)
                        x[i] = (k.grain.next() < 0.5 ? -fill : fill) * root[c][b];
            }
        }
    }
    static double spread(int distance) // 14 dB under the masker, 10 dB a band upwards, 27 dB a band downwards
    {
        static const auto table = [] {
            std::array<double, 2 * maxBands + 1> t {};
            for (int d = -maxBands; d <= maxBands; ++d)
                t[(size_t) (d + maxBands)] = std::pow(10.0, -(14 + (d > 0 ? 10.0 * d : -27.0 * d)) / 10);
            return t;
        }();
        return table[(size_t) (distance + maxBands)];
    }
};
} // namespace crumb
