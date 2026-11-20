#include "Check.h"
#include "PluginProcessor.h"
#include "chain/Latency.h"
#include "loops/Loops.h"

// M7: reorder crossfade, Magnitude, constant latency, snapshots, locks, undo and oversampling, through the processor.
namespace
{
using Proc = SnapProcessor;
using snap::ChainOrder;
using enum snap::Module;
constexpr ChainOrder reversed { drive, press, space, shift };

void set(Proc& p, const char* id, float v)
{
    auto* param = p.apvts.getParameter(id);
    param->setValueNotifyingHost(param->convertTo0to1(v));
}
void setAll(Proc& p, std::initializer_list<std::pair<const char*, float>> values)
{
    for (auto& [id, v] : values)
        set(p, id, v);
}

void prepare(Proc& p, double fs, int block, int ch)
{
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(ch));
    l.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(ch));
    CHECK(p.setBusesLayout(l));
    p.setPlayConfigDetails(ch, ch, fs, block);
    p.prepareToPlay(fs, block);
}

float input(int i, double fs) // the break loop's left channel, generated sample by sample from the same recipe
{
    static const auto loop = loops::make(1, 48000, 4);
    return 0.5f * loop.getSample(0, (int) ((long) i * 48000 / (long) fs) % loop.getNumSamples());
}

// Renders `seconds`; `each(p, start)` runs before every block. Returns channel 0, not latency-aligned.
template <typename Each>
std::vector<float> render(Proc& p, double fs, int block, int ch, double seconds, Each each, float (*source)(int, double) = input)
{
    juce::AudioBuffer<float> buffer(ch, block);
    juce::MidiBuffer midi;
    std::vector<float> out;
    for (int start = 0; start < (int) (seconds * fs); start += block)
    {
        each(p, start);
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i)
                buffer.setSample(c, i, source(start + i, fs) * (c == 0 ? 1.0f : 0.8f));
        setAudioThread(true);
        p.processBlock(buffer, midi);
        setAudioThread(false);
        out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + block);
    }
    return out;
}
std::vector<float> render(Proc& p, double fs, int block, int ch, double seconds) { return render(p, fs, block, ch, seconds, [](Proc&, int) {}); }

// Largest |out[i + latency] - in[i]|: the dry signal delayed by exactly the reported latency gives 0.
float residual(Proc& p, const std::vector<float>& out, double fs, float (*source)(int, double) = input, size_t skip = 0)
{
    float worst = 0;
    for (size_t i = skip; i + (size_t) p.getLatencySamples() < out.size(); ++i)
        worst = std::max(worst, std::abs(out[i + (size_t) p.getLatencySamples()] - source((int) i, fs)));
    return worst;
}

const std::initializer_list<std::pair<const char*, float>> allOff { { "subhiss_on", 0 }, { "shift_on", 0 }, { "space_on", 0 },
                                                                  { "press_on", 0 }, { "drive_on", 0 }, { "push_on", 0 } };
const std::initializer_list<std::pair<const char*, float>> amounts {
    { "sub_amt", 60 }, { "hiss_amt", 50 }, { "shift_amt", 40 }, { "space_amt", 50 }, { "press_amt", 70 }, { "drive_amt", 70 },
    { "push_amt", 60 }, { "sub_sens", 90 }, { "hiss_sens", 90 } };
const std::initializer_list<std::pair<const char*, float>> pushed { // the amounts plus the master
    { "sub_amt", 60 }, { "hiss_amt", 50 }, { "shift_amt", 40 }, { "space_amt", 50 }, { "press_amt", 70 }, { "drive_amt", 70 },
    { "push_amt", 60 }, { "tone", 50 }, { "air", 60 }, { "attack", 50 }, { "sustain", -40 }, { "in_gain", 6 }, { "out_gain", -4 },
    { "cut_lo", 200 }, { "sub_sens", 90 }, { "hiss_sens", 90 } };

float maxStep(const std::vector<float>& x, size_t from, size_t to)
{
    float worst = 0;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i)
        worst = std::max(worst, std::abs(x[i] - x[i - 1]));
    return worst;
}
double rmsDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to, double& ref)
{
    double d = 0, r = 0;
    for (size_t i = from; i < to; ++i)
        d += (a[i] - b[i]) * (a[i] - b[i]), r += b[i] * b[i];
    ref = std::sqrt(r / (double) (to - from));
    return std::sqrt(d / (double) (to - from));
}

