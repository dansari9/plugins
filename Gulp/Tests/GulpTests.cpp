// GulpTests: drives the real processor and editor without a host. Exit code 1 on failure.
//   GulpTests --screenshot a.png [id=value ...]   render the editor (100 %, 2x pixels) after a few cycles of bass + kick
//   at preset 03, with any parameters changed (the design mock: shift=20 soothe=40)
#include "Gulp.h"
#include "BinaryData.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <iostream>
#include <new>
#include <thread>

// Real-time check: count heap allocations made on a thread flagged as "audio" (as crumb).
static thread_local bool onAudioThread = false;
static std::atomic<int> audioAllocations { 0 };
static void countAllocation() { audioAllocations += onAudioThread ? 1 : 0; }
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#define GULP_COUNTS_MALLOC 1
extern "C" void* __libc_malloc(size_t);
extern "C" void* __libc_calloc(size_t, size_t);
extern "C" void* __libc_realloc(void*, size_t);
extern "C" void* malloc(size_t n) noexcept { return countAllocation(), __libc_malloc(n); }
extern "C" void* calloc(size_t n, size_t s) noexcept { return countAllocation(), __libc_calloc(n, s); }
extern "C" void* realloc(void* p, size_t n) noexcept { return countAllocation(), __libc_realloc(p, n); }
#endif
void* operator new(std::size_t n)
{
    countAllocation();
    if (void* p = std::malloc(n == 0 ? 1 : n))
        return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a)
{
    countAllocation();
    void* p = nullptr;
    if (posix_memalign(&p, std::max(sizeof(void*), (std::size_t) a), n == 0 ? 1 : n) == 0)
        return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return countAllocation(), std::malloc(n == 0 ? 1 : n); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
static void escape(void* p) { asm volatile("" : : "r"(p) : "memory"); }

static int failures = 0;
#define CHECK(c) ((c) ? (void) 0 : (void) (++failures, std::cerr << "FAIL " << __LINE__ << ": " #c "\n"))

using Proc = GulpProcessor;
using Settings = std::vector<std::pair<const char*, float>>;
constexpr double pi = juce::MathConstants<double>::pi;
// FROZEN: this list is the contract with every saved session. The test fails if the plug-in's list changes.
static const juce::StringArray ids { "trigger", "tempoSync", "rate", "freeHz", "curve", "slope", "invert", "depth", "shift",
                                     "loop",    "thresh",    "band", "split",  "output", "color", "tone",  "stereo", "soothe", "lookahead" };

static void set(Proc& p, const char* id, float v) { auto* q = p.apvts.getParameter(id); q->setValueNotifyingHost(q->convertTo0to1(v)); }
static void set(Proc& p, const Settings& s) { for (auto& [id, v] : s) set(p, id, v); }
static float get(Proc& p, const juce::String& id) { return p.apvts.getRawParameterValue(id)->load(); }
static std::vector<float> params(Proc& p)
{
    std::vector<float> v;
    for (auto& id : ids) v.push_back(get(p, id)); // plain values: a switch's getValue() keeps the unquantised float
    return v;
}

// Main bus ch (1 or 2) and a sidechain of sc channels (0 = disabled).
static void prepare(Proc& p, double fs, int block, int ch = 2, int sc = 2)
{
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(ch));
    l.inputBuses.add(sc > 0 ? juce::AudioChannelSet::canonicalChannelSet(sc) : juce::AudioChannelSet::disabled());
    l.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(ch));
    CHECK(p.setBusesLayout(l));
    p.setRateAndBufferSizeDetails(fs, block);
    p.prepareToPlay(fs, block);
}

struct Host : juce::AudioPlayHead
{
    double bpm = 124, ppq = 0;
    bool playing = true, known = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        if (!known) return {};
        PositionInfo i;
        i.setBpm(bpm), i.setPpqPosition(ppq), i.setIsPlaying(playing);
        return i;
    }
};

// The test material: a 55 Hz bass with a harmonic, a kick (150 -> 50 Hz) every half second for the sidechain.
static float bass(int i, double fs = 48000) { const double t = i / fs; return (float) (0.4 * std::sin(2 * pi * 55 * t) + 0.1 * std::sin(2 * pi * 110 * t + 0.3)); }
static float kick(int i, double fs = 48000)
{
    const double t = std::fmod(i / fs, 0.5);
    return (float) (0.8 * std::sin(2 * pi * (50 * t + 2 * (1 - std::exp(-t / 0.02)))) * std::exp(-t / 0.12));
}
static float noise(int i) { juce::uint32 h = (juce::uint32) i * 2654435761u; h = (h ^ (h >> 15)) * 0x2c1b3c6du, h ^= h >> 12; return (float) ((h >> 8) * 0x1p-23 - 1); }

// Processes `samples` in blocks (0 = random sizes 1..2048): in(i, channel), the sidechain sc(i) on all its channels,
// note-ons at the given samples, the host's ppq advancing (perBlock may move it). Returns channel 0 (and 1 in right).
struct Run
{
    std::function<float(int, int)> in = [](int i, int c) { return bass(i) * (c == 0 ? 1.0f : 0.7f); };
    std::function<float(int)> sc = [](int i) { return kick(i); };
    std::vector<int> notes;
    Host* host = nullptr;
    std::function<void(int start)> perBlock;
    std::vector<float>* right = nullptr;
    std::vector<double>* ppqs = nullptr; // the host ppq at every sample
    int offset = 0;                      // where on the timeline in, sc and the notes start
};
static std::vector<float> run(Proc& p, int samples, int block, const Run& r = {})
{
    const int ch = p.getMainBusNumOutputChannels(), total = juce::jmax(p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    const double fs = p.getSampleRate();
    juce::AudioBuffer<float> buffer(total, 2048);
    juce::MidiBuffer midi;
    midi.ensureSize(4096);
    juce::Random sizes(3);
    std::vector<float> out;
    p.setPlayHead(r.host);
    for (int start = 0, n = 0; start < samples; start += n)
    {
        n = juce::jmin(samples - start, block > 0 ? block : 1 + sizes.nextInt(2048));
        if (r.perBlock) r.perBlock(start);
        buffer.setSize(total, n, false, false, true);
        for (int c = 0; c < total; ++c)
            for (int i = 0; i < n; ++i)
                buffer.setSample(c, i, c < ch ? r.in(r.offset + start + i, c) : r.sc(r.offset + start + i));
        midi.clear();
        for (int t : r.notes)
            if (t >= r.offset + start && t < r.offset + start + n) midi.addEvent(juce::MidiMessage::noteOn(1 + t % 16, 36, (juce::uint8) 100), t - r.offset - start);
        if (r.ppqs) for (int i = 0; i < n; ++i) r.ppqs->push_back(r.host->ppq + i * r.host->bpm / 60 / fs);
        onAudioThread = true;
        p.processBlock(buffer, midi);
        onAudioThread = false;
        if (r.host) r.host->ppq += n * r.host->bpm / 60 / fs;
        for (int i = 0; i < n; ++i)
        {
            out.push_back(buffer.getSample(0, i));
            if (r.right) r.right->push_back(buffer.getSample(ch - 1, i));
        }
    }
    p.setPlayHead(nullptr);
    return out;
}
static Run dc(float v) { Run r; r.in = [v](int, int) { return v; }; return r; }
static double maxStep(const std::vector<float>& y, size_t from = 1)
{
    double m = 0;
    for (size_t i = juce::jmax<size_t>(1, from); i < y.size(); ++i) m = juce::jmax(m, (double) std::abs(y[i] - y[i - 1]));
    return m;
}
static double rmsDb(const std::vector<float>& y, size_t from)
{
    double e = 0;
    for (size_t i = from; i < y.size(); ++i) e += (double) y[i] * y[i];
    return 10 * std::log10(e / (double) (y.size() - from) + 1e-30);
}

static juce::MemoryBlock blob(const juce::String& xml) // the copyXmlToBinary layout, text verbatim
{
    juce::MemoryOutputStream m;
    m.writeInt(0x21324356), m.writeInt((int) xml.getNumBytesAsUTF8()), m.write(xml.toRawUTF8(), xml.getNumBytesAsUTF8());
    return m.getMemoryBlock();
}
static void load(Proc& p, const juce::MemoryBlock& m) { p.setStateInformation(m.getData(), (int) m.getSize()); }
static juce::MemoryBlock save(Proc& p) { juce::MemoryBlock m; p.getStateInformation(m); return m; }

static void testIdentity()
{
    CHECK(JucePlugin_PluginCode == 0x47756c70);       // 'Gulp'
    CHECK(JucePlugin_ManufacturerCode == 0x44776b73); // 'Dwks'
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_AUMainType)) == "'aumf'");
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_CFBundleIdentifier)) == "com.dripworks.gulp");
    CHECK(juce::String(JucePlugin_Name) == "gulp" && juce::String(JucePlugin_Manufacturer) == "daveydidit" && juce::String(JucePlugin_VersionString) == "0.2.1");
    static_assert(JUCE_VST3_CAN_REPLACE_VST2 == 0 && JUCE_FORCE_USE_LEGACY_PARAM_IDS == 0, "frozen host-facing IDs");
    CHECK(!JucePlugin_IsSynth && JucePlugin_WantsMidiInput && !JucePlugin_ProducesMidiOutput);
    // Golden: id -> range and default (floats), item list and default (choices), default (switches).
    struct Golden { const char* id; float lo, hi, def; const char* items; };
    const Golden golden[] = { { "trigger", 0, 2, 0, "host,midi,audio" }, // index 0 was named "sync" until 0.2.0: only its text changed
                              { "tempoSync", 0, 1, 1, "" },
                              { "rate", 0, 15, 8, "1/32,1/16T,1/16,1/16D,1/8T,1/8,1/8D,1/4T,1/4,1/4D,1/2T,1/2,1/2D,1/1,2/1,4/1" },
                              { "freeHz", 0.1f, 20, 2, nullptr },
                              { "curve", 0, 11, 0, "classic,tight,punch,round,long,slam,sub,sub long,trim,suck,wave,double" },
                              { "slope", -100, 100, 0, nullptr }, { "invert", 0, 1, 0, "" }, { "depth", 0, 100, 100, nullptr },
                              { "shift", -50, 50, 0, nullptr }, { "loop", 0, 1, 0, "" }, { "thresh", -60, 0, -24, nullptr },
                              { "band", 0, 2, 0, "full,lows,highs" }, { "split", 20, 5120, 120, nullptr }, { "output", -24, 12, 0, nullptr },
                              { "color", 0, 7, 0, "vol,lows,highs,dark,thin,pinch,hollow,grit" }, { "tone", 0, 100, 50, nullptr },
                              { "stereo", 0, 2, 0, "both,mid,side" }, { "soothe", 0, 100, 0, nullptr }, { "lookahead", 0, 1, 0, "" } };
    Proc p;
    CHECK(p.getParameters().size() == 19 && p.getName() == "gulp" && p.acceptsMidi() && p.getNumPrograms() == 10);
    for (int i = 0; i < p.getParameters().size(); ++i)
    {
        auto* q = dynamic_cast<juce::RangedAudioParameter*>(p.getParameters()[i]);
        const auto& g = golden[i];
        CHECK(q != nullptr && q->paramID == ids[i] && q->paramID == g.id && q->getVersionHint() == (i < 17 ? 1 : i < 18 ? 2 : 3));
        CHECK(q->isAutomatable() == (q->paramID != "band" && q->paramID != "tone") && !q->isMetaParameter()); // legacy: inert, kept
        const auto& r = q->getNormalisableRange();
        CHECK(r.start == g.lo && r.end == g.hi && get(p, g.id) == g.def); if (!(r.start == g.lo && r.end == g.hi && get(p, g.id) == g.def)) std::cerr << std::setprecision(10) << g.id << " " << r.start << " " << r.end << " " << get(p, g.id) << "\n";
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*>(q)) CHECK(g.items != nullptr && c->choices.joinIntoString(",") == g.items);
        if (dynamic_cast<juce::AudioParameterBool*>(q) != nullptr) CHECK(g.items != nullptr && juce::String(g.items).isEmpty());
        if (dynamic_cast<juce::AudioParameterFloat*>(q) != nullptr) CHECK(g.items == nullptr);
    }
    CHECK(p.getProgramName(3) == juce::String::fromUTF8("house \xc2\xb7 classic pump"));
    // Buses: stereo or mono main (in = out), the sidechain disabled by default, mono or stereo.
    CHECK(!p.getBus(true, 1)->isEnabled() && p.getBusCount(true) == 2);
    for (int ch : { 1, 2 }) for (int sc : { 0, 1, 2 }) prepare(p, 44100, 512, ch, sc);
    juce::AudioProcessor::BusesLayout bad;
    bad.inputBuses.add(juce::AudioChannelSet::mono()), bad.inputBuses.add(juce::AudioChannelSet::disabled()), bad.outputBuses.add(juce::AudioChannelSet::stereo());
    CHECK(!p.checkBusesLayoutSupported(bad));
    CHECK(p.getLatencySamples() == 0 && p.getTailLengthSeconds() == 0);
}

