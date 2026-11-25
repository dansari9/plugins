#pragma once
// crumb's picture: the last samples as they were, drawn from what the audio thread wrote for each of them (Crumb.h Scope),
// nothing else. A step is the lowest and highest sample of its time span (or the one sample it falls in), so the traces
// are the dry input and the output themselves: hold steps and bit steps are the output's own samples, a lost frame is
// where the codec lost it, what the codec took away is its residual, and a hot crest is where it clipped: the outline
// where the drive's clip pressed it down, a dot deeper every 2 dB up to 8; a rail at the edge where the output is past 0 dBFS.
// Three more pictures of the same samples, one language in all: warm is what comes out, cool is what was there before.
// spectrum: the output's bars, and over them what is missing up to the input's. steps: the crush stage, what went into the
// converter and the steps that came out. packets: a bar a packet, the input's height over it; lost ones hollow, repeats bright.
// Plain C++; the site's port is js/crumb-viz.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace crumb
{
struct Wave
{
    static constexpr int columns = 252, rows = 64, middle = rows / 2, capacity = 1 << 18; // 126 x 16 cells of 2 x 4 dots
    std::vector<float> dry = std::vector<float>((size_t) capacity), wet = dry, gone = dry, cut = dry, over = dry, pre = dry, held = dry;
    std::vector<uint8_t> mark = std::vector<uint8_t>((size_t) capacity);
    int64_t total = 0; // samples so far
    float peak = 0.1f; // the value drawn at 92 % of the height: the window's loudest sample, falling slowly

    void add(float d, float w, float g, uint8_t m, float c = 0, float o = 0, float p = 0, float h = 0)
    {
        const auto i = (size_t) (total++ & (capacity - 1));
        dry[i] = d, wet[i] = w, gone[i] = g, mark[i] = m, cut[i] = c, over[i] = o, pre[i] = p, held[i] = h;
    }
    void clear()
    {
        std::fill(dry.begin(), dry.end(), 0.0f), std::fill(wet.begin(), wet.end(), 0.0f), std::fill(gone.begin(), gone.end(), 0.0f);
        std::fill(cut.begin(), cut.end(), 0.0f), std::fill(over.begin(), over.end(), 0.0f);
        std::fill(pre.begin(), pre.end(), 0.0f), std::fill(held.begin(), held.end(), 0.0f);
        std::fill(std::begin(before), std::end(before), 0.0), std::fill(std::begin(after), std::end(after), 0.0);
        std::fill(mark.begin(), mark.end(), (uint8_t) 0);
        peak = 0.1f;
    }

    struct Range { float lo = 0, hi = 0; };
    struct Column { Range dry, wet, gone, cut, over; uint8_t mark = 0; }; // cut and over: the deepest clip down and up, dB
    std::vector<Column> column = std::vector<Column>((size_t) columns);
    using Dots = std::vector<std::pair<int, int>>; // column, row from the top
    Dots line, lost, taken, ghost, hot; // the output, where a frame was lost (gap or held), the residual, the dry input, clipping
    std::vector<std::pair<int, int>> water; // cells inside the output

    std::vector<float> state; // what the last of the three pictures was drawn from
    void reset()
    {
        for (auto* d : { &line, &lost, &taken, &ghost, &hot }) d->clear();
        water.clear(), state.clear();
    }

    // ---- spectrum: 63 bars, a seventh of an octave each, 40 Hz to 20 kHz, −72 to −12 dBFS; they rise at once and fall 1.5 dB a frame
    static constexpr int bars = 63;
    double before[bars] {}, after[bars] {}; // heights in dB over the floor
    void spectrum(double fs)
    {
        constexpr double floorDb = 72, range = 60, pi = 3.14159265358979323846;
        const int n = fs > 130000 ? 16384 : fs > 65000 ? 8192 : 4096;
        if ((int) re.size() != n)
        {
            re.assign((size_t) n, 0), im = re, window = re, powerDry.assign((size_t) (n / 2 + 1), 0), powerWet = powerDry, cosine.assign((size_t) (n / 2), 0), sine = cosine;
            for (int i = 0; i < n; ++i) window[(size_t) i] = 0.5 - 0.5 * std::cos(2 * pi * i / n);
            for (int i = 0; i < n / 2; ++i) cosine[(size_t) i] = std::cos(2 * pi * i / n), sine[(size_t) i] = std::sin(2 * pi * i / n);
        }
        for (int i = 0, j = 0; i < n; ++i) // the input as the real part, the output as the imaginary one, in bit-reversed order
        {
            const int64_t s = total - n + i;
            const bool known = s >= 0;
            const auto at = (size_t) (s & (capacity - 1));
            re[(size_t) j] = known ? (double) dry[at] * window[(size_t) i] : 0, im[(size_t) j] = known ? (double) wet[at] * window[(size_t) i] : 0;
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
        }
        for (int len = 2; len <= n; len <<= 1)
            for (int i = 0, step = n / len; i < n; i += len)
                for (int k = 0; k < len / 2; ++k)
                {
                    const auto a = (size_t) (i + k), b = (size_t) (i + k + len / 2), t = (size_t) (k * step);
                    const double x = re[b] * cosine[t] + im[b] * sine[t], y = im[b] * cosine[t] - re[b] * sine[t];
                    re[b] = re[a] - x, im[b] = im[a] - y, re[a] += x, im[a] += y;
                }
        double sumDry = 0, sumWet = 0; // running power: powerDry[k] is everything under bin k's upper edge; a full-scale sine is 1
        const double scale = 4 / ((double) n * (double) n * 0.375);
        powerDry[0] = powerWet[0] = 0;
        for (int k = 1; k <= n / 2; ++k)
        {
            const auto a = (size_t) k, b = (size_t) (n - k);
            const double xr = 0.5 * (re[a] + re[b]), xi = 0.5 * (im[a] - im[b]), yr = 0.5 * (im[a] + im[b]), yi = 0.5 * (re[b] - re[a]);
            powerDry[a] = sumDry += scale * (xr * xr + xi * xi), powerWet[a] = sumWet += scale * (yr * yr + yi * yi);
        }
        const auto under = [&](const std::vector<double>& p, double hz) {
            const double at = std::clamp(hz * n / fs + 0.5, 0.0, n / 2 - 1.0);
            const auto k = (size_t) at;
            return p[k] + (at - (double) k) * (p[k + 1] - p[k]);
        };
        reset();
        const auto height = [](double db) { return 1 + (int) std::floor(db / range * (rows - 4) + 0.5); };
        for (int b = 0; b < bars; ++b)
        {
            const double lo = 40 * std::pow(500.0, (double) b / bars), hi = 40 * std::pow(500.0, (double) (b + 1) / bars);
            const auto level = [&](const std::vector<double>& p) { return std::clamp(10 * std::log10(under(p, hi) - under(p, lo) + 1e-12) + floorDb, 0.0, range); };
            before[b] = std::max(level(powerDry), before[b] - 1.5), after[b] = std::max(level(powerWet), after[b] - 1.5);
            const int was = height(before[b]), is = height(after[b]);
            state.push_back((float) was), state.push_back((float) is);
            for (int x = 4 * b; x < 4 * b + 3; ++x)
            {
                for (int y = 0; y < is; ++y) line.push_back({ x, rows - 1 - y });
                for (int y = is + 1; y < was; ++y)
                    if (y >= was - 2) ghost.push_back({ x, rows - 1 - y });
                    else if ((x + y) % 2 == 0) taken.push_back({ x, rows - 1 - y });
            }
        }
    }

    // ---- steps: 48 holds, 4 to 40 ms, standing on a rising zero of what went into the converter
    void steps(double fs, double rate)
    {
        const double span = std::clamp(48 / std::min(fs, rate), 0.004, 0.04) * fs / columns;
        const int reach = (int) (0.04 * fs);
        const auto at = [this](const std::vector<float>& v, int64_t i) { return i < 0 ? 0.0f : v[(size_t) (i & (capacity - 1))]; };
        int64_t start = total - (int64_t) std::ceil(columns * span);
        for (int64_t i = start; i > std::max<int64_t>(1, start - reach); --i)
            if (at(pre, i - 1) <= 0 && at(pre, i) > 0)
            {
                start = i;
                break;
            }
        reset();
        float loudest = 0.02f;
        Range in[columns], out[columns];
        for (int k = 0; k < columns; ++k)
        {
            const int64_t from = start + (int64_t) std::floor(k * span), to = std::max(from + 1, start + (int64_t) std::floor((k + 1) * span));
            for (int64_t s = from; s < to; ++s)
            {
                const float p = s < total ? at(pre, s) : 0, h = s < total ? at(held, s) : 0;
                in[k].lo = s == from ? p : std::min(in[k].lo, p), in[k].hi = s == from ? p : std::max(in[k].hi, p);
                out[k].lo = s == from ? h : std::min(out[k].lo, h), out[k].hi = s == from ? h : std::max(out[k].hi, h);
            }
            loudest = std::max({ loudest, -in[k].lo, in[k].hi, -out[k].lo, out[k].hi });
            state.insert(state.end(), { in[k].lo, in[k].hi, out[k].lo, out[k].hi });
        }
        peak = std::max(loudest, peak * 0.8f), state.push_back(peak);
        const double gain = 0.92 / peak * (middle - 1);
        const auto level = [gain](float v) { return std::clamp((int) std::floor((double) v * gain + 0.5), -(middle - 2), middle - 2); };
        int a0 = 0, b0 = 0, c0 = 0, d0 = 0;
        for (int k = 0; k < columns; ++k)
        {
            const int hi = level(out[k].hi), lo = level(out[k].lo), top = level(in[k].hi), low = level(in[k].lo);
            if (k == 0) a0 = hi, b0 = lo, c0 = top, d0 = low;
            for (int t = std::min(lo, a0); t <= std::max(hi, b0); ++t) line.push_back({ k, middle - 1 - t }), line.push_back({ k, middle - t });
            for (int t = std::min(low, c0); t <= std::max(top, d0); ++t) ghost.push_back({ k, middle - t });
            for (int y = std::min(0, hi) + (hi < 0 ? 1 : 0); y < std::max(0, lo); ++y) // the step stands on the middle
                if ((k + y) % 2 == 0) taken.push_back({ k, middle - 1 - y });
            a0 = hi, b0 = lo, c0 = top, d0 = low;
        }
    }

    // ---- packets: a bar a codec frame (two or more together when they are short), on the codec's own count of samples
    static constexpr int packs = 42;
    void packets(double span, int64_t count)
    {
        const int64_t start = (int64_t) std::floor((double) count / span) - packs;
        reset();
        float loudest = 0.1f, in[packs] {}, out[packs] {};
        int share[packs] {}, kind[packs] {};
        for (int b = 0; b < packs; ++b)
        {
            const int64_t from = (int64_t) std::floor((double) (start + b) * span) + total - count, to = (int64_t) std::floor((double) (start + b + 1) * span) + total - count;
            int bad = 0, n = 0;
            for (int64_t s = from; s < to; ++s)
                if (s >= 0 && s < total && s >= total - capacity)
                {
                    const auto i = (size_t) (s & (capacity - 1));
                    in[b] = std::max(in[b], std::abs(dry[i])), out[b] = std::max(out[b], std::abs(wet[i]));
                    ++n;
                    if (mark[i] != 0) ++bad, kind[b] = mark[i];
                }
            share[b] = n > 0 && 2 * bad >= n ? kind[b] : 0;
            loudest = std::max({ loudest, in[b], out[b] });
            state.insert(state.end(), { in[b], out[b], (float) share[b] });
        }
        peak = std::max(loudest, peak * 0.95f), state.push_back(peak);
        const auto level = [this](float v) { return std::clamp((int) std::floor((double) v * 0.92 / peak * (middle - 1) + 0.5), 1, middle - 2); };
        for (int b = 0; b < packs; ++b)
        {
            const int was = level(in[b]), is = level(out[b]), x0 = 6 * b;
            for (int x = x0; x < x0 + 5; ++x)
                for (int y = 0; y < std::max(was, is); ++y)
                {
                    const bool rim = x == x0 || x == x0 + 4 || y >= was - 1;
                    auto* to = share[b] == 1 ? (y < was && rim ? &taken : nullptr) // lost: its outline, nothing in it
                             : y < is ? (share[b] == 2 ? &lost : &line)
                             : y > is && (x + y) % 2 == 0 ? &taken : nullptr;
                    if (to != nullptr) to->push_back({ x, middle - 1 - y }), to->push_back({ x, middle + y });
                }
        }
    }
    std::vector<double> re, im, window, powerDry, powerWet, cosine, sine;

    // ---- wave
    // span: samples per step, a step is `width` columns wide. triggered: the window starts where the dry input last rose
    // through zero, so it stands still (reach: how far back to look, in samples); otherwise it scrolls in whole steps, whose
    // edges are the multiples of span on a count of samples (the codec's frames) that stands at `count` now.
    void build(double span, int width, bool triggered, int reach, int64_t count = 0)
    {
        const int numSteps = columns / width;
        state.clear();
        int64_t start;
        if (triggered)
        {
            start = total - (int64_t) std::ceil(numSteps * span);
            const auto at = [this](int64_t i) { return i < 0 ? 0.0f : dry[(size_t) (i & (capacity - 1))]; };
            for (int64_t i = start; i > std::max<int64_t>(1, start - reach); --i)
                if (at(i - 1) <= 0 && at(i) > 0)
                {
                    start = i;
                    break;
                }
        }
        else
            start = (int64_t) std::floor((double) count / span) - numSteps; // in steps
        float loudest = triggered ? 0.02f : 0.1f;
        for (int k = 0; k < columns; ++k)
        {
            if (k % width != 0)
            {
                column[(size_t) k] = column[(size_t) k - 1];
                continue;
            }
            const int n = k / width;
            const int64_t from = triggered ? start + (int64_t) std::floor(n * span) : (int64_t) std::floor((double) (start + n) * span) + total - count;
            const int64_t to = std::max(from + 1, triggered ? start + (int64_t) std::floor((n + 1) * span) : (int64_t) std::floor((double) (start + n + 1) * span) + total - count);
            Column c;
            bool first = true;
            for (int64_t s = from; s < to; ++s)
            {
                const bool known = s >= 0 && s < total && s >= total - capacity;
                const auto i = (size_t) (s & (capacity - 1));
                const float v[3] { known ? dry[i] : 0, known ? wet[i] : 0, known ? gone[i] : 0 };
                Range* r[3] { &c.dry, &c.wet, &c.gone };
                for (int q = 0; q < 3; ++q)
                    r[q]->lo = first ? v[q] : std::min(r[q]->lo, v[q]), r[q]->hi = first ? v[q] : std::max(r[q]->hi, v[q]);
                if (known)
                    c.mark = std::max(c.mark, mark[i]), c.cut.lo = std::min(c.cut.lo, cut[i]), c.cut.hi = std::max(c.cut.hi, cut[i]), c.over.lo = std::min(c.over.lo, over[i]), c.over.hi = std::max(c.over.hi, over[i]);
                first = false;
            }
            loudest = std::max({ loudest, -c.dry.lo, c.dry.hi, -c.wet.lo, c.wet.hi });
            column[(size_t) k] = c;
        }
        peak = std::max(loudest, peak * (triggered ? 0.8f : 0.95f));

        const double gain = 0.92 / peak * (middle - 1);
        const auto level = [gain](float v) { return std::clamp((int) std::floor((double) v * gain + 0.5), -(middle - 2), middle - 2); };
        for (auto* d : { &line, &lost, &taken, &ghost, &hot }) d->clear();
        water.clear();
        std::vector<std::pair<int, int>> edge((size_t) columns);
        int last[6] {};
        for (int k = 0; k < columns; ++k)
        {
            const auto& c = column[(size_t) k];
            const int hi = level(c.wet.hi), lo = level(c.wet.lo);
            auto& to = c.mark != 0 ? lost : line;
            const int hi0 = k == 0 ? hi : last[0], lo0 = k == 0 ? lo : last[1];
            for (int t = std::min(hi0, hi); t <= std::max(hi0, hi); ++t) to.push_back({ k, middle - 1 - t });
            for (int t = std::min(lo0, lo); t <= std::max(lo0, lo); ++t) to.push_back({ k, middle - t });
            if (hi - lo > 1) to.push_back({ k, middle - 2 - hi }), to.push_back({ k, middle + 1 - lo }); // an outline is two dots thick
            last[0] = hi, last[1] = lo;
            const auto depth = [](float db) { return 2 + std::min(4, (int) std::floor(0.5 * (double) db + 0.5)); }; // dots
            if (c.cut.hi > 0)
                for (int row = middle - 1 - hi, end = std::min(middle - 1, row + depth(c.cut.hi)); row < end; ++row) hot.push_back({ k, row });
            if (c.cut.lo < 0)
                for (int row = middle - lo, end = std::max(middle, row - depth(-c.cut.lo)); row > end; --row) hot.push_back({ k, row });
            if (c.over.hi > 0) hot.push_back({ k, 0 }), hot.push_back({ k, 1 });
            if (c.over.lo < 0) hot.push_back({ k, rows - 1 }), hot.push_back({ k, rows - 2 });
            edge[(size_t) k] = { middle - 1 - hi, middle - lo };
            // the dry input: a checked fill between its lowest and highest, or its trace when that is one sample
            const auto fill = [&](const Range& r, int slot, Dots& dots, bool checked) {
                const int h = level(r.hi), l = level(r.lo), a = k == 0 ? h : std::max(h, last[slot + 1]), b = k == 0 ? l : std::min(l, last[slot]);
                last[slot] = h, last[slot + 1] = l;
                for (int row = middle - a; row <= middle - 1 - b || row == middle - a; ++row)
                    if (a - b < 3 || !checked || (row + k) % 2 == 0) dots.push_back({ k, row });
            };
            if (c.gone.hi > 0 || c.gone.lo < 0) fill(c.gone, 2, taken, false);
            else last[2] = last[3] = 0;
            fill(c.dry, 4, ghost, true);
        }
        for (int i = 0; i < columns / 2; ++i)
        {
            const int top = std::max(edge[(size_t) (2 * i)].first, edge[(size_t) (2 * i + 1)].first), bottom = std::min(edge[(size_t) (2 * i)].second, edge[(size_t) (2 * i + 1)].second);
            for (int j = 0; j < rows / 4; ++j)
                if (4 * j > top && 4 * j + 3 < bottom) water.push_back({ i, j });
        }
    }
};
} // namespace crumb