// The new order runs beside the old one for its latency plus the warm-up, then the two crossfade over 25 ms.
void testReorder()
{
    constexpr double fs = 48000;
    const std::initializer_list<std::pair<const char*, float>> settings { { "drive_amt", 80 }, { "drive_mode", 2 }, { "press_amt", 80 } };
    for (int block : { 64, 1, 511 })
    {
        auto reference = [&](const ChainOrder& o) {
            Proc p;
            setAll(p, settings), p.setOrder(o);
            prepare(p, fs, block, 2);
            return render(p, fs, block, 2, 2.0);
        };
        const auto a = reference(snap::defaultChainOrder), b = reference(reversed);
        Proc p;
        setAll(p, settings);
        prepare(p, fs, block, 2);
        const int switchAt = (int) fs / block * block;
        int first = -1, last = -1;
        takeAudioAllocations();
        const auto y = render(p, fs, block, 2, 2.0, [&](Proc& q, int start) {
            if (start == switchAt)
                q.setOrder(reversed);
            if (q.reordering())
                first = first < 0 ? start : first, last = start + block;
        });
        CHECK(takeAudioAllocations() == 0 && !p.reordering() && p.order() == reversed);
        CHECK(p.reorderFadeSamples() == (int) std::lround(0.025 * fs));
        // The audio thread picks the order up at the next block. Shift (on at amount 0) keeps its STFT latency and drive
        // its Target lookahead; the warm-up covers both. reordering() is sampled per block, hence the tolerance.
        const int expected = snap::Shift::latencyFor(fs, false) + snap::TargetFocusMix::lookaheadFor(fs, false)
                             + (int) std::lround(fs * snap::constants::reorderWarmupMs * 0.001) + p.reorderFadeSamples();
        CHECK(first == switchAt + block && std::abs(last - first - expected) <= 2 * block);
        CHECK(std::equal(y.begin(), y.begin() + switchAt, a.begin()));
        const float before = std::max(maxStep(a, (size_t) first, (size_t) last), maxStep(b, (size_t) first, (size_t) last));
        CHECK(maxStep(y, (size_t) first, (size_t) last) <= 1.5f * before);
        // After the fade the new order matches a render that ran it from the start (measured about -45 dB, checked at
        // -30 dB). Space is left out: its tail from before the switch is not carried over.
        double ref = 0;
        const auto d = rmsDiff(y, b, (size_t) last + 4800, y.size(), ref);
        CHECK(d < 0.0316 * ref);
    }
}

void testMagnitudeZero()
{
    for (bool low : { false, true })
    {
        Proc p;
        setAll(p, pushed), set(p, "magnitude", 0), set(p, "low_latency", low), p.setOrder(reversed);
        prepare(p, 48000, 256, 2);
        CHECK(residual(p, render(p, 48000, 256, 2, 1.0), 48000) <= 1.0e-3f); // the input peaks at -6 dBFS: <= -60 dB
    }
}

float sine1k(int i, double fs) { return 0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * 1000 * i / fs); }

std::vector<ChainOrder> permutations()
{
    std::vector<ChainOrder> all;
    auto o = snap::defaultChainOrder;
    std::sort(o.begin(), o.end());
    do
        all.push_back(o);
    while (std::next_permutation(o.begin(), o.end()));
    return all;
}