// Frozen v1 fixture, exactly as gulp 0.1.0 writes it. Must load in every future version.
static juce::MemoryBlock v1Fixture()
{
    return blob(R"(<?xml version="1.0" encoding="UTF-8"?>)" "\n"
                R"(<GULP stateVersion="1" preset="-1" presetName="user 07" ab="1" slotA="3;0,1,0.533333361,0.5,0,0.5,0,1,0.5,0,0.600000024,0.5,0.400000006,0.666666687,0,0.5,0")"
                R"( slotNameA="" editorWidth="1536"><PARAM id="band" value="2.0"/><PARAM id="color" value="5.0"/><PARAM id="curve" value="7.0"/>)"
                R"(<PARAM id="depth" value="62.5"/><PARAM id="freeHz" value="3.5"/><PARAM id="invert" value="1.0"/><PARAM id="loop" value="1.0"/>)"
                R"(<PARAM id="output" value="-3.5"/><PARAM id="rate" value="5.0"/><PARAM id="shift" value="12.5"/><PARAM id="slope" value="-40.0"/>)"
                R"(<PARAM id="split" value="250.0"/><PARAM id="stereo" value="1.0"/><PARAM id="tempoSync" value="0.0"/><PARAM id="thresh" value="-30.0"/>)"
                R"(<PARAM id="tone" value="75.0"/><PARAM id="trigger" value="2.0"/></GULP>)");
}

// Frozen v2 fixture, as gulp 0.2.0 writes it (18 parameters, slots of 18 values; no lookahead): loads with lookahead off.
static juce::MemoryBlock v2Fixture(int ab = 1)
{
    return blob(juce::String(R"(<?xml version="1.0" encoding="UTF-8"?>)" "\n"
                R"(<GULP stateVersion="2" preset="-1" presetName="user 03" ab="1" slotA="5;1,1,0.533333361,0.5,0,0.5,0,1,0.5,0,0.600000024,0.5,0.400000006,0.666666687,0.142857149,0.5,0,0.25")"
                R"( slotNameA="" slotB="2;0.5,0,1,0.5,0,0.5,0,1,0.5,1,0.600000024,0,0.400000006,0.666666687,0,0.5,0,0")"
                R"( slotNameB="user 03"><PARAM id="trigger" value="2.0"/><PARAM id="tempoSync" value="1.0"/><PARAM id="rate" value="15.0"/>)"
                R"(<PARAM id="freeHz" value="2.0"/><PARAM id="curve" value="3.0"/><PARAM id="slope" value="12.5"/><PARAM id="invert" value="0.0"/>)"
                R"(<PARAM id="depth" value="80.0"/><PARAM id="shift" value="0.0"/><PARAM id="loop" value="1.0"/><PARAM id="thresh" value="-30.0"/>)"
                R"(<PARAM id="band" value="0.0"/><PARAM id="split" value="150.0"/><PARAM id="output" value="0.0"/><PARAM id="color" value="1.0"/>)"
                R"(<PARAM id="tone" value="50.0"/><PARAM id="stereo" value="0.0"/><PARAM id="soothe" value="25.0"/></GULP>)").replace("ab=\"1\"", "ab=\"" + juce::String(ab) + "\""));
}

static void randomise(Proc& p, juce::Random& r) { for (auto* q : p.getParameters()) q->setValueNotifyingHost(r.nextFloat()); }

static void testState()
{
    juce::Random r(42);
    for (int n = 0; n < 200; ++n) // into a brand-new instance: identical state, parameters, A/B, preset name and audio
    {
        Proc a, b;
        a.setCurrentProgram(r.nextInt(10)), randomise(a, r);
        if (r.nextBool()) a.selectAb(1), randomise(a, r), r.nextBool() ? a.selectAb(0) : (void) 0;
        if (r.nextInt(4) == 0) a.preset = -1;
        const auto m = save(a);
        load(b, m);
        CHECK(m == save(b) && params(a) == params(b) && a.presetName() == b.presetName() && a.abSlot() == b.abSlot());
        if (a.abSlot() == 1) a.selectAb(0), b.selectAb(0), CHECK(params(a) == params(b));
        CHECK(juce::AudioProcessor::getXmlFromBinary(m.getData(), (int) m.getSize())->getIntAttribute("stateVersion") == 2);
        { // bass + kick sidechain + MIDI + moving host ppq: bit-identical

            Proc c;
            load(c, save(a));
            prepare(a, 48000, 256), prepare(c, 48000, 256);
            Host ha, hc;
            Run ra, rc;
            ra.notes = rc.notes = { 300, 9000, 20000, 33333 }, ra.host = &ha, rc.host = &hc;
            CHECK(run(a, 48000, 0, ra) == run(c, 48000, 0, rc));
        }
    }
    // Missing parameters load at their defaults; a newer stateVersion loads what it knows; garbage changes nothing.
    Proc p;
    juce::Random q(7);
    randomise(p, q);
    load(p, blob(R"(<GULP stateVersion="1"><PARAM id="depth" value="40"/></GULP>)"));
    for (auto* param : p.getParameters())
        CHECK(param == p.apvts.getParameter("depth") ? get(p, "depth") == 40 : param->getValue() == param->getDefaultValue());
    CHECK(p.presetName() == "custom");
    load(p, blob(R"(<GULP stateVersion="9" futureThing="x"><PARAM id="curve" value="5"/><PARAM id="tilt" value="3"/><PARAM id="split" value="1e9"/></GULP>)"));
    CHECK(get(p, "curve") == 5 && get(p, "split") == 5120 && get(p, "depth") == 100);
    const auto before = save(p);
    for (const char* bad : { "<CRUMB><PARAM id=\"depth\" value=\"1\"/></CRUMB>", "<GULP", "<GULP><PARAM id=\"depth\" value=\"1\"/>", "" })
        load(p, blob(bad)), CHECK(save(p) == before);
    load(p, blob(R"(<GULP><PARAM id="depth" value="nan"/><PARAM id="thresh" value="-1e300"/></GULP>)"));
    CHECK(get(p, "depth") == 100 && get(p, "thresh") == -60);
    p.setStateInformation("garbage!", 8), p.setStateInformation(nullptr, 0), CHECK(get(p, "thresh") == -60);
    // The checked-in v1 blob (lookahead was on: a missing parameter loads at its default, off).
    set(p, "lookahead", 1), load(p, v1Fixture());
    CHECK(get(p, "trigger") == 2 && get(p, "tempoSync") == 0 && get(p, "rate") == 5 && get(p, "freeHz") == 3.5f && get(p, "curve") == 7
          && get(p, "slope") == -40 && get(p, "invert") == 1 && get(p, "depth") == 62.5f && get(p, "shift") == 12.5f && get(p, "loop") == 1
          && get(p, "thresh") == -30 && get(p, "band") == 2 && get(p, "split") == 250 && get(p, "output") == -3.5f && get(p, "color") == 5
          && get(p, "tone") == 75 && get(p, "stereo") == 1 && get(p, "soothe") == 0 && get(p, "lookahead") == 0); // band highs with pinch: just pinch
    CHECK(p.presetName() == "user 07" && p.abSlot() == 1 && p.editorWidth == 1536);
    // Slot A is 0.1.0's preset 03 (band lows, level): it comes back as the lows mode, soothe off.
    set(p, "lookahead", 1), p.selectAb(0);
    CHECK(p.getCurrentProgram() == 3 && get(p, "rate") == 8 && get(p, "band") == 1 && get(p, "color") == 1 && get(p, "soothe") == 0 && get(p, "lookahead") == 0);
    p.selectAb(1), CHECK(get(p, "color") == 5 && get(p, "trigger") == 2);
    // Ducking only the lows / highs (band + level) becomes that mode; other modes on a band, and a full band, keep theirs.
    for (auto [band, colour, now] : { std::tuple { 1, 0, 1 }, { 2, 0, 2 }, { 0, 0, 0 }, { 1, 3, 3 }, { 0, 6, 6 } })
    {
        const auto xml = juce::String(R"(<GULP stateVersion="1"><PARAM id="band" value="B"/><PARAM id="color" value="C"/><PARAM id="soothe" value="50"/></GULP>)");
        set(p, "soothe", 30), load(p, blob(xml.replace("B", juce::String(band)).replace("C", juce::String(colour))));
        CHECK(get(p, "color") == (float) now && get(p, "band") == (float) band && get(p, "soothe") == 0);
    }
    load(p, blob(R"(<GULP stateVersion="2"><PARAM id="band" value="1"/><PARAM id="color" value="0"/></GULP>)")); // v2: band is inert
    CHECK(get(p, "color") == 0 && get(p, "soothe") == 0);
    // The checked-in v2 blob: everything as saved (rate 4/1 too, which the editor no longer steps to), lookahead off,
    // also in both slots (18 values each), and it sounds as a fresh 0.2.0-style setup without lookahead.
    Proc v2, same;
    set(v2, "lookahead", 1), load(v2, v2Fixture());
    CHECK(get(v2, "trigger") == 2 && get(v2, "rate") == 15 && get(v2, "curve") == 3 && get(v2, "slope") == 12.5f && get(v2, "depth") == 80
          && get(v2, "loop") == 1 && get(v2, "thresh") == -30 && get(v2, "split") == 150 && get(v2, "color") == 1 && get(v2, "soothe") == 25
          && get(v2, "lookahead") == 0 && v2.presetName() == "user 03" && v2.getLatencySamples() == 0);
    set(v2, "lookahead", 1), v2.selectAb(0); // slot A (saved current: B)
    CHECK(get(v2, "lookahead") == 0 && get(v2, "trigger") == 2 && get(v2, "soothe") == 25 && get(v2, "color") == 1 && get(v2, "rate") == 8 && v2.getCurrentProgram() == 5);
    load(v2, v2Fixture(0)), set(v2, "lookahead", 1), v2.selectAb(1); // slot B
    CHECK(get(v2, "lookahead") == 0 && get(v2, "loop") == 1 && get(v2, "trigger") == 1 && get(v2, "rate") == 15 && v2.getCurrentProgram() == 2);
    load(v2, v2Fixture());
    for (int i = 0; i < ids.size() - 1; ++i) set(same, ids[i].toRawUTF8(), get(v2, ids[i]));
    prepare(v2, 48000, 512), prepare(same, 48000, 512);
    CHECK(run(v2, 48000, 512) == run(same, 48000, 512) && v2.getLatencySamples() == 0);
    // 0.2.1 saves round-trip lookahead on, in the session and in the A/B slots, and report its latency on load.
    Proc on, back;
    set(on, { { "trigger", 2 }, { "lookahead", 1 } }), on.selectAb(1), set(on, "lookahead", 0), on.selectAb(0);
    load(back, save(on)), prepare(back, 48000, 512);
    CHECK(get(back, "lookahead") == 1 && back.getLatencySamples() == 240 && save(back) == save(on));
    back.selectAb(1), CHECK(get(back, "lookahead") == 0 && back.getLatencySamples() == 0);
    back.selectAb(0), CHECK(get(back, "lookahead") == 1 && back.getLatencySamples() == 240);
}

