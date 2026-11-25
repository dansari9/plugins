#pragma once
// crumb's picture: the last samples as they were, drawn from what the audio thread wrote for each of them (Crumb.h Scope),
// nothing else. A step is the lowest and highest sample of its time span (or the one sample it falls in), so the traces
// are the dry input and the output themselves: hold steps and bit steps are the output's own samples, a lost frame is
// where the codec lost it, what the codec took away is its residual, and a hot crest is where it clipped: the outline
// where the drive's clip pressed it down, a dot deeper every 2 dB up to 8; a rail at the edge where the output is past 0 dBFS.
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
    static constexpr int columns = 252, rows = 48, middle = rows / 2, capacity = 1 << 18; // 126 x 12 cells of 2 x 4 dots
    std::vector<float> dry = std::vector<float>((size_t) capacity), wet = dry, gone = dry, cut = dry, over = dry;
    std::vector<uint8_t> mark = std::vector<uint8_t>((size_t) capacity);
    int64_t total = 0; // samples so far
    float peak = 0.1f; // the value drawn at 92 % of the height: the window's loudest sample, falling slowly

    void add(float d, float w, float g, uint8_t m, float c = 0, float o = 0)
    {
        const auto i = (size_t) (total++ & (capacity - 1));
        dry[i] = d, wet[i] = w, gone[i] = g, mark[i] = m, cut[i] = c, over[i] = o;
    }
    void clear()
    {
        std::fill(dry.begin(), dry.end(), 0.0f), std::fill(wet.begin(), wet.end(), 0.0f), std::fill(gone.begin(), gone.end(), 0.0f);
        std::fill(cut.begin(), cut.end(), 0.0f), std::fill(over.begin(), over.end(), 0.0f);
        std::fill(mark.begin(), mark.end(), (uint8_t) 0);
        peak = 0.1f;
    }

    struct Range { float lo = 0, hi = 0; };
    struct Column { Range dry, wet, gone, cut, over; uint8_t mark = 0; }; // cut and over: the deepest clip down and up, dB
    std::vector<Column> column = std::vector<Column>((size_t) columns);
    using Dots = std::vector<std::pair<int, int>>; // column, row from the top
    Dots line, lost, taken, ghost, hot; // the output, where a frame was lost (gap or held), the residual, the dry input, clipping
    std::vector<std::pair<int, int>> water; // cells inside the output

    // span: samples per step, a step is `width` columns wide. triggered: the window starts where the dry input last rose
    // through zero, so it stands still (reach: how far back to look, in samples); otherwise it scrolls in whole steps, whose
    // edges are the multiples of span on a count of samples (the codec's frames) that stands at `count` now.
    void build(double span, int width, bool triggered, int reach, int64_t count = 0)
    {
        const int steps = columns / width;
        int64_t start;
        if (triggered)
        {
            start = total - (int64_t) std::ceil(steps * span);
            const auto at = [this](int64_t i) { return i < 0 ? 0.0f : dry[(size_t) (i & (capacity - 1))]; };
            for (int64_t i = start; i > std::max<int64_t>(1, start - reach); --i)
                if (at(i - 1) <= 0 && at(i) > 0)
                {
                    start = i;
                    break;
                }
        }
        else
            start = (int64_t) std::floor((double) count / span) - steps; // in steps
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
        int before[6] {};
        for (int k = 0; k < columns; ++k)
        {
            const auto& c = column[(size_t) k];
            const int hi = level(c.wet.hi), lo = level(c.wet.lo);
            auto& to = c.mark != 0 ? lost : line;
            const int hi0 = k == 0 ? hi : before[0], lo0 = k == 0 ? lo : before[1];
            for (int t = std::min(hi0, hi); t <= std::max(hi0, hi); ++t) to.push_back({ k, middle - 1 - t });
            for (int t = std::min(lo0, lo); t <= std::max(lo0, lo); ++t) to.push_back({ k, middle - t });
            if (hi - lo > 1) to.push_back({ k, middle - 2 - hi }), to.push_back({ k, middle + 1 - lo }); // an outline is two dots thick
            before[0] = hi, before[1] = lo;
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
                const int h = level(r.hi), l = level(r.lo), a = k == 0 ? h : std::max(h, before[slot + 1]), b = k == 0 ? l : std::min(l, before[slot]);
                before[slot] = h, before[slot + 1] = l;
                for (int row = middle - a; row <= middle - 1 - b || row == middle - a; ++row)
                    if (a - b < 3 || !checked || (row + k) % 2 == 0) dots.push_back({ k, row });
            };
            if (c.gone.hi > 0 || c.gone.lo < 0) fill(c.gone, 2, taken, false);
            else before[2] = before[3] = 0;
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