void testLatencyIdentity()
{
    const auto orders = permutations();
    CHECK(orders.size() == 24);
    for (double fs : { 44100.0, 96000.0 })
    {
        Proc p;
        setAll(p, amounts), setAll(p, allOff);
        prepare(p, fs, 128, 2);
        const int normal = p.getLatencySamples();
        CHECK(normal == snap::chainLatency(fs, false));
        juce::Random r(7);
        for (auto& o : orders)
        {
            p.setOrder(o);
            for (auto& m : snap::modules)
                set(p, m.onId.data(), (float) r.nextBool());
            CHECK(p.getLatencySamples() == normal);
        }
        // All off, after a reorder has settled: the dry signal on exactly the reported latency.
        setAll(p, allOff), p.setOrder(reversed);
        CHECK(residual(p, render(p, fs, 128, 2, 1.0), fs, input, (size_t) (0.3 * fs)) <= 1.0e-6f);

        set(p, "bypass", 1);
        setAll(p, { { "subhiss_on", 1 }, { "press_on", 1 }, { "drive_on", 1 } });
        CHECK(residual(p, render(p, fs, 128, 2, 1.0), fs, input, (size_t) (0.3 * fs)) <= 1.0e-6f);
        set(p, "bypass", 0);

        // Low Latency Mode: exactly 64, and shift does nothing.
        set(p, "low_latency", 1);
        CHECK(p.getLatencySamples() == 64);
        auto shifted = [&](float amount) {
            Proc q;
            setAll(q, { { "shift_amt", amount }, { "low_latency", 1 } });
            prepare(q, fs, 128, 2);
            return render(q, fs, 128, 2, 0.5);
        };
        CHECK(shifted(100) == shifted(0));
        set(p, "low_latency", 0);
        CHECK(p.getLatencySamples() == normal);

        // Oversampling: another constant latency; the half-band filters are not exact, so a 1 kHz sine all-off comes back
        // within -60 dB of full scale (measured about -100 dB).
        set(p, "oversampling", 1);
        const int over = p.getLatencySamples();
        CHECK(over != normal);
        for (auto& o : orders)
        {
            p.setOrder(o);
            CHECK(p.getLatencySamples() == over);
        }
        setAll(p, allOff);
        CHECK(residual(p, render(p, fs, 128, 2, 0.5, [](Proc&, int) {}, sine1k), fs, sine1k, (size_t) (0.3 * fs)) <= 1.0e-3f);
        set(p, "low_latency", 1);
        CHECK(p.getLatencySamples() == 64);
        set(p, "low_latency", 0);
        CHECK(p.getLatencySamples() == over);
        setAll(p, pushed);
        bool finite = true;
        for (float y : render(p, fs, 128, 2, 0.5))
            finite = finite && std::isfinite(y) && std::abs(y) < 8;
        CHECK(finite);
    }
}

std::vector<float> values(Proc& p)
{
    std::vector<float> v;
    for (auto* param : p.getParameters())
        v.push_back(param->getValue());
    return v;
}
bool same(const std::vector<float>& a, const std::vector<float>& b) // normalised, within a float ulp or so
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](float x, float y) { return std::abs(x - y) <= 1.0e-6f; });
}
void randomise(Proc& p, juce::Random& r) // legal values only (toggles and choices land on a step)
{
    for (auto& s : snap::parameterTable())
        if (auto* param = p.apvts.getParameter(s.id); !snap::isSessionToggle(s.id))
            param->setValueNotifyingHost(param->convertTo0to1(param->convertFrom0to1(r.nextFloat())));
    auto o = p.order();
    std::swap(o[(size_t) r.nextInt(4)], o[(size_t) r.nextInt(4)]);
    p.setOrder(o);
}

void testSnapshots()
{
    juce::Random r(3);
    Proc p;
    CHECK(!p.hasSnapshot(2) && !p.recallSnapshot(2));
    randomise(p, r), p.setOrder(reversed);
    const auto stored = values(p);
    p.storeSnapshot(1);
    CHECK(p.hasSnapshot(1));
    randomise(p, r);
    const auto other = values(p);
    CHECK(!p.recallSnapshot(0) && values(p) == other);
    CHECK(p.recallSnapshot(1) && same(values(p), stored) && p.order() == reversed);
    const auto recalledOnce = values(p); // a log-range value may move by one ulp on its first round trip, then stays put
    p.storeSnapshot(1), randomise(p, r);
    CHECK(p.recallSnapshot(1) && values(p) == recalledOnce);

    juce::MemoryBlock m;
    p.getStateInformation(m);
    Proc q;
    q.setStateInformation(m.getData(), (int) m.getSize());
    CHECK(q.hasSnapshot(1) && !q.hasSnapshot(0));
    randomise(q, r);
    CHECK(q.recallSnapshot(1) && q.order() == reversed);
    const auto recalled = values(q);
    for (size_t i = 0; i < stored.size(); ++i)
        CHECK(snap::isSessionToggle(snap::parameterTable()[i].id) || std::abs(recalled[i] - stored[i]) <= 1.0e-6f);
}