// Lookahead (audio trigger only): the main input is delayed by 5 ms, reported as latency, while the sidechain is read
// undelayed, so the duck is already down when the (delayed) transient plays. Off, or on another trigger: no latency and
// not a bit different. The latency follows the parameters on the message thread, also when set from another thread.
template <typename Done> static void pumpUntil(Done done);
static void testLookahead()
{
    for (auto [fs, samples] : { std::pair { 44100.0, 220 }, { 48000.0, 240 }, { 96000.0, 480 } })
    {
        Proc p;
        prepare(p, fs, 512), CHECK(p.getLatencySamples() == 0);
        set(p, "lookahead", 1), CHECK(p.getLatencySamples() == 0); // host sync: nothing to look ahead of
        set(p, "trigger", 2), CHECK(p.getLatencySamples() == samples);
        set(p, "trigger", 1), CHECK(p.getLatencySamples() == 0);
        set(p, "trigger", 2), set(p, "lookahead", 0), CHECK(p.getLatencySamples() == 0);
        set(p, "lookahead", 1), prepare(p, fs, 64), CHECK(p.getLatencySamples() == samples);
        std::thread([&] { set(p, "lookahead", 0); }).join(); // as automation on the audio thread: the message thread reports it
        pumpUntil([&] { return p.getLatencySamples() == 0; }), CHECK(p.getLatencySamples() == 0);
    }
    // A step from 0.25 to 0.5 at T on the input with a kick at T on the sidechain (classic, 1/4 at 120 bpm, depth 100).
    // gain = output / the input it played. Off, the duck starts just after the step, which passes at full level. On,
    // the duck starts at the same sample (detection is undelayed), 240 samples before the step plays, which it ducks.
    constexpr int T = 4800, L = 240;
    int onset[2] {};
    double atStep[2] {};
    for (int on : { 0, 1 })
    {
        Proc p;
        set(p, { { "trigger", 2 }, { "lookahead", (float) on }, { "curve", 0 } });
        prepare(p, 48000, 512);
        Run r;
        r.in = [](int i, int) { return i < T ? 0.25f : 0.5f; }, r.sc = [](int i) { return i < T ? 0.0f : kick(i - T); };
        const auto y = run(p, 9600, 0, r);
        const auto gain = [&](int i) { return y[(size_t) i] / (i - on * L < T ? 0.25 : 0.5); };
        onset[on] = L;
        while (onset[on] < 9600 && gain(onset[on]) > 0.9999) ++onset[on];
        atStep[on] = gain(T + on * L);
    }
    CHECK(onset[0] == onset[1] && onset[0] >= T && onset[0] <= T + 10); // detection: a few samples into the kick
    CHECK(onset[1] < T + L && T + L - onset[1] >= L - 10);                // on: the duck leads the output's transient
    CHECK(atStep[0] > 0.95 && atStep[1] < 0.2);                           // off the step passes, on it is ducked
    // Off, or on with another trigger: bit-identical to a plug-in without it (the goldens are rendered with it off).
    for (int trigger : { 0, 1 })
    {
        Proc off, on;
        set(off, "trigger", (float) trigger), set(on, { { "trigger", (float) trigger }, { "lookahead", 1 } });
        prepare(off, 48000, 512), prepare(on, 48000, 512);
        Host h1, h2;
        Run a, b;
        a.host = &h1, b.host = &h2, a.notes.push_back(300), a.notes.push_back(20000), b.notes = a.notes;
        CHECK(run(off, 48000, 0, a) == run(on, 48000, 0, b) && on.getLatencySamples() == 0);
    }
    // Switching it while playing crossfades (10 ms): no click on the bass, depth 0.
    Proc p;
    set(p, { { "trigger", 2 }, { "depth", 0 } });
    prepare(p, 48000, 512);
    Run r;
    r.perBlock = [&](int start) { set(p, "lookahead", (float) (start / 4096 % 2)); };
    CHECK(maxStep(run(p, 48000, 512, r)) < 0.01);
}

// Presets: names, prev/next through factory then user files, save, A/B (a fresh B starts as a copy of A).
static void testPresets()
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("gulp-presets", "", false);
    Proc p(dir);
    juce::StringArray names;
    for (int k = 0; k < 10; ++k) names.add(p.getProgramName(k));
    CHECK(names[0] == "init" && names[3] == juce::String::fromUTF8("house \xc2\xb7 classic pump") && p.getProgramName(10).isEmpty());
    p.setCurrentProgram(3);
    CHECK(get(p, "color") == 1 && get(p, "split") == 120 && get(p, "curve") == 0 && p.presetName() == names[3]);
    p.stepPreset(-1), CHECK(p.getCurrentProgram() == 2);
    set(p, "depth", 33);
    const auto file = p.saveUserPreset();
    CHECK(file.existsAsFile() && p.preset == -1 && p.presetName() == "user 01");
    p.setCurrentProgram(9), p.stepPreset(1), CHECK(p.presetName() == "user 01" && get(p, "depth") == 33 && get(p, "curve") == 7);
    p.stepPreset(1), CHECK(p.getCurrentProgram() == 0 && p.preset == 0);
    // A 0.1.0 user file (no stateVersion) is migrated like a session: band lows + level -> the lows mode.
    dir.getChildFile("user 02.xml").replaceWithText(R"(<GULP><PARAM id="band" value="2"/><PARAM id="color" value="0"/></GULP>)");
    p.setCurrentProgram(9), p.stepPreset(2), CHECK(p.presetName() == "user 02" && get(p, "color") == 2);
    CHECK(juce::parseXML(file)->getIntAttribute("stateVersion") == 2);
    p.setCurrentProgram(5);
    p.selectAb(1), CHECK(p.abSlot() == 1 && get(p, "trigger") == 2); // B starts as a copy
    p.setCurrentProgram(7), p.selectAb(0), CHECK(get(p, "trigger") == 2 && p.getCurrentProgram() == 5);
    p.selectAb(1), CHECK(get(p, "color") == 3 && p.getCurrentProgram() == 7);
    dir.deleteRecursively();
}

// Sync: the duck follows the host's ppq at every sample at 44.1 / 48 / 96 kHz, through a tempo change, a loop jump
// and a shift. The slam curve at depth 100 on DC is silent for phase 0 .. 0.3 and at full level from 0.36 on.
static void testSync()
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (float shift : { 0.0f, 25.0f })
        {
            Proc p;
            set(p, { { "curve", 5 }, { "shift", shift } });
            prepare(p, fs, 512);
            Host h;
            h.ppq = 0.37;
            std::vector<double> ppq;
            std::vector<int> jumps { 0 }; // the start, then a tempo change and a loop jump: 3 ms to settle each
            Run r = dc(0.5f);
            r.host = &h, r.ppqs = &ppq;
            const int n = (int) (8 * fs);
            r.perBlock = [&](int start) {
                if (start > n / 3 && h.bpm > 100) h.bpm = 90, jumps.push_back(start);       // tempo change
                if (start > 2 * n / 3 && h.ppq > 9) h.ppq = 2.25, jumps.push_back(start);   // loop jump
            };
            const auto y = run(p, n, 0, r);
            int checked = 0;
            for (size_t i = 0; i < y.size(); ++i)
            {
                double ph = ppq[i] - shift / 100; // one cycle = one quarter
                ph -= std::floor(ph);
                bool settled = true;
                for (int j : jumps) settled = settled && !((int) i >= j && (int) i < j + (int) (0.003 * fs));
                if (settled && ph > 0.02 && ph < 0.28) CHECK(y[i] < 0.005f), ++checked;
                if (settled && ph > 0.4 && ph < 0.98) CHECK(y[i] > 0.4995f), ++checked;
            }
            CHECK(checked > n / 2 && jumps.size() == 3);
        }
    // No host position: freewheels at 120 bpm (one quarter = 0.5 s).
    Proc p;
    set(p, "curve", 0);
    prepare(p, 48000, 512);
    Host h;
    h.known = false;
    Run r = dc(1);
    r.host = &h;
    const auto y = run(p, 96000, 0, r);
    for (int i = 50000; i < 72000; i += 97) CHECK(std::abs(y[(size_t) i] - y[(size_t) i + 24000]) < 1e-3f);
    CHECK(*std::min_element(y.begin() + 50000, y.end()) < 0.01f);
}

// Host edge cases: a non-finite or non-positive tempo freewheels at 120 bpm; a non-finite ppq counts as no position
// (freewheel at the reported tempo); huge finite ppqs stay in range. Output and phase finite, no invalid conversion
// (UBSan's float-cast-overflow). period: one quarter in samples to check, 0 = only finite.
static void testHostEdges()
{
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    struct Case { double bpm, ppq; bool playing; int period; };
    for (const auto& c : { Case { nan, 0, false, 24000 }, Case { inf, 0, false, 24000 }, Case { -inf, 0, false, 24000 }, Case { 0, 0, false, 24000 },
                           Case { -5, 0, false, 24000 }, Case { 96, nan, true, 30000 }, Case { 96, inf, true, 30000 }, Case { 96, -inf, true, 30000 },
                           Case { nan, nan, true, 24000 }, Case { nan, 3, true, 0 }, Case { 124, 1e300, true, 0 }, Case { 124, -1e300, true, 0 } })
        for (int trigger : { 0, 1 })
        {
            Proc p;
            set(p, { { "curve", 0 }, { "trigger", (float) trigger }, { "loop", 1 }, { "shift", 20 } });
            prepare(p, 48000, 512);
            Host h;
            Run r = dc(1);
            r.host = &h, r.notes.push_back(10), r.perBlock = [&](int) { h.bpm = c.bpm, h.ppq = c.ppq, h.playing = c.playing; }; // held
            const auto y = run(p, 110000, 0, r);
            const float phase = p.feed.phase.load();
            CHECK(std::all_of(y.begin(), y.end(), [](float v) { return std::isfinite(v); }) && phase >= 0 && phase <= 1);
            if (c.period > 0 && trigger == 0)
            {
                for (int i = 50000; i < 72000; i += 97) CHECK(std::abs(y[(size_t) i] - y[(size_t) (i + c.period)]) < 1e-3f);
                CHECK(*std::min_element(y.begin() + 50000, y.end()) < 0.01f);
            }
        }
}

// Lifecycle: stopped with the ppq held freewheels at the reported tempo; no playhead at all = 120 bpm; playing again
// snaps to the ppq; a state restored after prepareToPlay renders as a fresh instance; re-preparing at 44.1 / 48 / 96 kHz
// (with delayed triggers pending) resets; zero-length, changing and oversized blocks; offline renders the same.
static void testLifecycle()
{
    for (bool head : { true, false })
    {
        Proc p;
        set(p, "curve", 0);
        prepare(p, 48000, 512);
        Host h;
        h.bpm = 90, h.playing = false;
        Run r = dc(1);
        r.host = head ? &h : nullptr, r.perBlock = [&](int) { h.ppq = 7.3; };
        const auto y = run(p, 120000, 0, r);
        const int period = head ? 32000 : 24000;
        for (int i = 50000; i < 80000; i += 97) CHECK(std::abs(y[(size_t) i] - y[(size_t) (i + period)]) < 1e-3f);
        CHECK(*std::min_element(y.begin() + 50000, y.end()) < 0.01f);
    }
    {
        Proc p;
        set(p, "curve", 5);
        prepare(p, 48000, 512);
        Host h;
        std::vector<double> ppq;
        const int n = 6 * 48000;
        int resumed = -1, checked = 0;
        Run r = dc(0.5f);
        r.host = &h, r.ppqs = &ppq, r.perBlock = [&](int start) {
            h.playing = start >= n / 2;
            if (!h.playing) h.ppq = 4.6;
            else if (resumed < 0) resumed = start;
        };
        const auto y = run(p, n, 0, r);
        for (int i = resumed + 144; i < n; ++i)
        {
            const double ph = ppq[(size_t) i] - std::floor(ppq[(size_t) i]);
            if (ph > 0.02 && ph < 0.28) CHECK(y[(size_t) i] < 0.005f), ++checked;
            if (ph > 0.4 && ph < 0.98) CHECK(y[(size_t) i] > 0.4995f), ++checked;
        }
        CHECK(resumed > 0 && checked > n / 3);
    }
    const auto render = [](Proc& p, int samples, const std::function<void()>& between = {}) {
        Host h;
        Run r;
        r.host = &h, r.notes = std::vector<int> { 300, 9000, 20000 };
        if (between) r.perBlock = [&](int) { between(); };
        return run(p, samples, 0, r);
    };
    juce::Random rnd(17);
    for (int k = 0; k < 6; ++k)
    {
        Proc a, b, c;
        randomise(a, rnd), set(a, { { "trigger", (float) (k % 3) }, { "shift", 30 } });
        const auto state = save(a);
        prepare(b, 48000, 256), load(b, state); // restored after prepareToPlay
        load(c, state), prepare(c, 48000, 256);
        CHECK(render(b, 30000) == render(c, 30000));
    }
    {
        const Settings s { { "trigger", 1 }, { "shift", 40 }, { "loop", 1 }, { "color", 3 }, { "soothe", 30 } };
        Proc p;
        set(p, s);
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            prepare(p, 48000, 512), render(p, 16000); // notes at 9000 and 20000 still pending
            prepare(p, fs, 512);
            Proc q;
            set(q, s), prepare(q, fs, 512);
            CHECK(render(p, 30000) == render(q, 30000));
        }
    }
    {
        const Settings s { { "trigger", 2 }, { "shift", 10 }, { "color", 2 }, { "stereo", 1 }, { "soothe", 100 } };
        Proc a, b, c;
        set(a, s), set(b, s), set(c, s);
        c.setNonRealtime(true);
        prepare(a, 48000, 64), prepare(b, 48000, 64), prepare(c, 48000, 64); // blocks of up to 2048 exceed the prepared 64
        juce::AudioBuffer<float> empty(4, 0);
        juce::MidiBuffer none;
        const auto zero = [&] { onAudioThread = true, b.processBlock(empty, none), onAudioThread = false; };
        const auto y = render(a, 48000);
        CHECK(y == render(b, 48000, zero) && y == render(c, 48000));
        CHECK(std::all_of(y.begin(), y.end(), [](float v) { return std::isfinite(v); }));
    }
    CHECK(audioAllocations.load() == 0);
}

// MIDI and audio triggers, loop, shift as a trigger delay.
static void testTriggers()
{
    const double inc = 124.0 / 60 / 0.25 / 48000; // 1/16 at 124 bpm, cycles per sample
    const auto firstChange = [](const std::vector<float>& y, int from) { while (from < (int) y.size() && y[(size_t) from] == 0.5f) ++from; return from; };
    for (float loop : { 0.0f, 1.0f })
        for (float shift : { 0.0f, 25.0f, -25.0f })
        {
            Proc p;
            set(p, { { "trigger", 1 }, { "curve", 5 }, { "rate", 2 }, { "loop", loop }, { "shift", shift } });
            prepare(p, 48000, 512);
            Host h;
            Run r = dc(0.5f);
            r.host = &h, r.notes = { 1000, 30001 };
            const auto y = run(p, 48000, 512, r);
            const int delay = (int) std::lround(std::max(0.0f, shift) / 100 / inc);
            CHECK(firstChange(y, 0) == 1000 + delay); // the duck starts at the note's sample (plus the delay)
            const int idle = 1000 + delay + (int) (1 / inc) + 3000; // one cycle later, plus the slew settling
            const bool held = std::all_of(y.begin() + idle, y.begin() + 30001, [](float v) { return v == 0.5f; });
            CHECK(held == (loop < 0.5f)); // loop off holds at full level; loop on keeps ducking
            CHECK(loop < 0.5f ? firstChange(y, idle) == 30001 + delay : *std::min_element(y.begin() + idle, y.begin() + 30001) < 0.01f);
        }
    // Audio: one trigger per kick at its onset (+-1 ms), none on the tail, which wobbles +-1 dB around the threshold
    // after the kick until 0.42 s. Mono and stereo sidechains; without one nothing triggers.
    for (int sc : { 0, 1, 2 })
    {
        Proc p;
        set(p, { { "trigger", 2 }, { "curve", 5 }, { "rate", 2 }, { "thresh", -24 } });
        prepare(p, 48000, 480, 2, sc);
        Host h;
        Run r = dc(0.5f);
        r.host = &h, r.sc = [](int i) {
            const double t = std::fmod(i / 48000.0, 0.5), thr = std::pow(10.0, -24 / 20.0);
            return t < 0.3 ? kick(i) : t < 0.42 ? (float) (thr * std::pow(10.0, std::sin(2 * pi * 7 * t) / 20) * std::sin(2 * pi * 40 * t)) : 0.0f;
        };
        const auto y = run(p, 4 * 48000, 0, r);
        std::vector<int> edges;
        for (int i = 1; i < (int) y.size(); ++i)
            if (y[(size_t) i - 1] == 0.5f && y[(size_t) i] < 0.5f) edges.push_back(i);
        CHECK(edges.size() == (sc == 0 ? 0u : 8u));
        for (size_t k = 0; k < edges.size(); ++k) CHECK(std::abs(edges[k] - 24000 * (int) k) <= 48);
    }
}

// One shots rest at full level, exactly: before the first trigger and once the cycle has run (loop off), for every
// curve, both inverts and the slope extremes; MIDI, and audio with and without a sidechain. The trigger at 6000 ducks.
static void testIdle()
{
    for (auto [trigger, sc] : { std::pair { 1, 2 }, { 2, 2 }, { 2, 0 } })
        for (int curve = 0; curve < 12; ++curve)
            for (float invert : { 0.0f, 1.0f })
                for (float slope : { -100.0f, 100.0f })
                {
                    Proc p;
                    set(p, { { "trigger", (float) trigger }, { "curve", (float) curve }, { "invert", invert }, { "slope", slope }, { "rate", 0 } });
                    prepare(p, 48000, 512, 2, sc);
                    Run r = dc(1); // no playhead: 120 bpm, 1/32 = 3000 samples
                    r.notes.push_back(6000), r.sc = [](int i) { return i >= 6000 && i < 6048 ? 0.9f : 0.0f; };
                    const auto y = run(p, 12000, 0, r);
                    const bool fired = sc > 0;
                    CHECK(std::all_of(y.begin(), y.begin() + 6000, [](float v) { return v == 1; }));
                    CHECK(std::all_of(y.begin() + 9600, y.end(), [](float v) { return v == 1; })); // + 600 samples of slew
                    CHECK((*std::min_element(y.begin() + 6000, y.begin() + 9000) < 0.9f) == fired);
                    CHECK(maxStep(y) < 0.081);
                }
}

// Trigger delay longer than the hits' spacing: free 2 Hz (24000 samples), shift +50 % = 12000 samples, a hit every
// 6000: each fires at its own due sample (suck: full duck at its start), across random and fixed block boundaries.
static void testDelayedTriggers()
{
    for (int trigger : { 1, 2 })
        for (int block : { 0, 512 })
        {
            Proc p;
            set(p, { { "trigger", (float) trigger }, { "tempoSync", 0 }, { "freeHz", 2 }, { "shift", 50 }, { "curve", 9 }, { "slope", -100 }, { "thresh", -6 } });
            prepare(p, 48000, 512);
            std::vector<int> hits;
            for (int t = 1000; t <= 55000; t += 6000) hits.push_back(t);
            Run r = dc(0.5f);
            r.notes = hits, r.sc = [](int i) { return i >= 1000 && i < 55048 && (i - 1000) % 6000 < 48 ? 0.9f : 0.0f; };
            const auto y = run(p, 80000, block, r);
            int first = 0;
            while (first < (int) y.size() && y[(size_t) first] == 0.5f) ++first;
            CHECK(first == 13000);
            for (int t : hits) CHECK(y[(size_t) t + 11999] > 0.1f && y[(size_t) t + 12080] < 0.01f);
        }
}

// Clicks: the per-sample gain step stays under the 0.25 ms slew's bound (0.08 at 48 kHz) on a retrigger mid-duck and
// curve swaps; a retrigger never goes up before it goes down; band and colour switches are crossfaded.
static void testClicks()
{
    {
        Proc p;
        set(p, { { "trigger", 1 }, { "loop", 1 } });
        prepare(p, 48000, 512);
        Host h;
        Run r = dc(1);
        r.host = &h, r.notes = { 100, 7100, 14100, 21100, 28100, 31000 };
        const auto y = run(p, 48000, 0, r);
        CHECK(maxStep(y) < 0.081);
        for (int n : r.notes)
            for (int i = n; i < n + 232; ++i) CHECK(y[(size_t) i + 1] <= y[(size_t) i]);
    }
    {
        Proc p;
        prepare(p, 48000, 512);
        Host h;
        juce::Random rnd(5);
        Run r = dc(1);
        r.host = &h, r.perBlock = [&](int) { set(p, "curve", (float) rnd.nextInt(12)), set(p, "invert", (float) rnd.nextInt(2)); };
        CHECK(maxStep(run(p, 96000, 512, r)) < 0.081);
    }
    for (bool sine : { false, true }) // every mode on DC; vol / lows / highs (the crossover in and out) on a 100 Hz sine
    {
        Proc p;
        prepare(p, 48000, 500);
        Host h;
        int k = 0;
        Run r = dc(0.5f);
        if (sine) r.in = [](int i, int) { return (float) (0.5 * std::sin(2 * pi * 100 * i / 48000)); };
        r.host = &h, r.perBlock = [&](int start) { if (start % 2000 == 0) set(p, "color", (float) (sine ? ++k % 3 : (k += 3) % 8)); };
        const double step = maxStep(run(p, 96000, 500, r), 4000);
        std::cout << "mode switch: max step " << step << "\n";
        CHECK(step < 0.02);
    }
}

static juce::Rectangle<int> cellArea(int x, int y, int w, int h);
static juce::String pixels(const juce::Image& image);

// Soothe: the same rounded table in the DSP and the graph. On DC the gain's largest step per sample shrinks as soothe
// rises (a stronger soothe also takes more off the bottom of a short duck), in sync and on MIDI hits (whose eased attack
// starts at the hit, never stepping up first); at 0 the table is the
// curve itself, so 0.1.0's sound (goldens below) is untouched. Automating it never clicks.
static void testSoothe()
{
    gulp::Table plain, soft;
    gulp::shape(plain, 0, 0, false, 0), gulp::shape(soft, 0, 0, false, 0.4);
    for (size_t j = 0; j < plain.size(); ++j) CHECK(plain[j] == gulp::duck(0, j / 1024.0, 1));
    const auto steepest = [](const gulp::Table& t) { double m = 0; for (size_t j = 1; j < t.size(); ++j) m = juce::jmax(m, std::abs(t[j] - t[j - 1])); return m; };
    CHECK(steepest(soft) < 0.2 * steepest(plain) && soft[1024] == soft[0] && *std::max_element(soft.begin(), soft.end()) > 0.75);
    for (int trigger : { 0, 1 })
    {
        double last = 1;
        for (float soothe : { 0.0f, 20.0f, 60.0f, 100.0f })
        {
            Proc p;
            set(p, { { "trigger", (float) trigger }, { "soothe", soothe }, { "rate", 5 } });
            prepare(p, 48000, 512);
            Host h;
            Run r = dc(1);
            r.host = &h, r.notes = { 100, 12100, 24100, 36100 };
            const auto y = run(p, 48000, 0, r);
            const double step = maxStep(y, 2000); // after the first beat (a start inside a duck steps in, as without soothe)
            CHECK(step < (soothe > 0 ? 0.75 * last : 0.081) && *std::min_element(y.begin() + 2000, y.end()) < (soothe > 0 ? 0.6f : 0.02f));
            if (trigger == 1)
            {
                CHECK(std::all_of(y.begin(), y.begin() + 100, [](float v) { return v == 1; }));
                for (int n : r.notes) for (int i = n; i < n + 100; ++i) CHECK(y[(size_t) i + 1] <= y[(size_t) i]);
            }
            last = step;
        }
    }
    // One shots must ease both boundaries, including inverted curves and releases that cross the cycle end.
    // A DC input exposes gain discontinuities directly; 0.5 % per sample is a generous ceiling at this rate.
    for (int trigger : { 1, 2 }) for (int curve = 0; curve < gulp::numCurves; ++curve)
        for (float invert : { 0.0f, 1.0f }) for (float slope : { -100.0f, 100.0f })
        {
            Proc edge;
            set(edge, { { "trigger", (float) trigger }, { "curve", (float) curve }, { "slope", slope }, { "invert", invert }, { "soothe", 100 }, { "rate", 5 } });
            prepare(edge, 48000, 512, 2, 1);
            Run hit = dc(1);
            hit.notes = { 100 };
            hit.sc = [](int i) { return i == 100 ? 1.0f : 0.0f; };
            const auto y = run(edge, 15000, 0, hit);
            if (maxStep(y) >= 0.005) std::cerr << "one-shot edge: trigger " << trigger << " curve " << curve << " invert " << invert << " slope " << slope << " step " << maxStep(y) << "\n";
            CHECK(maxStep(y) < 0.005);
        }
    for (int trigger : { 0, 1, 2 })
    {
        Proc p;
        set(p, "trigger", (float) trigger);
        prepare(p, 48000, 256);
        Host h;
        juce::Random rnd(4);
        Run r = dc(1);
        r.host = &h, r.notes = { 100 }, r.sc = [](int i) { return i == 100 ? 1.0f : 0.0f; };
        r.perBlock = [&](int start) { set(p, "soothe", start % 1024 == 0 ? 0 : rnd.nextFloat() * 100); };
        CHECK(maxStep(run(p, 48000, 256, r), 2000) < 0.005);
    }
}