void testLocks()
{
    Proc p;
    setAll(p, { { "press_amt", 33 }, { "press_mode", 2 }, { "tone", -20 }, { "air", 15 }, { "drive_amt", 10 }, { "low_latency", 1 } });
    p.setOrder({ press, shift, space, drive });
    p.setLocked((int) press, true), p.setLocked(snap::masterLock, true);
    p.storeSnapshot(3);

    Proc source;
    juce::Random r(11);
    randomise(source, r);
    setAll(source, { { "press_amt", 90 }, { "tone", 80 }, { "drive_amt", 70 }, { "low_latency", 0 }, { "bypass", 1 } });
    source.setOrder(reversed);
    juce::MemoryBlock preset;
    source.getStateInformation(preset);

    const auto before = values(p);
    CHECK(p.loadPreset(preset.getData(), (int) preset.getSize()));
    Proc loaded; // the preset's values as they come back through the state (toggles and choices snap, log ranges round)
    loaded.setStateInformation(preset.getData(), (int) preset.getSize());
    const auto after = values(p), incoming = values(loaded);
    for (size_t i = 0; i < after.size(); ++i)
    {
        const auto* id = snap::parameterTable()[i].id;
        const int group = snap::lockGroup(id);
        const bool kept = snap::isSessionToggle(id) || group == (int) press || group == snap::masterLock;
        CHECK(kept ? after[i] == before[i] : std::abs(after[i] - incoming[i]) <= 1.0e-6f);
    }
    CHECK(p.order() == ChainOrder({ press, drive, space, shift })); // press pinned in slot 0, the rest in the preset's order
    CHECK(p.locked((int) press) && p.locked(snap::masterLock) && !p.locked((int) drive) && p.hasSnapshot(3));

    // A host restore is a whole session: its (absent) locks replace these.
    p.setStateInformation(preset.getData(), (int) preset.getSize());
    CHECK(!p.locked((int) press) && !p.locked(snap::masterLock) && values(p) == values(loaded) && p.order() == reversed);
}

void testUndo()
{
    Proc p;
    prepare(p, 48000, 64, 2);
    p.setOrder(reversed);
    CHECK(p.undo() && p.order() == snap::defaultChainOrder);
    CHECK(p.redo() && p.order() == reversed);

    p.beginUndoStep();
    set(p, "drive_amt", 40);
    p.beginUndoStep();
    set(p, "drive_amt", 75);
    CHECK(p.undo() && p.apvts.getRawParameterValue("drive_amt")->load() == 40.0f);

    juce::Random r(5);
    const auto plain = values(p);
    p.storeSnapshot(0);
    randomise(p, r);
    const auto random = values(p);
    p.recallSnapshot(1); // empty: nothing to undo here
    CHECK(p.recallSnapshot(0) && same(values(p), plain));
    CHECK(p.undo() && same(values(p), random));

    Proc source;
    randomise(source, r);
    juce::MemoryBlock preset;
    source.getStateInformation(preset);
    p.loadPreset(preset.getData(), (int) preset.getSize());
    CHECK(p.order() == source.order() && p.undo() && same(values(p), random));

    p.setStateInformation(preset.getData(), (int) preset.getSize());
    CHECK(!p.undo());
}

// The full chain under reorders, mode switches and IR / noise changes: never an audio-thread allocation.
void testNoAllocations()
{
    const auto orders = permutations();
    for (double fs : { 44100.0, 96000.0 })
        for (int block : { 1, 64, 4096 })
            for (int ch : { 1, 2 })
            {
                Proc p;
                setAll(p, pushed), set(p, "clip_on", 1), set(p, "hiss_post", 1);
                prepare(p, fs, block, ch);
                takeAudioAllocations();
                int step = 0;
                bool ok = true;
                for (float y : render(p, fs, block, ch, block == 1 ? 0.7 : 1.6, [&](Proc& q, int start) {
                         if (start / (int) (0.3 * fs) == step)
                             return;
                         step = start / (int) (0.3 * fs);
                         q.setOrder(orders[(size_t) (step * 7) % orders.size()]);
                         setAll(q, { { "low_latency", (float) (step % 3 == 2) }, { "space_type", (float) (step * 3 % 20) },
                                     { "hiss_type", (float) (step * 5 % 18) } });
                     }))
                    ok = ok && std::isfinite(y) && std::abs(y) <= 8;
                CHECK(ok && takeAudioAllocations() == 0);
            }
}
} // namespace

void testChain()
{
    testReorder();
    testMagnitudeZero();
    testLatencyIdentity();
    testSnapshots();
    testLocks();
    testUndo();
    testNoAllocations();
}