// The host's transport: stopped, the graph's telemetry (cycle, duck, its held peak, the bins) holds still while the audio
// still runs and the meters move; starting again zeroes the held peak. No transport at all (standalone): it all runs.
static void testTransport()
{
    Proc p;
    prepare(p, 48000, 512);
    Host h;
    Run r = dc(0.5f);
    r.host = &h;
    run(p, 48000, 512, r);
    CHECK(p.feed.live && p.feed.held > 0.9f);
    const auto frozen = [&] {
        std::vector<float> v { p.feed.phase, p.feed.duck, p.feed.held };
        for (size_t k = 0; k < gulp::Feed::bins; ++k) v.push_back(p.feed.hi[k]), v.push_back(p.feed.lo[k]);
        return v;
    };
    h.playing = false;
    run(p, 512, 512, r);
    const auto still = frozen();
    r.in = [](int i, int) { return bass(i); }, r.offset = 777;
    set(p, "depth", 0); // the audio still follows it (below): a = 0 by the time the transport starts
    run(p, 30000, 0, r);
    CHECK(!p.feed.live && frozen() == still && p.feed.outLevel > 0.1f);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    const auto image = [&] { dynamic_cast<juce::Timer&>(*e).timerCallback(); return pixels(e->createComponentSnapshot(cellArea(25, 3, 70, 17), true, 1.0f)); }; // the graph
    const auto shown = image();
    run(p, 9000, 0, r), CHECK(image() == shown);
    h.playing = true, r.in = [](int, int) { return 0.5f; };
    run(p, 256, 256, r), CHECK(p.feed.live && p.feed.held == 0 && image() != shown);
    Proc q;
    prepare(q, 48000, 512);
    Run none = dc(1); // no playhead
    run(q, 1000, 512, none);
    const float phase = q.feed.phase;
    run(q, 7000, 512, none);
    CHECK(q.feed.live && q.feed.phase != phase);
}

// A MIDI note at sample 0 starts slam stretched to 4.8 s of a 4/1 cycle: duck 1 for the whole of a short render.
static const Settings plateau { { "trigger", 1 }, { "curve", 5 }, { "slope", 100 }, { "rate", 15 } };
static Settings operator+(Settings a, const Settings& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

// Split: depth 0 vol is the dry signal bit for bit; lows at 100 % leaves 2 kHz alone and ducks 50 Hz; the LR4
// lows + highs sum is flat.
static void testBands()
{
    {
        juce::Random r(11);
        for (int n = 0; n < 6; ++n)
        {
            Proc p;
            randomise(p, r);
            set(p, { { "depth", 0 }, { "color", 0 }, { "output", 0 }, { "lookahead", 0 } });
            prepare(p, 48000, 512);
            Host h;
            std::vector<float> right;
            Run run_;
            run_.in = [](int i, int c) { return noise(i * 2 + c) * 0.9f; }, run_.host = &h, run_.notes = { 500, 9000 }, run_.right = &right;
            const auto y = run(p, 24000, 0, run_);
            bool exact = true;
            for (int i = 0; i < 24000; ++i) exact = exact && y[(size_t) i] == noise(i * 2) * 0.9f && right[(size_t) i] == noise(i * 2 + 1) * 0.9f;
            CHECK(exact);
        }
    }
    const auto level = [](Settings s, double hz) {
        Proc p;
        set(p, s);
        prepare(p, 48000, 512);
        Host h;
        Run r;
        r.in = [hz](int i, int) { return (float) (0.5 * std::sin(2 * pi * hz * i / 48000)); }, r.host = &h, r.notes.push_back(0);
        return rmsDb(run(p, 96000, 512, r), 24000) - 10 * std::log10(0.125);
    };
    // Legacy automation must remain sonically inert in every mode, even when the duck is active.
    for (int mode = 0; mode < gulp::numColors; ++mode)
    {
        Proc a, b;
        set(a, "color", (float) mode), set(b, { { "color", (float) mode }, { "band", 2 }, { "tone", 100 } });
        prepare(a, 48000, 512), prepare(b, 48000, 512);
        CHECK(run(a, 12000, 512) == run(b, 12000, 512));
    }
    const Settings pumping { { "color", 1 } }, lows { { "color", 1 } }, highs { { "color", 2 } };
    CHECK(std::abs(level(pumping, 2000)) < 0.05 && level(pumping, 50) < -1);
    CHECK(std::abs(level(lows + plateau, 2000)) < 0.05 && level(lows + plateau, 50) < -30); // duck 1 throughout (as in testColours)
    CHECK(std::abs(level(highs + plateau, 20)) < 0.05 && level(highs + plateau, 5000) < -30); // LR4 at a sixth of the split: -0.007 dB
    for (double hz : { 30.0, 120.0, 500.0, 4000.0 })
        CHECK(std::abs(level({ { "color", 1 }, { "depth", 0 } }, hz)) < 0.01);
}

// Modes: near dry at a = 0 (exact for vol, hollow, grit; lows and highs are an allpass); at a = 1 (the slam plateau: duck 1
// throughout) each does what it says. Mid / side leave the other component bit-exact.
static void testColours()
{
    const auto render = [](int colour, float depth, std::function<float(int)> in, Settings extra = {}) {
        Proc p;
        set(p, plateau + Settings { { "color", (float) colour }, { "depth", depth } }), set(p, extra);
        prepare(p, 48000, 512);
        Run r;
        r.in = [in](int i, int) { return in(i); }, r.notes.push_back(0);
        return run(p, 72000, 512, r);
    };
    const auto sine = [](double hz) { return [hz](int i) { return (float) (0.5 * std::sin(2 * pi * hz * i / 48000)); }; };
    const auto db = [&](int colour, double hz) { return rmsDb(render(colour, 100, sine(hz)), 48000) - 10 * std::log10(0.125); };
    for (int m = 0; m < 8; ++m)
    {
        const auto y = render(m, 0, sine(1000));
        bool exact = true;
        for (int i = 0; i < 72000; ++i) exact = exact && y[(size_t) i] == sine(1000)(i);
        CHECK(m >= 1 && m <= 5 ? std::abs(rmsDb(y, 24000) - 10 * std::log10(0.125)) < 0.1 : exact);
    }
    const auto silent = render(0, 100, sine(1000));
    CHECK(std::all_of(silent.begin() + 24000, silent.end(), [](float v) { return v == 0; }));
    CHECK(db(1, 50) < -3 && std::abs(db(1, 5000)) < 1);
    CHECK(db(2, 8000) < -3 && std::abs(db(2, 50)) < 1);
    CHECK(db(3, 8000) < -20 && db(4, 60) < -20);
    CHECK(db(5, 1000) - db(5, 60) > 12 && db(5, 1000) - db(5, 10000) > 12 && db(5, 1000) > -10);
    CHECK(db(6, 1000) < -20);
    const auto alias = [](const std::vector<float>& y, double hz) { // DFT magnitude over the last second
        double re = 0, im = 0;
        for (int i = 24000; i < 72000; ++i) re += y[(size_t) i] * std::cos(2 * pi * hz * i / 48000), im += y[(size_t) i] * std::sin(2 * pi * hz * i / 48000);
        return 20 * std::log10(2 * std::hypot(re, im) / 48000 + 1e-12);
    };
    CHECK(alias(render(7, 100, sine(300)), 1300) > -40 && alias(render(7, 0, sine(300)), 1300) < -100);
    for (int stereo : { 1, 2 }) // pure side through mid, pure mid through side
        for (int m = 0; m < 8; ++m)
        {
            Proc p;
            set(p, { { "color", (float) m }, { "stereo", (float) stereo }, { "soothe", (float) (10 * m) } });
            prepare(p, 48000, 512);
            Host h;
            std::vector<float> right;
            Run r;
            r.in = [stereo](int i, int c) { return bass(i) * (c == 1 && stereo == 1 ? -1.0f : 1.0f); }, r.host = &h, r.right = &right;
            const auto y = run(p, 48000, 0, r);
            bool exact = true;
            for (int i = 0; i < 48000; ++i) exact = exact && y[(size_t) i] == bass(i) && right[(size_t) i] == (stereo == 1 ? -bass(i) : bass(i));
            CHECK(exact);
        }
}

// No allocation in processBlock; no NaN, infinity or denormal on silence and full-scale noise in any setting.
static void testRealtime()
{
    CHECK(audioAllocations.exchange(0) == 0);
    auto counted = [](auto allocate) { onAudioThread = true; allocate(); onAudioThread = false; return audioAllocations.exchange(0) > 0; };
    CHECK(counted([] { void* q = ::operator new(sizeof(int)); escape(q); ::operator delete(q); }));
#ifdef GULP_COUNTS_MALLOC
    CHECK(counted([] { void* q = std::malloc(64); escape(q); std::free(q); }));
#endif
    juce::Random r(9);
    for (int layout = 0; layout < 3; ++layout)
    {
        Proc p;
        prepare(p, 48000, 512, layout == 2 ? 1 : 2, layout);
        Host h;
        Run silence, loud;
        silence.in = [](int, int) { return 0.0f; }, silence.sc = [](int) { return 0.0f; }, silence.host = &h;
        loud.in = [](int i, int c) { return noise(3 * i + c); }, loud.sc = [](int i) { return noise(7 * i); }, loud.host = &h;
        loud.notes = silence.notes = { 10, 5000, 17000, 30000 };
        for (int n = 0; n < 30; ++n)
        {
            randomise(p, r);
            loud.perBlock = silence.perBlock = [&](int) { if (r.nextInt(4) == 0) randomise(p, r); };
            for (float v : run(p, 4096, 0, silence)) CHECK(std::isfinite(v) && std::fpclassify(v) != FP_SUBNORMAL && (n > 0 || v == 0));
            for (float v : run(p, 4096, 0, loud)) CHECK(std::isfinite(v) && std::abs(v) < 20 && std::fpclassify(v) != FP_SUBNORMAL);
        }
    }
    CHECK(audioAllocations.load() == 0);
}

// ================================ Editor ================================
static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(5); }
template <typename Done> static void pumpUntil(Done done)
{
    for (auto end = juce::Time::getMillisecondCounter() + 2000; !done() && juce::Time::getMillisecondCounter() < end;)
        pump(), juce::Timer::callPendingTimersSynchronously();
}
static juce::Component* find(juce::Component& c, const juce::String& id)
{
    if (c.getComponentID() == id) return &c;
    for (auto* child : c.getChildren())
        if (auto* found = find(*child, id)) return found;
    return nullptr;
}
static juce::String sha(const juce::MemoryOutputStream& m) { return juce::SHA256(m.getData(), m.getDataSize()).toHexString(); }
static juce::String pixels(const juce::Image& image)
{
    const juce::Image argb = image.convertedToFormat(juce::Image::ARGB);
    const juce::Image::BitmapData d(argb, juce::Image::BitmapData::readOnly);
    juce::MemoryOutputStream m;
    for (int y = 0; y < d.height; ++y) m.write(d.getLinePointer(y), (size_t) (d.width * d.pixelStride));
    return sha(m);
}

// Observe real repaint invalidations without a window (as crumb).
struct RepaintCapture : juce::CachedComponentImage
{
    juce::Component& component;
    juce::Rectangle<int> pending;
    explicit RepaintCapture(juce::Component& c) : component(c), pending(c.getLocalBounds()) {}
    void paint(juce::Graphics& g) override { component.paint(g); }
    bool invalidateAll() override { pending = component.getLocalBounds(); return false; }
    bool invalidate(const juce::Rectangle<int>& r) override { pending = pending.getUnion(r); return false; }
    void releaseResources() override {}
    bool consume(juce::Image& image, float scale)
    {
        if (pending.isEmpty()) return false;
        juce::Graphics g(image);
        g.addTransform(juce::AffineTransform::scale(scale)), g.reduceClipRegion(pending);
        paint(g), pending = {};
        return true;
    }
};

// Sizes 50-200 % at the fixed aspect, the size persisted in the state; partial repaints build the same image as a
// full render; an idle editor repaints nothing.
static void testEditorSizes()
{
    for (int percent : { 50, 100, 200 })
    {
        Proc p;
        prepare(p, 48000, 512);
        Host h;
        Run r;
        r.host = &h;
        run(p, 24000, 512, r);
        std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
        CHECK(e->getWidth() == 1024 && e->getHeight() == 576);
        auto& timer = dynamic_cast<juce::Timer&>(*e);
        timer.stopTimer();
        e->setSize(1024 * percent / 100, 576 * percent / 100), e->setVisible(true);
        CHECK(p.editorWidth == 1024 * percent / 100);
        auto* capture = new RepaintCapture(*e);
        e->setCachedComponentImage(capture);
        const float scale = (float) percent / 50;
        juce::Image image(juce::Image::RGB, 1024 * percent / 50, 576 * percent / 50, true);
        capture->consume(image, scale);
        int paints = 0;
        for (int frame = 0; frame < 40; ++frame)
        {
            if (frame < 30) r.offset = 24000 + 1600 * frame, run(p, 1600, 512, r);
            if (frame == 10) set(p, { { "curve", 3 }, { "color", 2 }, { "tempoSync", 0 } });
            if (frame == 15) set(p, { { "shift", -7 }, { "depth", 23 }, { "freeHz", 10.1f }, { "soothe", 100 }, { "output", -2 } });
            if (frame == 20) set(p, { { "shift", 0 }, { "depth", 9 }, { "freeHz", 2 }, { "soothe", 30 }, { "output", 0 } });
            timer.timerCallback();
            const bool painted = capture->consume(image, scale);
            paints += painted;
            if (frame >= 31) CHECK(!painted); // no audio, nothing changed: idle
        }
        CHECK(paints >= 25);
        juce::Image fresh(juce::Image::RGB, image.getWidth(), image.getHeight(), true), other(juce::Image::RGB, 8, 8, true);
        { juce::Graphics g(other); g.addTransform(juce::AffineTransform::scale(0.01f)); e->paint(g); }
        { juce::Graphics g(fresh); g.addTransform(juce::AffineTransform::scale(scale)); e->paint(g); }
        CHECK(pixels(image) == pixels(fresh));
        Proc q;
        load(q, save(p));
        std::unique_ptr<juce::AudioProcessorEditor> reopened(q.createEditor());
        CHECK(reopened->getWidth() == 1024 * percent / 100 && reopened->getHeight() == 576 * percent / 100);
    }
}

// Telemetry: the input level and the output trim move the output meter, the sidechain level moves the sidechain meter;
// the threshold moves its mark. No control moves, and none covers the meters.
static juce::Rectangle<int> cellArea(int x, int y, int w, int h) // the editor's cells() at 100 %
{
    const auto px = [](int k) { return juce::roundToInt(1024.0f / 120 * (float) k); };
    const auto py = [](int k) { return juce::roundToInt(576.0f / 34 * (float) k); };
    return juce::Rectangle<int>::leftTopRightBottom(px(x), py(y), px(x + w), py(y + h));
}
static void testTelemetry()
{
    Proc p;
    set(p, { { "trigger", 2 }, { "depth", 0 } });
    prepare(p, 48000, 512);
    Host h;
    float in = 1, sc = 1;
    Run r;
    r.host = &h, r.in = [&](int i, int c) { return in * bass(i) * (c == 0 ? 1.0f : 0.7f); }, r.sc = [&](int i) { return sc * kick(i); };
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    auto& timer = dynamic_cast<juce::Timer&>(*e);
    timer.stopTimer();
    auto* layer = find(*e, "prev")->getParentComponent();
    const auto controls = [&] {
        std::vector<juce::Rectangle<int>> v;
        for (auto* c : layer->getChildren()) if (c->isVisible()) v.push_back(c->getBounds());
        return v;
    };
    const auto before = controls();
    const juce::Rectangle<int> scMeter = cellArea(30, 30, 10, 1), outMeter = cellArea(101, 29, 16, 2);
    int offset = 0;
    const auto frame = [&] {
        r.offset = offset, offset += 24000, run(p, 24000, 512, r);
        timer.timerCallback();
        return e->createComponentSnapshot(e->getLocalBounds(), true, 1.0f);
    };
    const auto look = [](const juce::Image& image, juce::Rectangle<int> area) { return pixels(image.getClippedImage(area)); };
    const auto loud = frame();
    const float out0 = p.feed.outLevel, sc0 = p.feed.scLevel;
    in = 0.1f;
    const auto quiet = frame();
    const float out1 = p.feed.outLevel;
    CHECK(out0 > 0.3f && out1 < 0.5f * out0 && look(quiet, outMeter) != look(loud, outMeter));
    set(p, "output", -12);
    const auto trimmed = frame();
    CHECK(p.feed.outLevel < 0.5f * out1 && look(trimmed, outMeter) != look(quiet, outMeter));
    sc = 0.1f;
    const auto soft = frame();
    CHECK(sc0 > 0.01f && p.feed.scLevel < 0.5f * sc0 && look(soft, scMeter) != look(trimmed, scMeter));
    set(p, "thresh", -6), timer.timerCallback();
    CHECK(look(e->createComponentSnapshot(e->getLocalBounds(), true, 1.0f), scMeter) != look(soft, scMeter));
    CHECK(controls() == before);
    for (const auto& c : controls())
        for (const auto& area : { scMeter, outMeter }) CHECK(!c.intersects(area));
}

// Every control through the component tree and JUCE's handlers (not peer hit-testing): knobs and the free rate drag
// with the Slider's 250 px sensitivity, reset on Slider::mouseDoubleClick and type on Label::showEditor; the slope's
// handle (found by getComponentAt) drags sideways and stays under the cursor; words, toggles, curve rows, the rate
// stepper, the held peak and the header are Button clicks. Visible controls never overlap.
static void testInteraction()
{
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("gulp-ui", "", false);
    for (int percent : { 50, 100, 200 })
    {
        Proc p(folder);
        prepare(p, 48000, 512);
        std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
        auto& timer = dynamic_cast<juce::Timer&>(*e);
        e->setSize(1024 * percent / 100, 576 * percent / 100), e->setVisible(true);
        auto* layer = find(*e, "prev")->getParentComponent();
        const auto centre = [&](juce::Component& c, juce::Rectangle<int> area) { return e->getLocalArea(&c, area).toFloat().getCentre(); };
        const auto at = [&](const juce::String& id) { auto* c = find(*e, id); CHECK(c != nullptr && c->isVisible()); return centre(*c, c->getLocalBounds()); };
        const auto click = [&](juce::Point<float> where) {
            auto* c = e->getComponentAt(where.roundToInt());
            if (auto* b = dynamic_cast<juce::Button*>(c))
            {
                auto done = std::make_shared<bool>(false);
                b->triggerClick(), juce::MessageManager::callAsync([done] { *done = true; }), pumpUntil([&] { return *done; });
            }
            else if (auto* l = dynamic_cast<juce::Label*>(c))
                CHECK(l->isEditableOnSingleClick()), l->showEditor();
            else if (auto* slider = dynamic_cast<juce::Slider*>(c))
            {
                const auto pos = slider->getLocalBounds().toFloat().getCentre();
                const auto time = juce::Time::getCurrentTime();
                const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys::leftButtonModifier,
                                             1, 0, 0, 0, 0, slider, slider, time, pos, time, 1, false);
                slider->mouseDown(event), slider->mouseUp(event);
            }
            CHECK(dynamic_cast<juce::Button*>(c) != nullptr || dynamic_cast<juce::Label*>(c) != nullptr || dynamic_cast<juce::Slider*>(c) != nullptr);
            timer.timerCallback();
        };
        const auto press = [&](const juce::String& id) { click(at(id)); };
        const auto overlaps = [&] {
            std::vector<juce::Component*> visible;
            for (auto* c : layer->getChildren())
                if (c->isVisible()) visible.push_back(c);
            for (size_t i = 0; i < visible.size(); ++i)
                for (size_t j = i + 1; j < visible.size(); ++j)
                    if (visible[i]->getBounds().intersects(visible[j]->getBounds())) { std::cerr << visible[i]->getComponentID() << " x " << visible[j]->getComponentID() << "\n"; return true; }
            return false;
        };
        CHECK(!overlaps());
        for (auto [id, x, y] : { std::tuple { "shift", 60, 25 }, { "thresh", 73, 25 }, { "soothe", 86, 25 },
                                { "depth", 104, 3 }, { "split", 104, 12 }, { "output", 104, 21 } })
            CHECK(find(*e, id)->getBounds() == cellArea(x, y, 10, 6));
        const auto time = juce::Time::getCurrentTime();
        const auto mouse = [&](juce::Component* c, juce::Point<float> pos, juce::Point<float> down, int clicks = 1) {
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys::leftButtonModifier, 1, 0, 0, 0, 0,
                                    c, c, time, down, time, clicks, pos != down);
        };
        const auto box = [](juce::Component* slider) {
            juce::Label* l = nullptr;
            for (auto* child : slider->getChildren()) l = l != nullptr ? l : dynamic_cast<juce::Label*>(child);
            return l;
        };
        const auto type = [&](const char* id, float normalised) { // click the value, type a value
            auto* label = box(find(*e, id));
            auto* param = p.apvts.getParameter(id);
            click(centre(*label, label->getLocalBounds()));
            CHECK(label->isBeingEdited());
            const auto typed = param->getText(normalised, 0);
            if (auto* editor = label->getCurrentTextEditor()) editor->setText(typed);
            label->hideEditor(false);
            CHECK(param->getCurrentValueAsText() == typed);
        };
        const auto dial = [&](const char* id) {
            auto* slider = dynamic_cast<juce::Slider*>(find(*e, id));
            auto* param = p.apvts.getParameter(id);
            CHECK(box(slider) != nullptr && slider->isVisible());
            juce::Point<float> grab = slider->getLocalBounds().toFloat().getCentre(); // a point off the value box, if any
            auto* target = e->getComponentAt(centre(*slider, juce::Rectangle<int>((int) grab.x, (int) grab.y, 1, 1)).roundToInt());
            if (target != slider) std::cerr << "drag target for " << id << ": " << target->getComponentID() << " " << target->getName() << "\n";
            CHECK(target == slider);
            param->setValueNotifyingHost(0.3f);
            const auto to = grab - juce::Point<float>(0, 50.0f); // 50 design px up = 0.2 of the range
            slider->mouseDown(mouse(slider, grab, grab)), slider->mouseDrag(mouse(slider, to, grab)), slider->mouseUp(mouse(slider, to, grab));
            CHECK(std::abs(param->getValue() - 0.5f) < 0.01f);
            slider->mouseDoubleClick(mouse(slider, grab, grab, 2));
            CHECK(param->getValue() == param->getDefaultValue());
            timer.timerCallback();
            type(id, 0.7f);
        };
        for (auto* id : { "shift", "thresh", "soothe", "depth", "split", "output" })
            dial(id);

        // The slope: getComponentAt finds the handle on the ◆ (and nothing in the open graph); a sideways drag of a
        // quarter cycle from 0.3 to 0.6 is slope +100 % and the ◆ is under the cursor again; double-click resets.
        set(p, { { "slope", 0 }, { "shift", 0 }, { "soothe", 0 }, { "curve", 0 } }), timer.timerCallback();
        auto* slope = find(*e, "slope");
        const auto diamond = [&] { return slope->getPosition().toFloat() + juce::Point<float>(1.5f, 1.5f) * juce::Point<float>(1024.0f / 120, 576.0f / 34); }; // in the layer
        const auto onEditor = [&](juce::Point<float> inLayer) { return e->getLocalPoint(layer, inLayer); };
        CHECK(e->getComponentAt(onEditor(diamond()).roundToInt()) == slope && std::abs(diamond().x - (25 + 0.3f * 70) * 1024 / 120 - 4) < 6);
        CHECK(e->getComponentAt(onEditor({ (25 + 0.8f * 70) * 1024 / 120, (5 + 8) * 576 / 34.0f }).roundToInt()) != slope);
        // Empty space between the grip and value label must not start a drag.
        CHECK(e->getComponentAt(onEditor(slope->getPosition().toFloat() + juce::Point<float>(0.5f * 1024 / 120, 0.5f * 576 / 34)).roundToInt()) != slope);
        const auto start = diamond();
        const auto grab = slope->getLocalPoint(layer, start);
        const float cycle = 70 * 1024 / 120.0f;
        slope->mouseDown(mouse(slope, grab, grab));
        slope->mouseDrag(mouse(slope, grab + juce::Point<float>(0.1f * cycle, 0), grab));
        CHECK(std::abs(get(p, "slope") - 100 * std::log2(0.4 / 0.3)) < 0.2f);
        timer.timerCallback(); // the handle moves under the drag: the drag still measures from where it began
        const auto down = slope->getLocalPoint(layer, start); // as JUCE reports it on the moved component
        slope->mouseDrag(mouse(slope, down + juce::Point<float>(0.3f * cycle, 0), down));
        slope->mouseUp(mouse(slope, down + juce::Point<float>(0.3f * cycle, 0), down));
        CHECK(std::abs(get(p, "slope") - 100) < 0.01f), timer.timerCallback();
        CHECK(std::abs(diamond().x - (25 + 0.6f * 70) * 1024 / 120 - 4) < 6 && e->getComponentAt(onEditor(diamond()).roundToInt()) == slope);
        slope->mouseDoubleClick(mouse(slope, grab, grab, 2)), CHECK(get(p, "slope") == 0);
        timer.timerCallback();
        CHECK(e->getComponentAt(onEditor(diamond()).roundToInt()) == slope);
        type("slope", 0.25f), CHECK(get(p, "slope") == -50);
        set(p, "slope", 0), timer.timerCallback();

        // Rate: ‹ › step the rates up to 2/1 (2 bars, clamped); an old 4/1 shows, stays and steps down; in free they walk
        // the ladder of round Hz and the value drags and types.
        CHECK(!find(*e, "freeHz")->isVisible());
        set(p, "rate", 8), timer.timerCallback();
        press("rate+"), CHECK(get(p, "rate") == 9);
        for (int k = 0; k < 14; ++k) press("rate-");
        CHECK(get(p, "rate") == 0), press("rate+"), CHECK(get(p, "rate") == 1);
        for (int k = 0; k < 20; ++k) press("rate+"), CHECK(!overlaps());
        CHECK(get(p, "rate") == 14 && find(*e, "rate+")->getBounds() == cellArea(82, 3, 1, 1) && find(*e, "rate-")->getBounds() == cellArea(71, 3, 1, 1)); // "2 bars"
        load(p, blob(R"(<GULP stateVersion="2"><PARAM id="rate" value="15.0"/></GULP>)")); // a 0.2.0 session at 4/1
        timer.timerCallback(), CHECK(get(p, "rate") == 15 && p.apvts.getParameter("rate")->getCurrentValueAsText() == "4/1" && !overlaps());
        press("rate+"), CHECK(get(p, "rate") == 15);
        press("rate-"), CHECK(get(p, "rate") == 14), press("rate+"), CHECK(get(p, "rate") == 14);
        press("rate-"), CHECK(get(p, "rate") == 13 && find(*e, "rate-")->getBounds() == cellArea(72, 3, 1, 1)); // "1 bar"
        press("rate+"), press("free"), CHECK(get(p, "tempoSync") == 0 && find(*e, "freeHz")->isVisible() && get(p, "rate") == 14);
        press("free"), CHECK(get(p, "tempoSync") == 0); // choices are idempotent, not toggles
        CHECK(!overlaps());
        set(p, "freeHz", 2), timer.timerCallback();
        press("rate+"), CHECK(get(p, "freeHz") == 3);
        press("rate-"), press("rate-"), CHECK(get(p, "freeHz") == 1.5f);
        set(p, "freeHz", 2.2f), timer.timerCallback(), press("rate-"), CHECK(get(p, "freeHz") == 2);
        for (int k = 0; k < 20; ++k) press("rate+"), CHECK(!overlaps() && find(*e, "rate-")->getRight() <= find(*e, "freeHz")->getX());
        CHECK(get(p, "freeHz") == 20 && get(p, "rate") == 14);
        for (int k = 0; k < 20; ++k) press("rate-");
        CHECK(std::abs(get(p, "freeHz") - 0.1f) < 1e-6f);
        dial("freeHz");
        press("sync"), CHECK(get(p, "tempoSync") == 1 && !find(*e, "freeHz")->isVisible());
        press("sync"), CHECK(get(p, "tempoSync") == 1);
        set(p, "tempoSync", 0), timer.timerCallback();
        CHECK(dynamic_cast<juce::Button*>(find(*e, "free"))->getToggleState() && !dynamic_cast<juce::Button*>(find(*e, "sync"))->getToggleState());
        press("sync");
        CHECK(dynamic_cast<juce::Button*>(find(*e, "sync"))->getToggleState() && !dynamic_cast<juce::Button*>(find(*e, "free"))->getToggleState());
        CHECK(find(*e, "sync")->getBounds() == cellArea(85, 3, 4, 2) && find(*e, "free")->getBounds() == cellArea(91, 3, 4, 2));
        CHECK(find(*e, "loop")->getBounds() == cellArea(25, 29, 6, 1) && find(*e, "loop")->getX() == find(*e, "trigger host")->getX());
        CHECK(find(*e, "lookahead")->getBounds() == cellArea(35, 29, 11, 1) && find(*e, "lookahead")->getX() == find(*e, "trigger audio")->getX());

        // The held peak: click the number to zero it.
        p.feed.held = 0.6f, timer.timerCallback();
        CHECK(e->getComponentAt(onEditor(cellArea(40, 3, 1, 1).toFloat().getCentre()).roundToInt()) == find(*e, "peak"));
        click(onEditor(cellArea(40, 3, 1, 1).toFloat().getCentre())), CHECK(p.feed.held == 0);

        // The modes spread evenly from the graph's left edge to its right edge.
        const juce::StringArray modes { "vol", "lows", "highs", "dark", "thin", "pinch", "hollow", "grit" };
        CHECK(find(*e, "color vol")->getX() == cellArea(25, 0, 1, 1).getX() && find(*e, "color grit")->getRight() == cellArea(25, 0, 70, 1).getRight());
        for (int k = 1; k < modes.size(); ++k) // equal gaps (5 cells, give or take a pixel of rounding)
            CHECK(std::abs(find(*e, "color " + modes[k])->getX() - find(*e, "color " + modes[k - 1])->getRight() - cellArea(0, 0, 5, 1).getWidth()) <= 1);
        const std::pair<const char*, juce::StringArray> choices[] = {
            { "curve", { "classic", "tight", "punch", "round", "long", "slam", "sub", "sub long", "trim", "suck", "wave", "double" } },
            { "color", modes }, { "stereo", { "both", "mid", "side" } }, { "trigger", { "host", "midi", "audio" } } };
        for (auto& [id, items] : choices)
            for (int k = items.size(); --k >= 0;)
                press(juce::String(id) + " " + items[k]), CHECK(get(p, id) == (float) k);
        CHECK(find(*e, "curve double")->getBottom() == cellArea(0, 0, 1, 31).getBottom() && find(*e, "curve classic")->getY() == cellArea(0, 5, 1, 1).getY());
        for (auto* id : { "invert", "loop", "lookahead" })
            press(id), CHECK(get(p, id) == 1), press(id), CHECK(get(p, id) == 0);
        p.setCurrentProgram(3);
        press("next"), CHECK(p.getCurrentProgram() == 4);
        press("prev"), press("prev"), CHECK(p.getCurrentProgram() == 2);
        press("B"), CHECK(p.abSlot() == 1);
        press("A"), CHECK(p.abSlot() == 0);
        const int files = folder.getNumberOfChildFiles(juce::File::findFiles);
        press("save"), CHECK(folder.getNumberOfChildFiles(juce::File::findFiles) == files + 1 && p.preset == -1);
        CHECK(!overlaps());
    }
    folder.deleteRecursively();
}

// The nudge is stored as % of a cycle and shown in ms of one cycle at the live tempo and rate (the host's last tempo, 120
// without one; 1 / Hz in free), on the parameter itself and on its knob; typed ms convert back and clamp. Trigger index
// 0 reads "host" in the editor and the host, and saves as 0 as it always did.
static void testDisplayNames()
{
    Proc p;
    prepare(p, 48000, 512);
    auto* nudge = p.apvts.getParameter("shift");
    const auto text = [&](float percent) { return nudge->getText(nudge->convertTo0to1(percent), 0); };
    const auto value = [&](const juce::String& t) { return nudge->convertFrom0to1(nudge->getValueForText(t)); };
    const auto near = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
    const auto roundTrip = [&](float percent, float cycleMs) { // text -> value lands within half a ms (+ the 0.1 % step)
        const float back = value(text(percent));
        return text(back) == text(percent) && std::abs(back - percent) <= 50 / cycleMs + 0.05f + 1e-4f;
    };
    CHECK(nudge->getLabel() == "ms" && p.feed.bpm == 120);
    // No host tempo yet (as standalone): 120 BPM, 1/4 = 500 ms.
    CHECK(text(2.4f) == "+12 ms" && text(0) == "0 ms" && text(-1.6f) == "-8 ms");
    CHECK(near(value("+12 ms"), 2.4f) && near(value("-8 ms"), -1.6f) && near(value(juce::String::fromUTF8("\xe2\x88\x92" "8 ms")), -1.6f) && value("0 ms") == 0);
    CHECK(value("+9999 ms") == 50 && value("-9999 ms") == -50 && near(value("12.5%"), 12.5f)); // clamped; % still typed as %
    CHECK(roundTrip(10, 500) && roundTrip(-33.3f, 500));
    set(p, "rate", 0), CHECK(text(0.1f) == "0 ms" && text(-0.1f) == "0 ms"); // 1/32: one step is 0.06 ms, never "-0"
    // Host at 124 BPM, 1/16: 120.97 ms; 3/16 dotted 1/16 (index 3) = 181.45 ms.
    Host h;
    run(p, 512, 512, [&] { Run r; r.host = &h; return r; }());
    set(p, "rate", 2);
    CHECK(p.feed.bpm == 124 && text(10) == "+12 ms" && text(-50) == "-60 ms" && roundTrip(10, 120.97f) && roundTrip(-27.5f, 120.97f));
    set(p, "rate", 3), CHECK(text(10) == "+18 ms" && roundTrip(41.2f, 181.45f));
    // Free 4 Hz: 250 ms, whatever the tempo.
    set(p, { { "tempoSync", 0 }, { "freeHz", 4 } });
    CHECK(text(-20) == "-50 ms" && near(value("-50 ms"), -20) && text(4.8f) == "+12 ms" && roundTrip(17.3f, 250));
    // A playhead without a tempo, and none at all: 120 again.
    set(p, { { "tempoSync", 1 }, { "rate", 8 } }), h.bpm = 90;
    run(p, 512, 512, [&] { Run r; r.host = &h; return r; }()), CHECK(p.feed.bpm == 90);
    h.known = false;
    run(p, 512, 512, [&] { Run r; r.host = &h; return r; }()), CHECK(p.feed.bpm == 120 && text(-1.6f) == "-8 ms");
    h.known = true, h.bpm = 90;
    run(p, 512, 512, [&] { Run r; r.host = &h; return r; }()), run(p, 512, 512), CHECK(p.feed.bpm == 120);

    // The knob shows the same text, and follows a tempo change without the value moving.
    set(p, { { "rate", 8 }, { "shift", 2.4f } });
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    auto& timer = dynamic_cast<juce::Timer&>(*e);
    timer.stopTimer();
    auto* knob = dynamic_cast<juce::Slider*>(find(*e, "shift"));
    const auto shown = [&] { timer.timerCallback(); auto* l = dynamic_cast<juce::Label*>(knob->getChildComponent(0)); return l != nullptr ? l->getText() : juce::String(); };
    CHECK(shown() == "+12 ms");
    h.bpm = 60;
    run(p, 512, 512, [&] { Run r; r.host = &h; return r; }()), CHECK(shown() == "+24 ms" && get(p, "shift") == 2.4f && nudge->getCurrentValueAsText() == "+24 ms");
    set(p, "shift", -1.6f), CHECK(shown() == juce::String::fromUTF8("\xe2\x88\x92" "16 ms"));

    // "host": the trigger row's word and the host's text for index 0; the saved value is the index, unchanged.
    set(p, "trigger", 0), timer.timerCallback();
    auto* host = dynamic_cast<juce::Button*>(find(*e, "trigger host"));
    CHECK(host != nullptr && host->getToggleState() && find(*e, "trigger sync") == nullptr && host->getBounds() == cellArea(25, 27, 4, 2));
    CHECK(p.apvts.getParameter("trigger")->getCurrentValueAsText() == "host" && get(p, "trigger") == 0);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(save(p).getData(), (int) save(p).getSize());
    CHECK(xml != nullptr && xml->getChildByAttribute("id", "trigger") != nullptr && xml->getChildByAttribute("id", "trigger")->getStringAttribute("value") == "0.0");
    Proc old; // a 0.2.0 session's sync trigger (index 0) loads as host
    load(old, blob(R"(<?xml version="1.0" encoding="UTF-8"?>)" "\n" R"(<GULP stateVersion="2"><PARAM id="trigger" value="0.0"/><PARAM id="shift" value="12.5"/></GULP>)"));
    CHECK(get(old, "trigger") == 0 && old.apvts.getParameter("trigger")->getCurrentValueAsText() == "host" && get(old, "shift") == 12.5f);
}

// ================================ Screenshot and goldens ================================
// Preset 03 with the bass on the input and the kick on the sidechain at 124 bpm, a few cycles, then 30 editor frames.
static std::unique_ptr<juce::AudioProcessorEditor> fixture(Proc& p, Host& h, const Settings& extra = {})
{
    p.setCurrentProgram(3), set(p, extra);
    prepare(p, 48000, 512);
    Run r;
    r.host = &h, r.sc = [](int i) { // a kick on every quarter at 124 bpm
        const double q = 48000 * 60 / 124.0; return (float) (0.8 * std::sin(2 * pi * (50 * std::fmod(i, q) / 48000 + 2 * (1 - std::exp(-std::fmod(i, q) / 960)))) * std::exp(-std::fmod(i, q) / 2160)); }; // 45 ms decay
    run(p, 48000, 512, r);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    for (int frame = 0; frame < 30; ++frame)
    {
        r.offset = 48000 + 1600 * frame, run(p, 1600, 512, r);
        dynamic_cast<juce::Timer&>(*e).timerCallback();
    }
    return e;
}
static juce::Image snapshot(int percent, const Settings& extra = {})
{
    Proc p;
    Host h;
    auto e = fixture(p, h, extra);
    e->setSize(1024 * percent / 100, 576 * percent / 100);
    return e->createComponentSnapshot(e->getLocalBounds(), true, 2.0f);
}
static int screenshot(const juce::File& file, const juce::StringArray& changes)
{
    Settings extra;
    for (auto& c : changes)
        if (const int i = ids.indexOf(c.upToFirstOccurrenceOf("=", false, false)); i >= 0)
            extra.push_back({ ids[i].toRawUTF8(), c.fromFirstOccurrenceOf("=", false, false).getFloatValue() });
        else
            return std::cerr << "unknown parameter in " << c << "\n", 1;
    const auto image = snapshot(100, extra);
    file.deleteFile();
    juce::FileOutputStream out(file);
    return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out) ? 0 : 1;
}
// 4 s of bass + kick sidechain + MIDI + moving ppq at 48 kHz from preset 03 with the trigger or colour changed, hashed.
static juce::String render(int trigger, int colour)
{
    Proc p;
    p.setCurrentProgram(3);
    set(p, { { "trigger", (float) trigger }, { "color", (float) colour }, { "shift", 5 } });
    prepare(p, 48000, 512);
    Host h;
    Run r;
    r.host = &h, r.notes = { 0, 23226, 46452, 69677, 92903, 116129, 139355, 162581 };
    std::vector<float> right;
    r.right = &right;
    const auto y = run(p, 4 * 48000, 512, r);
    juce::MemoryOutputStream bytes;
    bytes.write(y.data(), y.size() * sizeof(float)), bytes.write(right.data(), right.size() * sizeof(float));
    return sha(bytes);
}
struct Golden { int trigger, colour; const char* hash; };
// Captured on Debian 13 x86-64, GCC 14.2, glibc 2.41, JUCE's software renderer (enforced only where the probes match).
// soothe 0 is 0.1.0's sound bit for bit: the lows hashes are 0.1.0's for band lows + level (preset 03 then), and the
// vol, dark .. grit hashes are 0.1.0's for band full at tone 50 (checked against a 0.1.0 build). Only the golden list
// changed: 0.1.0 rendered every colour on preset 03's band lows, which no longer exists.
static const Golden sounds[] = {
    { 0, 1, "c7b229c3a57632920e0fa841a4c117377342fe461478f56e67b545b3d23b959e" },
    { 1, 1, "2fbf5f15b256e0590bbfd6847bd8b68ccefbb1e2abd5faf65653ee8b27b9a436" },
    { 2, 1, "b77f5685ee4c16e8e30543ededa8361e18702c13a8fefef8ffe62175272b3063" },
    { 0, 0, "7583bb28246ea674a1b934274b33b03cfefd14cc0b83ad79c935f111c053e450" },
    { 0, 2, "b3374ab8b411ae3833e81f4a3778b4089b408c77eba9278bfbc7b519fb017237" },
    { 0, 3, "eae6f392095da1ebf1610e365b44f41a63abb9a508d0b4d51a5326833a18027e" },
    { 0, 4, "f174fdcf3360201e29c4276771a534c663575f59ec02fc30aee237173139e600" },
    { 0, 5, "0a2ec4453fdbd65e0451c7f89c36bdb618e941ab543000eebb32a32613ec8de4" },
    { 0, 6, "03605e6dfe815adbeedede492bfc2cfe1c00a676c6571bcc58ebd23c2e0c87a3" },
    { 0, 7, "f3ef30e486df5dfa2c033e2e2bb9cc182a3cf82a373247bb2527928ee56863f7" },
};
struct Shot { int percent; const char* hash; };
static const Shot shots[] = { { 100, "831c28b9a53187353072e91db702b814d906eafac14565e133f8286b539bf509" },
                              { 50, "7d4c1816a369e1f7919661a31bb1cf0f235e0123e406387362ccaa7b1a30ee11" },
                              { 200, "36b186d872ead8104eceb3d04f9aee417d273041df957a06fc8f2a81f0808e97" } };
static void testGoldens()
{
    CHECK(render(0, 0) == render(0, 0) && pixels(snapshot(100)) == pixels(snapshot(100)));
#if JUCE_LINUX && JUCE_INTEL && JUCE_64BIT
    // Independent probes (never plug-in output): libm and the software renderer, as crumb.
    juce::MemoryOutputStream math;
    for (int i = 1; i <= 256; ++i)
    {
        volatile double input = i * 0.123456789;
        const double x = input;
        const double values[] = { std::sin(x), std::cos(x), std::tan(x), std::exp(-x), std::exp2(x), std::log(x), std::log2(x), std::log1p(x), std::tanh(x), std::pow(x, 0.37) };
        math.write(values, sizeof(values));
    }
    juce::Image probe(juce::Image::ARGB, 128, 32, true);
    {
        juce::Graphics g(probe);
        g.setColour(juce::Colours::white), g.fillRoundedRectangle(0.25f, 0.75f, 127.0f, 31.0f, 4.5f);
        g.setColour(juce::Colours::black);
        g.setFont(juce::Font(juce::FontOptions(juce::Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize)).withPointHeight(11.0f)));
        g.drawText("gulp 0123456789", probe.getBounds(), juce::Justification::centred, false);
    }
    const bool matchingMath = sha(math) == "07adb83962bd7f372226ea055bca705c159534fa4a19d3279488b12cd95eea5c";
    const bool matchingRenderer = pixels(probe) == "af951f38787ba7f604a30693c9d3e31c8086bc2d6f549bada12d2be7462f914b";
    const bool ci = std::getenv("GITHUB_ACTIONS") != nullptr; // CI compilers change processBlock bits: skipped there
    if (matchingMath && !ci)
        for (const auto& s : sounds)
            if (const auto hash = render(s.trigger, s.colour); hash != s.hash)
                CHECK(!"bit-exact render"), std::cerr << "  trigger " << s.trigger << " colour " << s.colour << ": " << hash << "\n";
    if (matchingMath && matchingRenderer && !ci)
        for (const auto& s : shots)
            if (const auto hash = pixels(snapshot(s.percent)); hash != s.hash)
                CHECK(!"pixel-exact snapshot"), std::cerr << "  " << s.percent << " %: " << hash << "\n";
    std::cout << "Environment fingerprints: " << sha(math) << " / " << pixels(probe) << "\nChecked " << (matchingMath && !ci ? std::size(sounds) : 0)
              << " audio hashes and " << (matchingMath && matchingRenderer && !ci ? std::size(shots) : 0) << " pixel hashes"
              << (ci ? " (skipped on GitHub Actions)\n" : " (matching environments only)\n");
#endif
}

#if JUCE_LINUX
#include <dlfcn.h>
static void ignoreXErrors() // as crumb: Xlib's default handler exits on a benign error on a bare Xvfb
{
    if (auto* x11 = dlopen("libX11.so.6", RTLD_NOW))
        if (auto setHandler = (void* (*) (int (*)(void*, void*))) dlsym(x11, "XSetErrorHandler"))
            setHandler([](void*, void*) { return 0; });
}
#endif

int main(int argc, char** argv)
{
#if JUCE_LINUX
    ignoreXErrors();
#endif
    const juce::ScopedJuceInitialiser_GUI gui;
    if (argc >= 3 && juce::String(argv[1]) == "--screenshot")
        return screenshot(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]), juce::StringArray(argv + 3, argc - 3));
    testGoldens();
    testIdentity();
    testState();
    testPresets();
    testSync();
    testHostEdges();
    testTriggers();
    testIdle();
    testDelayedTriggers();
    testLookahead();
    testClicks();
    testSoothe();
    testTransport();
    testBands();
    testColours();
    testLifecycle();
    testRealtime();
    testEditorSizes();
    testTelemetry();
    testInteraction();
    testDisplayNames();
    std::cout << (failures == 0 ? "All Gulp tests passed\n" : "Gulp tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
