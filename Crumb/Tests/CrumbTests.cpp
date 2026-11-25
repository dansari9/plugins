// CrumbTests: drives the real processor and editor without a host. Exit code 1 on failure.
//   CrumbTests --screenshot a.png [width] [clip|quiet|crush]   render the editor at 1x after a drum + bass loop
//   CrumbTests --plates dir                   the editor's riso plates as PNGs (the site's public/riso/crumb-*.png)
//   CrumbTests --render in.f32 out.f32 [preset=n] [id=value ...]   stereo interleaved float32 at 48 kHz, 128-sample blocks
//   CrumbTests --feed in.f32 out.f32 [preset=n] [id=value ...]     the display feed instead (dry, wet, gone, mark, cut, over per sample)
//   CrumbTests --bench | --bench-editor       DSP and editor timings
#include "Crumb.h"
#include "Wave.h"
#include "BinaryData.h"
#include <juce_cryptography/juce_cryptography.h>
#define SCREENSHOT_PRESET 28 // "56k stream", the approved frame's state
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <set>
#include <new>
#include <thread>

// Real-time check: count heap allocations made on a thread flagged as "audio". operator new is
// counted everywhere; on glibc (outside ASan, which owns malloc) the C allocators are too, which
// catches JUCE's HeapBlock / AudioBuffer storage.
static thread_local bool onAudioThread = false;
static std::atomic<int> audioAllocations { 0 };
static void countAllocation() { audioAllocations += onAudioThread ? 1 : 0; }
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#define CRUMB_COUNTS_MALLOC 1
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
void* operator new(std::size_t n, std::align_val_t a) // glibc's aligned path bypasses the malloc hooks
{
    countAllocation();
    void* p = nullptr;
    if (posix_memalign(&p, std::max(sizeof(void*), (std::size_t) a), n == 0 ? 1 : n) == 0)
        return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return countAllocation(), std::malloc(n == 0 ? 1 : n); } // std::stable_sort
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
static void escape(void* p) { asm volatile("" : : "r"(p) : "memory"); } // p and its memory become observable

static int failures = 0;
#define CHECK(c) ((c) ? (void) 0 : (void) (++failures, std::cerr << "FAIL " << __LINE__ << ": " #c "\n"))

using Proc = CrumbProcessor;
using Settings = std::vector<std::pair<const char*, float>>;
// FROZEN: this list is the contract with every saved session. The test fails if the plug-in's list changes.
static const juce::StringArray ids { "preamp", "bits",   "dcShift", "dither", "rate", "jitter", "approxOn", "approxDev", "imagesOn",
                                     "imagesShift", "fType", "fOrder", "cutoff", "reso", "mix",  "output", "bypass", "autoGain",
                                     "loss", "speed", "drop", "fill", "keep", "ear", "gate", "stereo", "lowCut", "highCut", "slope", "invert", "auto" };

static void set(Proc& p, const char* id, float v)
{
    auto* param = p.apvts.getParameter(id);
    param->setValueNotifyingHost(param->convertTo0to1(v));
}
static void set(Proc& p, const Settings& s)
{
    for (auto& [id, v] : s)
        set(p, id, v);
}
static float get(Proc& p, const juce::String& id) { return p.apvts.getRawParameterValue(id)->load(); }
static const Settings cleanCrush { { "bits", 0 }, { "rate", 44100 }, { "approxOn", 0 } };
static bool defaults(Proc& p)
{
    for (auto* param : p.getParameters())
        if (param->getValue() != param->getDefaultValue())
            return false;
    return true;
}

static void prepare(Proc& p, double fs, int block, int ch)
{
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(ch));
    l.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(ch));
    CHECK(p.setBusesLayout(l));
    p.setPlayConfigDetails(ch, ch, fs, block);
    p.prepareToPlay(fs, block);
}

// Test signal: two sines plus hashed (repeatable) noise, peak below the ADC clip's knee (0.5).
static float signal(int i, double fs)
{
    juce::uint32 h = (juce::uint32) i * 2654435761u;
    h = (h ^ (h >> 15)) * 0x2c1b3c6du, h ^= h >> 12;
    const double t = i / fs, noise = (h >> 8) * 0x1p-24;
    return (float) (0.2 * std::sin(2 * juce::MathConstants<double>::pi * 220 * t) + 0.15 * std::sin(2 * juce::MathConstants<double>::pi * 3150 * t)
                    + 0.1 * (noise * 2 - 1));
}

// Processes `samples` of `in(i)` in blocks of `block` (0 = random sizes 1..4096); returns channel 0.
template <typename In>
static std::vector<float> run(Proc& p, int block, int ch, int samples, In in, bool poison = false)
{
    juce::AudioBuffer<float> buffer(ch, 4096);
    juce::MidiBuffer midi;
    juce::Random sizes(3);
    std::vector<float> out;
    for (int start = 0, n = 0; start < samples; start += n)
    {
        n = juce::jmin(samples - start, block > 0 ? block : 1 + sizes.nextInt(4096));
        buffer.setSize(ch, n, false, false, true);
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < n; ++i)
                buffer.setSample(c, i, in(start + i) * (c == 0 ? 1.0f : -0.7f));
        if (poison && (start / juce::jmax(1, n)) % 3 == 1)
            buffer.setSample(0, n / 2, start % 2 ? NAN : INFINITY), buffer.setSample(ch - 1, 0, -INFINITY);
        onAudioThread = true;
        p.processBlock(buffer, midi);
        onAudioThread = false;
        for (int i = 0; i < n; ++i)
            out.push_back(buffer.getSample(0, i));
    }
    return out;
}

static juce::MemoryBlock blob(const juce::String& xml) // the copyXmlToBinary layout, text verbatim
{
    juce::MemoryOutputStream m;
    m.writeInt(0x21324356), m.writeInt((int) xml.getNumBytesAsUTF8()), m.write(xml.toRawUTF8(), xml.getNumBytesAsUTF8());
    return m.getMemoryBlock();
}
static void load(Proc& p, const juce::MemoryBlock& m) { p.setStateInformation(m.getData(), (int) m.getSize()); }

static void testIdentity()
{
    CHECK(JucePlugin_PluginCode == 0x43726d62);       // 'Crmb'
    CHECK(JucePlugin_ManufacturerCode == 0x44776b73); // 'Dwks'
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_AUMainType)) == "'aufx'");
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_CFBundleIdentifier)) == "com.dripworks.crumb");
    CHECK(juce::String(JucePlugin_Name) == "crumb" && juce::String(JucePlugin_Manufacturer) == "daveydidit");
    static_assert(JUCE_VST3_CAN_REPLACE_VST2 == 0 && JUCE_FORCE_USE_LEGACY_PARAM_IDS == 0, "frozen host-facing IDs");
    CHECK(!JucePlugin_IsSynth && !JucePlugin_WantsMidiInput);
    Proc p;
    juce::StringArray found;
    for (auto* param : p.getParameters())
    {
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(param);
        CHECK(withId != nullptr && withId->getVersionHint() == (found.size() < 17 ? 1 : found.size() == 17 ? 2 : 3)); // appended in 0.2.0 and 0.3.0
        CHECK(!withId->paramID.containsIgnoreCase("verb") && !withId->paramID.containsIgnoreCase("decay") && !withId->name.containsIgnoreCase("verb")); // no reverb
        found.add(withId->paramID);
    }
    CHECK(found == ids && found.size() == 31 && p.getName() == "crumb");
    CHECK(p.getBypassParameter() == p.apvts.getParameter("bypass"));
    // Plan v2 defaults.
    CHECK(get(p, "preamp") == 0 && get(p, "bits") == 12 && get(p, "dcShift") == 0 && get(p, "dither") == 0 && get(p, "rate") == 26040
          && get(p, "jitter") == 0 && get(p, "approxOn") == 1 && get(p, "approxDev") == 0 && get(p, "imagesOn") == 0
          && get(p, "imagesShift") == 0 && get(p, "fType") == 0 && get(p, "fOrder") == 1 && get(p, "cutoff") == 20000 && get(p, "reso") == 0
          && get(p, "mix") == 100 && get(p, "output") == 0 && get(p, "bypass") == 0 && get(p, "autoGain") == 0); // 0.4.0: auto gain off
    // 0.3.0's are neutral.
    CHECK(get(p, "loss") == 0 && get(p, "speed") == 100 && get(p, "drop") == 0 && get(p, "fill") == 0 && get(p, "keep") == 0 && get(p, "ear") == 0
          && get(p, "gate") == -96 && get(p, "stereo") == 0 && get(p, "lowCut") == 20 && get(p, "highCut") == 20000 && get(p, "slope") == 3 && get(p, "invert") == 0 && get(p, "auto") == 100);
    // 0.4.0's names (the IDs stay): what the host shows is what the panel says.
    const std::pair<const char*, const char*> titles[] { { "preamp", "Drive" }, { "bits", "Bits" }, { "dcShift", "DC Shift" }, { "dither", "Dither" }, { "rate", "Rate" }, { "jitter", "Jitter" }, { "approxDev", "Anti-Alias" },
        { "imagesShift", "Anti-Image" }, { "loss", "Loss" }, { "speed", "Speed" }, { "drop", "Packet Loss" }, { "fill", "Packets" }, { "keep", "Delta" }, { "ear", "Weighting" }, { "gate", "Gate" }, { "stereo", "Stereo Mode" },
        { "lowCut", "Low Cut" }, { "highCut", "High Cut" }, { "reso", "Resonance" }, { "slope", "Filter Slope" }, { "fOrder", "Filter Position" }, { "invert", "Filter Invert" }, { "mix", "Mix" }, { "output", "Output" }, { "bypass", "Bypass" } };
    for (auto& [id, name] : titles) CHECK(p.apvts.getParameter(id)->getName(64) == name);
    set(p, { { "fill", 1 }, { "keep", 1 }, { "ear", 0 } });
    CHECK(p.apvts.getParameter("fill")->getCurrentValueAsText() == "repeat" && p.apvts.getParameter("keep")->getCurrentValueAsText() == "on" && p.apvts.getParameter("ear")->getCurrentValueAsText() == "perceptual");
    set(p, { { "fill", 0 }, { "keep", 0 } });
    CHECK(p.apvts.getParameter("gate")->getCurrentValueAsText() == "off" && p.apvts.getParameter("rate")->getCurrentValueAsText() == "26.0 kHz");
    CHECK(p.apvts.getParameter("bits")->getText(0, 16) == "OFF");
    CHECK(p.getLatencySamples() == 0 && p.getTailLengthSeconds() == 0 && !p.acceptsMidi() && !p.producesMidi());
    prepare(p, 44100, 512, 2);
    CHECK(p.getLatencySamples() == 0);
}

// Frozen v1 fixture, exactly as Crumb 0.1.0 writes it (no autoGain). Must load in every future version.
static juce::MemoryBlock v1Fixture()
{
    return blob(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                "\n"
                R"(<CRUMB stateVersion="1"><PARAM id="approxDev" value="-25.0"/><PARAM id="approxOn" value="0.0"/>)"
                R"(<PARAM id="bits" value="8.0"/><PARAM id="bypass" value="0.0"/><PARAM id="cutoff" value="1234.5"/>)"
                R"(<PARAM id="dcShift" value="1.0"/><PARAM id="dither" value="40.0"/><PARAM id="fOrder" value="0.0"/>)"
                R"(<PARAM id="fType" value="2.0"/><PARAM id="imagesOn" value="1.0"/><PARAM id="imagesShift" value="60.0"/>)"
                R"(<PARAM id="jitter" value="15.0"/><PARAM id="mix" value="75.0"/><PARAM id="output" value="-3.5"/>)"
                R"(<PARAM id="preamp" value="12.5"/><PARAM id="rate" value="22050.0"/><PARAM id="reso" value="33.3"/></CRUMB>)");
}

static void testState()
{
    juce::Random r(42);
    for (int n = 0; n < 50; ++n) // random round trip into a brand-new instance: identical parameters and audio
    {
        Proc a, b;
        for (auto* param : a.getParameters())
            if (param != a.getBypassParameter())
                param->setValueNotifyingHost(r.nextFloat());
        CHECK(!defaults(a));
        juce::MemoryBlock m;
        a.getStateInformation(m);
        load(b, m);
        juce::MemoryBlock m2;
        b.getStateInformation(m2);
        CHECK(m == m2);
        for (auto& id : ids)
            CHECK(get(a, id) == get(b, id));
        CHECK(juce::AudioProcessor::getXmlFromBinary(m.getData(), (int) m.getSize())->getIntAttribute("stateVersion") == 4);
        if (n < 6)
        {
            prepare(a, 48000, 256, 2), prepare(b, 48000, 256, 2);
            const auto ya = run(a, 256, 2, 24000, [](int i) { return signal(i, 48000); });
            const auto yb = run(b, 256, 2, 24000, [](int i) { return signal(i, 48000); });
            CHECK(ya == yb);
        }
    }
    Proc v1;
    const auto fixture = v1Fixture();
    auto fixtureLoaded = [&](Proc& p) {
        return get(p, "approxDev") == -25.0f && get(p, "approxOn") == 0 && get(p, "bits") == 8 && get(p, "cutoff") == 1234.5f && get(p, "dcShift") == 1
               && get(p, "dither") == 40 && get(p, "fOrder") == 0 && get(p, "fType") == 2 && get(p, "imagesOn") == 1 && get(p, "imagesShift") == 60
               && get(p, "jitter") == 15 && get(p, "mix") == 75 && get(p, "output") == -3.5f && get(p, "preamp") == 12.5f && get(p, "rate") == 22050
               && get(p, "reso") == 33.3f && get(p, "bypass") == 0 && get(p, "autoGain") == 0 // 0.1.x: auto gain off
               && get(p, "loss") == 0 && get(p, "drop") == 0 && get(p, "lowCut") == 20 && get(p, "highCut") == 20000 && get(p, "gate") == -96 && get(p, "auto") == 100;
    };
    load(v1, fixture);
    CHECK(fixtureLoaded(v1) && v1.preset == -1 && v1.presetName() == "custom" && v1.abSlot() == 0 && v1.editorWidth == 0);
    for (float on : { 0.0f, 1.0f }) // autoGain round trip either way, into an instance holding the other value
    {
        Proc a, b;
        set(a, "autoGain", on), set(b, "autoGain", 1 - on);
        juce::MemoryBlock m;
        a.getStateInformation(m), load(b, m);
        CHECK(get(b, "autoGain") == on);
    }
    // A 0.1.x A/B slot (16 values) and a 0.1.x user preset file carry no autoGain: both load it off.
    load(v1, blob("<CRUMB ab='0' slotB='3;0.5" + juce::String::repeatedString(",0.5", 15) + "'/>"));
    set(v1, "autoGain", 1), v1.selectAb(1);
    CHECK(v1.abSlot() == 1 && get(v1, "autoGain") == 0 && get(v1, "mix") == 50);
    const auto old = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("crumb-old", "", false);
    old.createDirectory(), old.getChildFile("old.xml").replaceWithText("<CRUMB><PARAM id=\"bits\" value=\"7\"/></CRUMB>");
    Proc legacy(old);
    legacy.setCurrentProgram(crumb::presetOrder[crumb::numPresets - 1]), legacy.stepPreset(1);
    CHECK(legacy.presetName() == "old" && get(legacy, "bits") == 7 && get(legacy, "autoGain") == 0);
    old.deleteRecursively();
    // Unknown entries ignored (e.g. a future `link` / `rateR`), out-of-range clamped, missing and unparsable back to default.
    load(v1, blob(R"(<CRUMB stateVersion="7" future="x"><PARAM id="preamp" value="1e9"/><PARAM id="mix" value="abc"/>)"
                  R"(<PARAM id="output" value="nan"/><PARAM id="rateR" value="3"/><PARAM id="link" value="0"/><EXTRA/></CRUMB>)"));
    CHECK(get(v1, "preamp") == 48 && get(v1, "mix") == 100 && get(v1, "output") == 0 && get(v1, "bits") == 12 && get(v1, "rate") == 26040);
    for (auto* bad : { "12abc", "1-2", "5.5.5", "--3", "1e", ".", "1e2147483648", "-1e4294967296" }) // not wholly a (finite) number
    {
        load(v1, fixture);
        juce::String xml("<CRUMB>");
        for (auto& id : ids)
            xml << "<PARAM id=\"" << id << "\" value=\"" << bad << "\"/>";
        load(v1, blob(xml + "</CRUMB>"));
        CHECK(defaults(v1));
    }
    // Valid flat XML supports paired children, both quote styles and decoded user identities.
    load(v1, blob("<CRUMB preset='-1' presetName='Upper &amp; &#xC9;tude'><PARAM id='bits' value='7'></PARAM></CRUMB>"));
    CHECK(get(v1, "bits") == 7 && get(v1, "preamp") == 0 && v1.presetName() == juce::String::fromUTF8("Upper & \xc3\x89tude"));
    load(v1, blob("<CRUMB><EXTRA id='bits'/><PARAM id='bits' value='7'/></CRUMB>")); // an unknown entry hides nothing
    CHECK(get(v1, "bits") == 7);
    // Hostile or incomplete blobs (deep nesting, DTD entities, truncated references, foreign roots) leave the state alone.
    load(v1, fixture);
    load(v1, blob("<CRUMB>" + juce::String::repeatedString("<a>", 1000000)));
    load(v1, blob(R"(<!DOCTYPE CRUMB [<!ENTITY a "&a;&a;">]><CRUMB><PARAM id="preamp" value="&a;"/></CRUMB>)"));
    const auto text = [](const juce::MemoryBlock& m) { return std::string_view(static_cast<const char*>(m.getData()) + 8, m.getSize() - 8); };
    CHECK(crumb::isFlatCrumbXml(text(fixture)) && !crumb::isFlatCrumbXml(text(blob("<CRUMB>" + juce::String::repeatedString("<a>", 3)))));
    for (auto* bad : { "<CRUMB>&#x", "<CRUMB>&#", "<CRUMB a='&#x110000;'/>", "<CRUMB a='&#0;'/>", "<CRUMB a='&#12345678;'/>", "<CRUMB a='&;'/>", "<CRUMB><PARAM id='bits' value='&;'/></CRUMB>", "<CRUMB><PARAM id=\"bits\" value=\"&#x", "</CRUMB>", "<CRUMB><a></CRUMB>",
                       R"(<SNAP><PARAM id="bits" value="3"/></SNAP>)",
                       R"(<CRUMB><PARAM id="bits" value="3"/></BROKEN>)", R"(<CRUMB><PARAM id="bits" value="3'/></CRUMB>)",
                       R"(<CRUMB><PARAM id="bits" value="&#x"/></CRUMB>)", R"(<CRUMB><PARAM id="bits" value="&bad;"/></CRUMB>)",
                       R"(<CRUMB><PARAM id="bits" value="3" extra/></CRUMB>)", "<CRUMB/>trailing", "<CRUMB/><CRUMB/>",
                       "junk<CRUMB/>", "<CRUMB><a></b></CRUMB>", "<CRUMB x='1' x='2'/>" })
    {
        CHECK(!crumb::isFlatCrumbXml(bad));
        load(v1, blob(bad));
        CHECK(fixtureLoaded(v1));
    }

    // Garbage, empty, truncated and fuzzed data never crash and leave a fresh instance at its defaults.
    Proc g;
    juce::MemoryBlock good;
    v1.getStateInformation(good);
    g.setStateInformation(nullptr, 0);
    g.setStateInformation(good.getData(), 0);
    for (size_t cut = 1; cut < good.getSize() - 8; ++cut) // the last bytes only hold the closing tag
        g.setStateInformation(good.getData(), (int) cut);
    for (int n = 0; n < 2000; ++n)
    {
        juce::MemoryBlock junk((size_t) r.nextInt(400));
        r.fillBitsRandomly(junk.getData(), junk.getSize());
        if (n % 2 == 0 && junk.getSize() >= 8) // a valid header with a random payload
            std::memcpy(junk.getData(), good.getData(), 8);
        g.setStateInformation(junk.getData(), (int) junk.getSize());
    }
    CHECK(defaults(g));
    for (int n = 0; n < 5000; ++n) // a real state with random bytes overwritten: must not crash
    {
        juce::MemoryBlock m(good);
        for (int k = 0; k <= n % 4; ++k)
            static_cast<char*>(m.getData())[r.nextInt((int) m.getSize())] = (char) r.nextInt(256);
        g.setStateInformation(m.getData(), (int) m.getSize());
    }
}

// Clean settings: converter off, rate = host rate, no filters, preamp, jitter or dither -> the input, bit for bit.
static void testNull()
{
    for (int block : { 1, 64, 0 })
    {
        Proc p;
        set(p, { { "bits", 0 }, { "rate", 44100 }, { "approxOn", 0 }, { "imagesOn", 0 } });
        prepare(p, 44100, 4096, 2);
        const auto y = run(p, block, 2, 44100, [](int i) { return signal(i, 44100); });
        float diff = 0;
        for (size_t i = 0; i < y.size(); ++i)
            diff = std::max(diff, std::abs(y[i] - signal((int) i, 44100)));
        CHECK(diff < 1e-7f);
    }
}

// Power spectrum (dB re the 0.4 amplitude input sine) of channel 0 with a Blackman-Harris window, after the filters settle.
// Reference instead of the plug-in: 1 = a plain sample & hold at rate s[0], 2 = the plain clip at preamp s[0].
static std::vector<double> spectrum(const Settings& s, double freq, double fs = 44100, int reference = 0, double amp = 0.4, int block = 512)
{
    constexpr int size = 1 << 15, skip = 8192;
    const double f0 = std::round(freq * size / fs) * fs / size, w = 2 * juce::MathConstants<double>::pi * f0 / fs;
    std::vector<float> y;
    if (reference == 2)
        for (int i = 0; i < size + skip; ++i)
        {
            const double x = amp * std::sin(w * i) * std::pow(10.0, s[0].second / 20), a = std::abs(x);
            y.push_back((float) (a <= 0.5 ? x : std::copysign(0.5 + 0.5 * std::tanh((a - 0.5) / 0.5), x)));
        }
    else if (reference == 1)
        for (int i = 0; i < size + skip; ++i)
            y.push_back((float) (amp * std::sin(w * std::floor(i * (double) s[0].second / fs) * fs / s[0].second)));
    else
    {
        Proc p;
        set(p, "autoGain", 0), set(p, s);
        prepare(p, fs, block, 2);
        y = run(p, block, 2, size + skip, [w, amp](int i) { return (float) (amp * std::sin(w * i)); });
    }
    std::vector<std::complex<double>> x(size);
    for (int i = 0; i < size; ++i)
    {
        const double a = 2 * juce::MathConstants<double>::pi * i / size;
        x[(size_t) i] = y[(size_t) (i + skip)] * (0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a));
    }
    crumb::fft(x, false);
    std::vector<double> db(size / 2);
    const double top = std::pow(amp * 0.35875 * size / 2, 2);
    for (int k = 0; k < size / 2; ++k)
        db[(size_t) k] = 10 * std::log10(std::norm(x[(size_t) k]) / top + 1e-30);
    return db;
}
// Strongest bin outside the listed tones (+-8 bins), and the level at one tone.
static double spur(const std::vector<double>& db, const std::vector<double>& tones, double fs = 44100)
{
    double m = -400;
    for (size_t k = 8; k < db.size(); ++k)
    {
        bool near = false;
        for (double t : tones)
            near = near || std::abs((double) k - t * 2 * (double) db.size() / fs) <= 8;
        m = near ? m : std::max(m, db[k]);
    }
    return m;
}
static double at(const std::vector<double>& db, double hz, double fs = 44100)
{
    const size_t k = (size_t) std::round(hz * 2 * (double) db.size() / fs);
    return *std::max_element(db.begin() + (long) k - 3, db.begin() + (long) k + 4);
}

// The resampler adds no aliasing of its own: only the emulated fold-down at the virtual rate appears.
static void testAlias()
{
    const float fr = 26040;
    for (float approx : { 0.0f, 1.0f }) // 1 kHz: every emulated image (fr +- 1k, ...) lies above the host Nyquist
    {
        const double floorDb = spur(spectrum({ { "rate", fr }, { "bits", 0 }, { "approxOn", approx } }, 1000), { 1000 });
        std::cout << "alias floor, 1 kHz at fr 26.04 kHz, approx " << approx << ": " << floorDb << " dB\n";
        CHECK(floorDb < -100);
    }
    const double naiveDb = spur(spectrum({ { "rate", fr } }, 1000, 44100, 1), { 1000 });
    std::cout << "same signal through a naive sample & hold: " << naiveDb << " dB\n";
    CHECK(naiveDb > -60);
    // 10 kHz: the emulated image at fr - 10k = 16.04 kHz is there, nothing else.
    const auto ten = spectrum({ { "rate", fr }, { "bits", 0 }, { "approxOn", 0 } }, 10000);
    std::cout << "10 kHz: image at 16.04 kHz " << at(ten, 16040) << " dB, other spurs " << spur(ten, { 10000, 16040 }) << " dB\n";
    CHECK(at(ten, 16040) > -20 && spur(ten, { 10000, 16040 }) < -95);
    // 20 kHz, above Nyquist(fr): folds to 6.04 kHz without the approx filter, which suppresses it. (Off, the fold is
    // already ~10 dB down: 20 kHz sits in the transition band of the converter's causal input kernel.)
    const double off = at(spectrum({ { "rate", fr }, { "bits", 0 }, { "approxOn", 0 } }, 20000), 6040);
    const double on = at(spectrum({ { "rate", fr }, { "bits", 0 }, { "approxOn", 1 }, { "output", 0 } }, 20000), 6040);
    std::cout << "20 kHz fold-down to 6.04 kHz: approx off " << off << " dB, on " << on << " dB \n";
    CHECK(off > -20 && on < off - 60);
    // Images filter at shift 0 removes most of the 16.04 kHz image.
    CHECK(at(spectrum({ { "rate", fr }, { "bits", 0 }, { "approxOn", 0 }, { "imagesOn", 1 } }, 10000), 16040) < at(ten, 16040) - 6);
}

static void testConverterPath()
{
    for (float sign : { -1.0f, 1.0f })
    {
        Proc p;
        set(p, { { "preamp", 48 }, { "bits", 0 }, { "rate", 44100 }, { "approxOn", 0 }, { "autoGain", 0 } });
        prepare(p, 44100, 512, 1);
        const auto y = run(p, 512, 1, 2048, [sign](int i) { return sign * (i % 2 ? 0.2f : 0.4f); });
        for (size_t i = 1; i < y.size(); ++i) CHECK(sign * y[i] > 0.999f && std::abs(y[i]) <= 1.0f);
    }
    for (float input : { -0.8f, -0.5f, -0.1f, 0.1f, 0.5f, 0.8f })
    {
        Proc p;
        set(p, { { "bits", 0 }, { "rate", 44100 }, { "approxOn", 0 } });
        prepare(p, 44100, 512, 1);
        const double a = std::abs(input), expected = a <= 0.5 ? input : std::copysign(0.5 + 0.5 * std::tanh((a - 0.5) / 0.5), input);
        CHECK(std::abs(run(p, 512, 1, 4096, [input](int) { return input; }).back() - expected) < 1e-7);
    }
    // ADAA beats the plain clip at every drive, just past the knee (peaks 0.55, 0.7) as well as hard (12 dB).
    constexpr double fs = 44100, n = 32768;
    const double f0 = std::round(10000 * n / fs) * fs / n;
    for (auto [preamp, hz] : { std::pair { 2.8f, 13000.0 }, { 4.9f, 7000.0 }, { 12.0f, 10000.0 } })
    {
        const Settings drive { { "preamp", preamp }, { "rate", 44100 }, { "bits", 0 }, { "approxOn", 0 } };
        std::vector<double> harmonics; // the true odd harmonics, below Nyquist
        for (double h = std::round(hz * n / fs) * fs / n, k = 1; h * k < fs / 2; k += 2)
            harmonics.push_back(h * k);
        const double adaa = spur(spectrum(drive, hz), harmonics), naive = spur(spectrum(drive, hz, fs, 2), harmonics);
        std::cout << "preamp " << preamp << " dB, " << hz << " Hz: worst alias ADAA " << adaa << " dB, plain clip " << naive << " dB\n";
        CHECK(adaa < naive - 6);
    }
    // Preamp 0, a sine just over the knee: the clip stays engaged across knee crossings at every block size (no
    // block-peak decision modulating between the clip paths). Plain clip reference about -83 dB.
    std::vector<double> odd;
    for (double h = std::round(13000 * n / fs) * fs / n, k = 1; h * k < fs / 2; k += 2)
        odd.push_back(h * k);
    const Settings knee { { "preamp", 0 }, { "rate", 44100 }, { "bits", 0 }, { "approxOn", 0 } };
    const double plain = spur(spectrum(knee, 13000, fs, 2, 0.55), odd);
    for (int block : { 1, 2, 4, 16, 512 })
    {
        const double adaa = spur(spectrum(knee, 13000, fs, 0, 0.55, block), odd);
        std::cout << "preamp 0, peak 0.55, 13 kHz, block " << block << ": worst alias " << adaa << " dB, plain clip " << plain << " dB\n";
        CHECK(adaa < plain - 6);
    }
    // The DAC ladder errors: 4-bit codes +2 and -2 sit 0.3 and 0.5 LSB (of 1/8) off the ideal levels 0.25 and -0.25.
    for (float v : { 0.3f, -0.3f })
    {
        Proc p;
        set(p, { { "bits", 4 }, { "dcShift", 1 }, { "rate", 44100 }, { "approxOn", 0 } });
        prepare(p, 44100, 512, 1);
        CHECK(std::abs(run(p, 512, 1, 64, [v](int) { return v; })[0] - (v > 0 ? 0.25 + 0.3 / 8 : -0.25 + 0.5 / 8)) < 1e-4);
    }

    // With quantization and dither active, every deviation maps to the designed 8th-order TPT cutoff.
    const double harmonic = 3 * std::round(5000 * n / fs) * fs / n;
    const Settings quant { { "rate", 44100 }, { "bits", 2 }, { "dcShift", 1 }, { "dither", 100 }, { "approxOn", 0 } };
    const double unfiltered = at(spectrum(quant, 5000), harmonic);
    for (float dev : { -100.0f, -50.0f, 0.0f, 50.0f, 100.0f })
    {
        auto settings = quant;
        settings.insert(settings.end(), { { "approxOn", 1 }, { "approxDev", dev } });
        const double cutoff = std::min(0.49 * fs, 0.5 * fs * std::exp2(dev / 100));
        const double ratio = std::tan(juce::MathConstants<double>::pi * harmonic / fs) / std::tan(juce::MathConstants<double>::pi * cutoff / fs);
        const double expected = -10 * std::log10(1 + std::pow(ratio, 16));
        const double attenuation = at(spectrum(settings, 5000), harmonic) - unfiltered;
        std::cout << "quantizer harmonic approx dev " << dev << ": " << attenuation << " dB (expected " << expected << ")\n";
        CHECK(std::abs(attenuation - expected) < 1.0);
    }
    // Images Shift endpoint and intermediate cutoffs follow the 4th-order mapping on the complete chain.
    const Settings resample { { "rate", 22050 }, { "bits", 8 }, { "dither", 100 }, { "approxOn", 0 } };
    const double image = 22050 - f0, base = at(spectrum(resample, 10000), image);
    for (float shift : { 0.0f, 25.0f, 50.0f, 100.0f })
    {
        auto settings = resample;
        settings.insert(settings.end(), { { "imagesOn", 1 }, { "imagesShift", shift } });
        const double cutoff = std::min(0.49 * fs, 11025.0 * std::exp2(2 * shift / 100));
        const double ratio = std::tan(juce::MathConstants<double>::pi * image / fs) / std::tan(juce::MathConstants<double>::pi * cutoff / fs);
        CHECK(std::abs(at(spectrum(settings, 10000), image) - base + 10 * std::log10(1 + std::pow(ratio, 8))) < 0.2);
    }
}

static void testSound()
{
    // Quantizer: 4-bit mid-tread passes silence as silence; mid-riser idles at +-LSB/2 whose DC the DAC stage removes.
    for (float tread : { 0.0f, 1.0f })
    {
        Proc p;
        set(p, { { "bits", 4 }, { "dcShift", tread }, { "rate", 44100 }, { "approxOn", 0 } });
        prepare(p, 44100, 512, 2);
        const auto y = run(p, 512, 2, 44100, [](int) { return 0.0f; });
        CHECK(std::abs(y.back()) < 1e-4f);
        CHECK(tread == 0 ? std::abs(y[1]) > 0.05f : y[1] == 0);
    }
    // Bits reduce resolution: 8-bit error is far above 16-bit error.
    auto error = [](float bits) {
        Proc p;
        set(p, { { "bits", bits }, { "dcShift", 1 }, { "rate", 44100 }, { "approxOn", 0 } });
        prepare(p, 44100, 512, 1);
        const auto y = run(p, 512, 1, 22050, [](int i) { return signal(i, 44100); });
        double e = 0;
        for (size_t i = 0; i < y.size(); ++i)
            e += std::pow(y[i] - signal((int) i, 44100), 2);
        return e;
    };
    CHECK(error(8) > 100 * error(16));
    // Preamp drives the ADC clip: output stays within +-1 at +48 dB, mix 0 and bypass give the dry signal.
    Proc p;
    set(p, { { "preamp", 48 }, { "bits", 0 }, { "rate", 44100 }, { "approxOn", 0 }, { "autoGain", 0 } });
    prepare(p, 44100, 512, 2);
    float m = 0;
    for (float y : run(p, 512, 2, 22050, [](int i) { return signal(i, 44100); }))
        m = std::max(m, std::abs(y));
    CHECK(m <= 1.0f && m > 0.9f);
    for (auto* id : { "mix", "bypass" })
    {
        Proc q;
        set(q, { { "preamp", 30 }, { "bits", 3 }, { "rate", 2000 }, { id, id[0] == 'm' ? 0.0f : 1.0f } });
        prepare(q, 44100, 512, 2);
        const auto y = run(q, 512, 2, 4410, [](int i) { return signal(i, 44100); });
        float diff = 0;
        for (size_t i = 0; i < y.size(); ++i)
            diff = std::max(diff, std::abs(y[i] - signal((int) i, 44100)));
        CHECK(diff < 1e-6f);
    }
    // Auto gain: a -30 dBFS 1 kHz sine, bits off (the clip stays linear up to +24 dB: peak 0.5, its knee). At +12 dB
    // preamp with auto gain on the output RMS stays within 1 dB of preamp 0 (off: 12 dB up). Toggling it on, jumping the
    // preamp to +24 dB and toggling it off are smoothed: on a 20 Hz sine (largest own step 0.0013 at +24 dB, off) no
    // sample step exceeds 0.003 (unsmoothed, the toggles would step by ~0.1 and ~0.45).
    const auto sine = [](int i) { return (float) (0.0316 * std::sin(2 * juce::MathConstants<double>::pi * 1000 * i / 48000)); };
    const auto rms = [&](float preamp, float on) {
        Proc q;
        set(q, { { "preamp", preamp }, { "bits", 0 }, { "autoGain", on } });
        prepare(q, 48000, 512, 2);
        const auto y = run(q, 512, 2, 48000, sine);
        double e = 0;
        for (size_t i = 24000; i < y.size(); ++i)
            e += y[i] * y[i];
        return 10 * std::log10(e / 24000);
    };
    const double flat = rms(0, 1), compensated = rms(12, 1), off = rms(12, 0);
    std::cout << "auto gain, -30 dBFS 1 kHz: preamp 0 " << flat << " dB, +12 on " << compensated << " dB, +12 off " << off << " dB\n";
    CHECK(std::abs(compensated - flat) < 1 && std::abs(off - flat - 12) < 1);
    const auto low = [](int i) { return (float) (0.0316 * std::sin(2 * juce::MathConstants<double>::pi * 20 * i / 48000)); };
    Proc q;
    set(q, { { "preamp", 12 }, { "bits", 0 }, { "autoGain", 0 } });
    prepare(q, 48000, 512, 2);
    auto y = run(q, 512, 2, 4800, low);
    for (auto [id, v] : { std::pair { "autoGain", 1.0f }, { "preamp", 24.0f }, { "autoGain", 0.0f } })
    {
        set(q, id, v);
        for (float s : run(q, 512, 2, 4800, [&](int i) { return low((int) y.size() + i); }))
            y.push_back(s);
    }
    float step = 0;
    for (size_t i = 1; i < y.size(); ++i)
        step = std::max(step, std::abs(y[i] - y[i - 1]));
    std::cout << "auto gain transitions: largest step " << step << "\n";
    CHECK(step < 0.003f);
    // Auto gain only ever moves the wet path: preset 24 with it on keeps its dry path at its output gain (mix 0: the input
    // exactly), and its blend is no louder than with auto gain off.
    const auto preset = [&](float on, float mix) {
        Proc r;
        r.setCurrentProgram(24), set(r, { { "autoGain", on }, { "output", 0 }, { "mix", 35 } });
        if (mix >= 0) set(r, "mix", mix);
        prepare(r, 48000, 512, 2);
        return run(r, 512, 2, 48000, sine);
    };
    const auto dry = preset(1, 0);
    float worst = 0;
    for (size_t i = 0; i < dry.size(); ++i)
        worst = std::max(worst, std::abs(dry[i] - sine((int) i)));
    const auto level = [](const std::vector<float>& v) { double e = 0; for (size_t i = v.size() / 2; i < v.size(); ++i) e += v[i] * v[i]; return 10 * std::log10(e / (double) (v.size() / 2)); };
    const double blendOn = level(preset(1, -1)), blendOff = level(preset(0, -1));
    std::cout << "preset 24, -30 dBFS 1 kHz: auto on " << blendOn << " dB, off " << blendOff << " dB\n";
    CHECK(worst < 1e-6f && blendOn <= blendOff + 0.1);
}

static const std::vector<Settings> extremes {
    { { "preamp", 48 }, { "bits", 1 }, { "dither", 100 }, { "rate", 44 }, { "jitter", 100 }, { "approxDev", -100 }, { "imagesOn", 1 },
      { "imagesShift", 100 }, { "fType", 1 }, { "fOrder", 0 }, { "cutoff", 20 }, { "reso", 100 }, { "output", 12 } },
    { { "bits", 24 }, { "dcShift", 1 }, { "rate", 44100 }, { "jitter", 100 }, { "approxDev", 100 }, { "fType", 2 }, { "cutoff", 20000 }, { "reso", 100 } },
    { { "bits", 4 }, { "rate", 3000 }, { "approxOn", 0 }, { "imagesOn", 1 }, { "fType", 3 }, { "cutoff", 1000 }, { "reso", 100 }, { "output", 12 } },
    { { "preamp", 24 }, { "bits", 0 }, { "rate", 11025 }, { "fType", 4 }, { "reso", 100 }, { "mix", 50 }, { "output", -24 } },
    { { "loss", 100 }, { "speed", 0 }, { "drop", 100 }, { "fill", 1 }, { "jitter", 100 }, { "stereo", 1 }, { "lowCut", 20000 }, { "highCut", 20 }, { "slope", 7 }, { "reso", 100 } },
    { { "loss", 70 }, { "speed", 100 }, { "drop", 40 }, { "keep", 1 }, { "ear", 1 }, { "stereo", 2 }, { "gate", -30 }, { "lowCut", 20000 }, { "highCut", 20000 }, { "slope", 0 }, { "invert", 1 }, { "fOrder", 0 }, { "reso", 100 }, { "output", 12 } },
};

static void testBounded()
{
    for (int samples : { 0, 512 }) // hosts may supply an empty buffer even with an enabled input bus
    {
        Proc p;
        prepare(p, 48000, 512, 2);
        juce::AudioBuffer<float> empty(0, samples);
        juce::MidiBuffer midi;
        p.processBlock(empty, midi); // sanitizers check that no channel pointer is formed or dereferenced
        CHECK(p.scope.write.load() == (uint32_t) samples);
    }
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int ch : { 1, 2 })
            for (int block : { 1, 7, 64, 511, 4096, 0 })
                for (auto& s : extremes)
                {
                    Proc p;
                    set(p, s);
                    prepare(p, fs, block == 0 ? 4096 : block, ch);
                    bool ok = true;
                    for (float y : run(p, block, ch, (int) (fs * (block == 1 ? 0.05 : 0.2)), [fs](int i) { return signal(i, fs) * 2; }, true))
                        ok = ok && std::isfinite(y) && std::abs(y) < 2000;
                    // Silence afterwards: bounded, finite, never denormal.
                    for (float y : run(p, block, ch, (int) (fs * 0.3), [](int) { return 0.0f; }))
                        ok = ok && std::isfinite(y) && std::abs(y) < 2000 && std::fpclassify(y) != FP_SUBNORMAL;
                    CHECK(ok);
                }
    for (double fs : { 0.0, -48000.0, 1.0e300, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
    {
        Proc q; // a broken host's sample rate must not break anything (UBSan: no invalid casts)
        set(q, extremes[0]);
        prepare(q, fs, 64, 2);
        bool ok = true;
        for (float y : run(q, 64, 2, 4800, [](int i) { return signal(i, 48000); }))
            ok = ok && std::isfinite(y) && std::abs(y) < 2000;
        CHECK(ok);
    }
    Proc p; // repeated prepare / release with changing rates, block sizes and moving parameters
    for (int n = 0; n < 24; ++n)
    {
        const double fs = std::array<double, 4> { 44100, 48000, 96000, 192000 }[(size_t) n % 4];
        set(p, extremes[(size_t) n % extremes.size()]);
        prepare(p, fs, 4096, 1 + n % 2);
        for (float y : run(p, 1 + (n * 397) % 4096, 1 + n % 2, 9000, [fs](int i) { return signal(i, fs); }))
            CHECK(std::isfinite(y));
        if (n % 3 == 0)
            p.releaseResources();
    }
}

static void testAllocations()
{
    // No allocation inside processBlock: in every run so far, and below while parameters move. The counter itself is
    // checked; the probes call the replaceable functions directly (never elided) and escape every pointer.
    CHECK(audioAllocations.exchange(0) == 0);
    auto counted = [](auto allocate) {
        onAudioThread = true;
        allocate();
        onAudioThread = false;
        return audioAllocations.exchange(0) > 0;
    };
    CHECK(counted([] { void* q = ::operator new(sizeof(int)); escape(q); ::operator delete(q); }));
    CHECK(counted([] { void* q = ::operator new(64, std::align_val_t(64)); escape(q); ::operator delete(q, std::align_val_t(64)); }));
#ifdef CRUMB_COUNTS_MALLOC
    CHECK(counted([] { void* q = std::malloc(64); escape(q); std::free(q); }));
    CHECK(counted([] { juce::AudioBuffer<float> b(2, 512); escape(b.getWritePointer(0)); })); // JUCE's malloc buffers
#endif
    Proc p;
    prepare(p, 48000, 512, 2);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    juce::Random r(9);
    for (int n = 0; n < 100; ++n)
    {
        for (auto* param : p.getParameters())
            param->setValueNotifyingHost(r.nextFloat());
        for (int i = 0; i < 512; ++i)
            buffer.setSample(0, i, signal(i, 48000)), buffer.setSample(1, i, -signal(i, 48000));
        onAudioThread = true;
        p.processBlock(buffer, midi);
        onAudioThread = false;
    }
    CHECK(audioAllocations.load() == 0);
}

static void testEditor()
{
    Proc p;
    prepare(p, 48000, 128, 2);
    std::atomic<bool> stop { false };
    std::thread audio([&] {
        juce::AudioBuffer<float> b(2, 128);
        juce::MidiBuffer midi;
        for (int n = 0; !stop; ++n)
        {
            for (int i = 0; i < 128; ++i)
                b.setSample(0, i, signal(n * 128 + i, 48000)), b.setSample(1, i, signal(n * 128 + i, 48000));
            p.processBlock(b, midi);
        }
    });
    for (int n = 0; n < 10; ++n)
    {
        std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
        CHECK(e != nullptr && e->getWidth() > 0 && e->getHeight() > 0);
        set(p, "bits", (float) (n * 2));
        CHECK(e->createComponentSnapshot(e->getLocalBounds()).isValid());
    }
    stop = true;
    audio.join();
}

static juce::Component* find(juce::Component& c, const juce::String& id) // by component ID, depth first
{
    if (c.getComponentID() == id)
        return &c;
    for (auto* child : c.getChildren())
        if (auto* found = find(*child, id))
            return found;
    return nullptr;
}
static std::vector<float> params(Proc& p)
{
    std::vector<float> v;
    for (auto* param : p.getParameters())
        v.push_back(param->getValue());
    return v;
}

// Forced overlap: mid-transaction (from a parameter listener) another thread saves and reads the A/B slot and the
// preset name. It must not wait for the writer, and it must get the complete state committed before the transaction.
static void testTransactions()
{
    for (int mode = 0; mode < 3; ++mode)
    {
        Proc p;
        juce::MemoryBlock other;
        p.setCurrentProgram(1), p.selectAb(1), p.setCurrentProgram(23), p.getStateInformation(other), p.selectAb(0);
        struct Listener : juce::AudioProcessorParameter::Listener
        {
            Proc& p;
            juce::MemoryBlock before;
            const juce::String name = p.presetName();
            std::future<bool> reader; // destroyed after the transaction: a blocked reader fails the test, no hang
            bool ok = false;
            int calls = 0;
            explicit Listener(Proc& owner) : p(owner) { p.getStateInformation(before); }
            void parameterValueChanged(int, float) override
            {
                if (calls++ > 0)
                    return;
                reader = std::async(std::launch::async, [this] {
                    juce::MemoryBlock m;
                    p.getStateInformation(m);
                    return m == before && p.abSlot() == 0 && p.presetName() == name;
                });
                ok = reader.wait_for(std::chrono::seconds(10)) == std::future_status::ready && reader.get();
            }
            void parameterGestureChanged(int, bool) override {}
        } listener(p);
        p.apvts.getParameter("preamp")->addListener(&listener);
        if (mode == 0) p.setCurrentProgram(7);
        else if (mode == 1) load(p, other);
        else p.selectAb(1);
        p.apvts.getParameter("preamp")->removeListener(&listener);
        juce::MemoryBlock after;
        p.getStateInformation(after);
        CHECK(listener.ok && listener.calls > 0 && after != listener.before);
    }
}

static void testRestoreAudio()
{
    Proc source;
    source.setCurrentProgram(23);
    set(source, "dither", 100);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    for (int block : { 1, 256, 4096 })
    {
        Proc a, b;
        load(a, state), prepare(a, 48000, block, 2);
        prepare(b, 48000, block, 2), load(b, state);
        const auto reference = run(a, block, 2, 96000, [](int i) { return signal(i, 48000); });
        auto y = run(b, block, 2, 48000, [](int i) { return signal(i, 48000); });
        load(b, state); // mid-playback restore of the same state: no reset, playback carries on unchanged
        for (float v : run(b, block, 2, 48000, [](int i) { return signal(48000 + i, 48000); }))
            y.push_back(v);
        CHECK(reference == y);
        CHECK(a.getLatencySamples() == 0 && b.getLatencySamples() == 0);
    }
}

static void testPresets()
{
    Proc p;
    CHECK(p.getNumPrograms() == 41 && p.getCurrentProgram() == 0 && p.presetName() == "init" && defaults(p)); // init = defaults
    juce::StringArray names;
    for (int i = 0; i < p.getNumPrograms(); ++i)
    {
        names.addIfNotAlreadyThere(p.getProgramName(i));
        set(p, { { "autoGain", (float) (i % 2) }, { "auto", 40 } }), p.setCurrentProgram(i); // a preset turns auto gain off
        CHECK(p.getCurrentProgram() == i && p.presetName() == p.getProgramName(i) && get(p, "autoGain") == 0);
        for (int k = 0; k < 16; ++k) // every value is on its parameter's grid, so it is stored exactly
        {
            const float v = crumb::presets[i].v[k];
            CHECK(std::abs(get(p, ids[k]) - v) < 1e-3f * std::max(1.0f, std::abs(v)));
        }
        for (int k = 0; k < 12; ++k)
        {
            const float v = crumb::presets[i].x[k];
            CHECK(std::abs(get(p, ids[18 + k]) - v) < 1e-3f * std::max(1.0f, std::abs(v)));
            if (i < 25) CHECK(p.apvts.getParameter(ids[18 + k])->getValue() == p.apvts.getParameter(ids[18 + k])->getDefaultValue()); // 0.2's presets leave 0.3's neutral
        }
        CHECK(get(p, "auto") == 100);
        prepare(p, 48000, 512, 2);
        CHECK(p.getLatencySamples() == (get(p, "loss") > 0 || get(p, "drop") > 0 ? 2048 : 0));
        const auto y = run(p, 512, 2, 48000, [](int n) { return 1.5f * signal(n, 48000); });
        float peak = 0;
        for (float v : y)
            CHECK(std::isfinite(v)), peak = std::max(peak, std::abs(v));
        CHECK(peak > 1e-3f && peak < 4);
    }
    CHECK(names.size() == 41);
    juce::SortedSet<int> order;
    for (int i : crumb::presetOrder) order.add(i);
    CHECK(order.size() == 41 && crumb::presetOrder[0] == 0);
    CHECK(p.getProgramName(28) == "56k stream");
    p.setCurrentProgram(0);
    for (int k = 1; k <= 41; ++k) // the arrows walk the whole list and wrap
        p.stepPreset(1), CHECK(p.getCurrentProgram() == crumb::presetOrder[k % 41]);
    p.stepPreset(-1), CHECK(p.getCurrentProgram() == crumb::presetOrder[40]);

    // A/B: switching copies the full parameter state and the preset each way.
    Proc a;
    a.setCurrentProgram(3);
    const auto three = params(a);
    a.selectAb(1); // B starts as a copy of A
    CHECK(a.abSlot() == 1 && params(a) == three && a.getCurrentProgram() == 3);
    a.setCurrentProgram(7);
    set(a, "jitter", 33);
    const auto b = params(a);
    a.selectAb(0);
    CHECK(a.abSlot() == 0 && params(a) == three && a.getCurrentProgram() == 3);
    a.selectAb(1);
    CHECK(params(a) == b && a.getCurrentProgram() == 7);
    // A/B, the preset and the editor size survive save -> reload into a fresh instance; A/B still works there.
    a.editorWidth = 1440;
    juce::MemoryBlock m;
    a.getStateInformation(m);
    Proc c;
    load(c, m);
    CHECK(c.abSlot() == 1 && params(c) == b && c.getCurrentProgram() == 7 && c.editorWidth == 1440);
    c.selectAb(0);
    CHECK(params(c) == three && c.getCurrentProgram() == 3);
    juce::MemoryBlock m2;
    load(c, m), c.getStateInformation(m2);
    CHECK(m == m2);

    // User presets: "save" writes an XML file, prev/next reach it after the factory list.
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("crumb-presets", "", false);
    Proc u(folder);
    CHECK(u.presetFolder() == folder && !folder.exists());
    u.setCurrentProgram(5);
    set(u, "cutoff", 777);
    const auto mine = params(u);
    CHECK(u.saveUserPreset().existsAsFile() && u.presetName() == "user 01" && u.saveUserPreset().getFileName() == "user 02.xml");
    u.setCurrentProgram(24);
    u.stepPreset(1);
    CHECK(u.userPresets().size() == 2);
    CHECK(u.presetName() == "user 01" && params(u) == mine && u.preset == -1);
    u.stepPreset(1), u.stepPreset(1);
    CHECK(u.presetName() == "init" && defaults(u));
    u.stepPreset(-1);
    CHECK(u.presetName() == "user 02");
    // Inactive A's user identity survives state reload, XML escaping and Unicode filename text.
    const auto renamed = folder.getChildFile(juce::String::fromUTF8("Upper & \xc3\x89tude.xml"));
    CHECK(folder.getChildFile("user 02.xml").moveFileTo(renamed));
    u.setCurrentProgram(24), u.stepPreset(1);
    CHECK(u.presetName() == renamed.getFileNameWithoutExtension() && params(u) == mine);
    u.selectAb(1), u.setCurrentProgram(5);
    juce::MemoryBlock userState;
    u.getStateInformation(userState);
    Proc restored(folder);
    load(restored, userState);
    CHECK(restored.abSlot() == 1 && restored.getCurrentProgram() == 5);
    restored.selectAb(0);
    CHECK(restored.presetName() == renamed.getFileNameWithoutExtension() && params(restored) == mine);
    restored.getStateInformation(userState);
    load(restored, userState);
    CHECK(restored.presetName() == renamed.getFileNameWithoutExtension());
    restored.stepPreset(1);
    CHECK(restored.presetName() == "user 01");
    CHECK(folder.deleteRecursively());

    // A full numbering range must fail without replacing any existing file or changing the preset identity.
    CHECK(folder.createDirectory().wasOk());
    for (int k = 1; k < 1000; ++k)
        CHECK(folder.getChildFile("user " + juce::String(k).paddedLeft('0', 2) + ".xml").replaceWithText("keep " + juce::String(k)));
    const auto name = u.presetName();
    const auto beforeFull = params(u);
    const int index = u.preset;
    CHECK(u.saveUserPreset() == juce::File() && u.presetName() == name && u.preset == index && params(u) == beforeFull);
    for (int k = 1; k < 1000; ++k)
        CHECK(folder.getChildFile("user " + juce::String(k).paddedLeft('0', 2) + ".xml").loadFileAsString() == "keep " + juce::String(k));
    CHECK(folder.deleteRecursively());
}

static void testScopeFifo()
{
    // Best-effort display feed: split atomic samples may tear during concurrent overwrites; no allocations.
    auto scope = std::make_unique<crumb::Scope>();
    std::atomic<bool> stop { false };
    std::thread writer([&] {
        onAudioThread = true;
        for (uint32_t w = UINT32_MAX - 127; !stop; w += 64)
        {
            for (uint32_t i = 0; i < 64; ++i)
                scope->push(w + i, (float) ((w + i) % 1000), -(float) ((w + i) % 1000), 2, (uint8_t) ((w + i) % 3));
            scope->write.store(w + 64, std::memory_order_release);
        }
        onAudioThread = false;
    });
    int torn = 0;
    for (int n = 0; n < 2000; ++n)
        for (uint32_t i = 0; i < 4096; ++i)
        {
            const auto at = (scope->write.load(std::memory_order_acquire) - 4096 + i) & (crumb::Scope::size - 1);
            torn += scope->dry[at].load() != -scope->wet[at].load() ? 1 : 0;
        }
    stop = true;
    writer.join();
    CHECK(audioAllocations.exchange(0) == 0);
    CHECK(torn < 2000 * 8);
    // What the feed holds is what the plug-in did: the input, the very samples of the output, and (codec running) the
    // input lined up with the output, what the codec took away and which frames it lost. Across the index wrap too.
    for (uint32_t start : { uint32_t(INT32_MAX - 127), UINT32_MAX - 127 })
    {
        Proc p;
        prepare(p, 48000, 512, 1);
        p.scope.write = start;
        const auto y = run(p, 512, 1, 512, [](int i) { return (float) i / 2048; });
        CHECK(p.scope.write.load() == start + 512u);
        for (uint32_t i = 0; i < 512; ++i)
        {
            const auto at = (start + i) & (crumb::Scope::size - 1);
            CHECK(p.scope.dry[at].load() == (float) i / 2048 && p.scope.wet[at].load() == y[i] && p.scope.gone[at].load() == 0 && p.scope.mark[at].load() == 0);
        }
    }
    Proc p;
    set(p, { { "bits", 0 }, { "rate", 44100 }, { "approxOn", 0 }, { "loss", 60 }, { "drop", 40 }, { "fill", 1 }, { "autoGain", 0 } });
    prepare(p, 44100, 512, 1);
    const auto y = run(p, 512, 1, 16384, [](int i) { return signal(i, 44100); });
    int held = 0, dryWrong = 0, wetWrong = 0, sumWrong = 0;
    for (uint32_t i = 2048; i < 16384; ++i)
    {
        dryWrong += p.scope.dry[i].load() != signal((int) i - 2048, 44100), wetWrong += p.scope.wet[i].load() != y[i];
        sumWrong += std::abs(p.scope.wet[i].load() + p.scope.gone[i].load() - p.scope.dry[i].load()) > 1e-5f; // output + taken = input
        held += p.scope.mark[i].load() == 2;
    }
    CHECK(dryWrong == 0 && wetWrong == 0 && sumWrong == 0 && held > 2000 && held < 12000);
}

// The clip feed carries what really clipped: nothing under full scale, the dB the drive's clip took off each sample past it
// (signed, the louder side's, late with the codec), the dB each output sample is past 0 dBFS; none while it is not heard.
static void testClipFeed()
{
    const auto ramp = [](int i) { return (float) (0.9 * std::sin(i * 0.02)); };
    const auto feed = [&](const Settings& s, int ch, int n = 4096) {
        auto p = std::make_unique<Proc>();
        set(*p, cleanCrush), set(*p, s);
        prepare(*p, 48000, 512, ch);
        run(*p, 512, ch, n, ramp);
        return p;
    };
    const auto wrong = [&](Proc& p, double drive, int late, int from, int n) {
        int bad = 0, hits = 0;
        for (int i = from; i < n; ++i)
        {
            const double a = i < late ? 0.0 : (double) ramp(i - late) * drive, want = std::abs(a) > 1 ? 20 * std::log10(std::abs(a) / crumb::clip(std::abs(a))) : 0.0;
            const float is = p.scope.cut[(size_t) i].load();
            bad += std::abs(std::abs(is) - want) > 2e-4 || (want > 0 && (is > 0) != (a > 0)) ? 1 : 0, hits += is != 0 ? 1 : 0;
        }
        return std::pair { bad, hits };
    };
    CHECK(wrong(*feed({}, 1), 1, 0, 0, 4096) == std::pair(0, 0)); // 0.9: in the knee, under full scale
    const auto driven = feed({ { "preamp", 12 } }, 2);
    const auto [bad, hits] = wrong(*driven, juce::Decibels::decibelsToGain(12.0), 0, 0, 4096);
    CHECK(bad == 0 && hits > 2000);
    const auto coded = feed({ { "preamp", 12 }, { "loss", 30 } }, 1, 12288); // the codec's latency later
    const auto [bad2, hits2] = wrong(*coded, juce::Decibels::decibelsToGain(12.0), 2048, 2048, 12288);
    CHECK(bad2 == 0 && hits2 > 4000);
    for (auto& s : { Settings { { "preamp", 12 }, { "bypass", 1 } }, Settings { { "preamp", 12 }, { "mix", 0 } }, Settings { { "preamp", 12 }, { "gate", 0 } } })
        CHECK(wrong(*feed(s, 2, 12288), 0, 0, 8192, 12288).first == 0);
    // the output: exactly the samples past 0 dBFS
    for (float output : { 0.0f, 6.0f })
    {
        Proc p;
        set(p, cleanCrush), set(p, "output", output);
        prepare(p, 48000, 512, 1);
        const auto y = run(p, 512, 1, 4096, [&](int i) { return 0.5f * ramp(i) / 0.45f; });
        int hot = 0;
        for (size_t i = 0; i < y.size(); ++i)
        {
            const float is = p.scope.over[i].load();
            CHECK(std::abs(y[i]) > 1 ? std::abs(std::abs(is) - 20 * std::log10(std::abs(y[i]))) < 1e-5f && (is > 0) == (y[i] > 0) : is == 0);
            hot += is != 0;
        }
        CHECK(output == 0 ? hot == 0 : hot > 1000);
    }
    // the louder side is the one that counts
    Proc p;
    set(p, cleanCrush), set(p, { { "output", 12 }, { "preamp", 6 } });
    prepare(p, 48000, 64, 2);
    juce::AudioBuffer<float> b(2, 64);
    juce::MidiBuffer midi;
    for (int k = 0; k < 60; ++k)
    {
        b.clear();
        for (int i = 0; i < 64; ++i) b.setSample(1, i, -0.9f);
        onAudioThread = true, p.processBlock(b, midi), onAudioThread = false;
    }
    CHECK(audioAllocations.exchange(0) == 0);
    const auto last = (size_t) (p.scope.write.load() - 1);
    CHECK(std::abs(p.scope.cut[last].load() + 20 * std::log10(1.796 / crumb::clip(1.796))) < 0.01 && p.scope.over[last].load() < -11);
}

// ------------------------------------------------------------------------------------------------ 0.3.0: codec, band, gate
static double energy(const std::vector<float>& y, size_t from, size_t to) { double e = 0; for (size_t i = from; i < to; ++i) e += (double) y[i] * y[i]; return e; }

static void testCodec()
{
    // The transform: the fast DCT-IV is the definition's, for every hop.
    for (int hop : crumb::hops)
    {
        crumb::Mdct m;
        m.prepare(hop);
        std::vector<double> x, want, a((size_t) hop), b = a, c = a, d = a;
        juce::Random r(7);
        for (int i = 0; i < hop; ++i) x.push_back(r.nextDouble() - 0.5);
        for (int k = 0; k < hop; k += 1 + hop / 32)
        {
            double v = 0;
            for (int i = 0; i < hop; ++i) v += x[(size_t) i] * std::cos(juce::MathConstants<double>::pi / hop * (i + 0.5) * (k + 0.5));
            want.push_back(v * std::sqrt(2.0 / hop));
        }
        m.dct4(x.data(), a.data(), b.data(), c.data(), d.data());
        for (int k = 0, j = 0; k < hop; k += 1 + hop / 32, ++j) CHECK(std::abs(x[(size_t) k] - want[(size_t) j]) < 1e-9);
    }
    // Loss 0 and drop 0: no latency and the input bit for bit (testNull); frames of every size at loss 0 rebuild the input.
    for (float speed : { 0.0f, 12.5f, 25.0f, 40.0f, 50.0f, 62.5f, 75.0f, 87.5f, 100.0f })
    {
        Proc p;
        set(p, cleanCrush), set(p, { { "drop", 0.1f }, { "speed", speed } }); // 0.09 % of frames: none in this second
        prepare(p, 44100, 512, 2);
        CHECK(p.getLatencySamples() == 2048);
        const auto y = run(p, 512, 2, 30000, [](int i) { return signal(i, 44100); });
        float diff = 0;
        int lost = 0;
        for (size_t i = 4096; i < y.size(); ++i)
            if (p.scope.mark[i & (crumb::Scope::size - 1)].load() == 0) diff = std::max(diff, std::abs(y[i] - signal((int) i - 2048, 44100)));
            else ++lost;
        if (lost == 0) CHECK(diff < 1e-6f);
    }
    // The latency follows the codec at every rate: one long frame while it runs, none while it does not; restores too.
    for (auto [fs, want] : { std::pair { 44100.0, 2048 }, { 48000.0, 2048 }, { 96000.0, 4096 }, { 192000.0, 8192 } })
    {
        Proc p;
        prepare(p, fs, 256, 2);
        CHECK(p.getLatencySamples() == 0);
        set(p, "loss", 30), CHECK(p.getLatencySamples() == want);
        set(p, "loss", 0), CHECK(p.getLatencySamples() == 0);
        set(p, "drop", 5), CHECK(p.getLatencySamples() == want);
        juce::MemoryBlock m;
        p.getStateInformation(m);
        Proc q;
        prepare(q, fs, 256, 2), load(q, m);
        CHECK(q.getLatencySamples() == want);
        // mix 0 and bypass: the latency-aligned input, exactly
        for (bool bypassed : { false, true })
        {
            set(q, { { "mix", bypassed ? 100.0f : 0.0f }, { "bypass", bypassed ? 1.0f : 0.0f }, { "loss", 80 } }), prepare(q, fs, 256, 2);
            const double rate = fs;
            const auto y = run(q, 256, 2, 3 * want, [rate](int i) { return signal(i, rate); });
            for (int i = want; i < 3 * want; ++i) CHECK(y[(size_t) i] == signal(i - want, rate));
        }
    }
    // More loss throws away more: the residual grows, the energy kept (auto off) falls; auto brings the level back.
    const auto render = [](const Settings& s, double rate = 44100, int seconds = 1) {
        Proc p;
        set(p, cleanCrush), set(p, "autoGain", 0), set(p, "speed", 25), set(p, s);
        prepare(p, rate, 512, 2);
        return run(p, 512, 2, (int) rate * seconds + 2048, [rate](int i) { return signal(i, rate); });
    };
    std::vector<float> dry;
    for (int i = 0; i < 46148; ++i) dry.push_back(i < 2048 ? 0 : signal(i - 2048, 44100));
    double last = -1, lastKept = 1e9;
    for (float loss : { 4.0f, 15.0f, 35.0f, 60.0f, 100.0f })
    {
        const auto y = render({ { "loss", loss } });
        double gone = 0;
        for (size_t i = 4096; i < y.size(); ++i) gone += std::pow((double) y[i] - dry[i], 2);
        const double kept = energy(y, 4096, y.size());
        std::cout << "loss " << loss << " %: kept " << 10 * std::log10(kept / energy(dry, 4096, dry.size())) << " dB, residual " << 10 * std::log10(gone / energy(dry, 4096, dry.size())) << " dB\n";
        CHECK(gone > last && kept < lastKept * 1.02);
        last = gone, lastKept = kept;
        const auto loud = render({ { "loss", loss }, { "autoGain", 1 } }), half = render({ { "loss", loss }, { "autoGain", 1 }, { "auto", 50 } }), none = render({ { "loss", loss }, { "autoGain", 1 }, { "auto", 0 } });
        CHECK(none == y); // auto 0 % = the old switch off
        const double full = energy(loud, 22050, loud.size()), target = energy(dry, 22050, dry.size());
        CHECK(std::abs(10 * std::log10(full / target)) < 1.0);
        CHECK(energy(half, 22050, half.size()) <= full * 1.01 && energy(half, 22050, half.size()) >= energy(y, 22050, y.size()) * 0.99);
        // keep: what it threw away, the exact complement of what it kept
        const auto inverse = render({ { "loss", loss }, { "keep", 1 } });
        float worst = 0;
        for (size_t i = 4096; i < y.size(); ++i) worst = std::max(worst, std::abs(y[i] + inverse[i] - dry[i]));
        CHECK(worst < 1e-5f);
    }
    // Ear and flat choose differently; the ear throws away what it cannot hear first (the 19 kHz tone under the noise).
    CHECK(render({ { "loss", 40 }, { "ear", 1 } }) != render({ { "loss", 40 } }));
    // Speed is the frame: a click smears before itself by up to a frame, slow more than fast.
    const auto early = [](float speed) {
        Proc p;
        set(p, cleanCrush), set(p, { { "loss", 60 }, { "speed", speed }, { "autoGain", 0 } });
        prepare(p, 44100, 512, 1);
        const auto y = run(p, 512, 1, 16384, [](int i) { return i >= 8192 && i < 8200 ? 0.9f : 0.0f; });
        return energy(y, 4096, 8192 + 2048 - 200) / energy(y, 4096, y.size());
    };
    const double smeared = early(0), tight = early(100);
    std::cout << "pre-echo share, speed 0: " << smeared << ", speed 100: " << tight << "\n";
    CHECK(smeared > 0.05 && tight < 1e-9);
    // Dropouts: the share of lost frames follows drop; a gap is silence, a hold repeats; both marked per frame.
    for (float fill : { 0.0f, 1.0f })
        for (float drop : { 10.0f, 50.0f })
        {
            Proc p;
            set(p, cleanCrush), set(p, { { "drop", drop }, { "fill", fill }, { "speed", 50 }, { "autoGain", 0 } });
            prepare(p, 44100, 512, 1);
            int lost = 0, total = 0, quietWrong = 0, cleanWrong = 0, loudHeld = 0, heldRuns = 0, hop = 256;
            for (int k = 0; k < 40; ++k)
            {
                const auto y = run(p, 512, 1, 16384, [k](int i) { return 0.4f * (float) std::sin(0.05 * (k * 16384 + i)); });
                for (int i = 2 * hop; i < 16384 - 2 * hop; ++i)
                {
                    const auto mark = [&](int at) { return (int) p.scope.mark[(size_t) ((k * 16384 + at) & (crumb::Scope::size - 1))].load(); };
                    if (k == 0 && i < 6144) continue;
                    ++total, lost += mark(i) != 0;
                    CHECK(mark(i) == 0 || mark(i) == (fill > 0 ? 2 : 1));
                    const bool deep = mark(i) != 0 && mark(i - hop) != 0 && mark(i + hop) != 0, clear = mark(i) == 0 && mark(i - hop) == 0 && mark(i + hop) == 0;
                    if (deep && fill < 0.5f) quietWrong += std::abs(y[(size_t) i]) > 1e-6f;
                    if (deep && fill > 0.5f) ++heldRuns, loudHeld += std::abs(y[(size_t) i]) > 0.05f;
                    if (clear) cleanWrong += std::abs(y[(size_t) i] - 0.4f * (float) std::sin(0.05 * (k * 16384 + i - 2048))) > 1e-5f;
                }
            }
            const double share = (double) lost / total;
            std::cout << "drop " << drop << " % " << (fill > 0 ? "hold" : "gap") << ": " << 100 * share << " % of the time lost\n";
            CHECK(std::abs(share - 0.9 * drop / 100) < 0.25 * drop / 100 && quietWrong == 0 && cleanWrong == 0);
            if (fill > 0 && drop > 20) CHECK(heldRuns > 1000 && loudHeld > heldRuns / 4);
        }
    // Stereo loses sides separately, joint and mono together; mono is the sum on both sides.
    for (float stereo : { 0.0f, 1.0f, 2.0f })
    {
        Proc p;
        set(p, cleanCrush), set(p, { { "loss", 30 }, { "drop", 40 }, { "stereo", stereo }, { "speed", 50 } });
        prepare(p, 44100, 512, 2);
        juce::AudioBuffer<float> b(2, 512);
        juce::MidiBuffer midi;
        double differ = 0;
        int oneSide = 0;
        for (int k = 0; k < 120; ++k)
        {
            for (int i = 0; i < 512; ++i) b.setSample(0, i, signal(k * 512 + i, 44100)), b.setSample(1, i, 0.8f * signal(k * 512 + i, 44100));
            p.processBlock(b, midi);
            double l = 0, r = 0;
            for (int i = 0; i < 512; ++i) differ += std::abs(b.getSample(0, i) - b.getSample(1, i)), l += std::abs(b.getSample(0, i)), r += std::abs(b.getSample(1, i));
            oneSide += k > 8 && ((l < 1e-6) != (r < 1e-6));
        }
        if (stereo > 1.5f) CHECK(differ < 1e-3);
        if (stereo > 0.5f) CHECK(oneSide == 0);
        if (stereo < 0.5f) CHECK(oneSide > 0);
    }
    // Every host block size gives the same audio, and the codec's switches are smooth: on a 20 Hz sine nothing steps.
    {
        const Settings s { { "loss", 55 }, { "drop", 20 }, { "fill", 1 }, { "jitter", 30 }, { "speed", 60 } };
        std::vector<float> first;
        for (int block : { 512, 1, 333 })
        {
            Proc p;
            set(p, s), prepare(p, 48000, 512, 2);
            const auto y = run(p, block, 2, 24000, [](int i) { return signal(i, 48000); });
            if (first.empty()) first = y;
            CHECK(y == first);
        }
        Proc p;
        set(p, { { "bits", 0 }, { "autoGain", 0 }, { "speed", 0 } });
        prepare(p, 48000, 512, 2);
        std::vector<float> y;
        for (auto [id, v] : { std::pair { "gate", -96.0f }, { "drop", 0.001f }, { "speed", 100.0f }, { "speed", 30.0f }, { "drop", 0.0f }, { "lowCut", 25.0f }, { "lowCut", 20.0f }, { "highCut", 8000.0f }, { "slope", 7.0f }, { "highCut", 20000.0f } })
        {
            set(p, id, v);
            const auto at = (int) y.size();
            for (float v2 : run(p, 512, 2, 9600, [at](int i) { return (float) (0.2 * std::sin(2 * juce::MathConstants<double>::pi * 20 * (at + i) / 48000)); })) y.push_back(v2);
        }
        float step = 0;
        for (size_t i = 1; i < y.size(); ++i) step = std::max(step, std::abs(y[i] - y[i - 1]));
        std::cout << "codec and band switches, 20 Hz sine: largest step " << step << "\n"; // the sine's own: 0.0005
        CHECK(step < 0.004f);
    }
}

static void testBand()
{
    const auto level = [](const Settings& s, double hz) {
        Settings all { { "bits", 0 }, { "rate", 44100 }, { "approxOn", 0 } };
        all.insert(all.end(), s.begin(), s.end());
        return at(spectrum(all, hz), hz);
    };
    // Butterworth edges: -3 dB at the edge, the slope an octave past it, flat inside, at every slope.
    for (int k = 0; k < crumb::numSlopes; ++k)
    {
        const double order = crumb::slopeOrder[k], edge = std::round(1000.0 * 32768 / 44100) * 44100 / 32768;
        const auto want = [order](double ratio) { return -10 * std::log10(1 + std::pow(ratio, 2 * order)); };
        const auto warp = [](double hz) { return std::tan(juce::MathConstants<double>::pi * hz / 44100); };
        for (double hz : { 500.0, 1000.0, 1400.0, 2000.0 })
        {
            const double f = std::round(hz * 32768 / 44100) * 44100 / 32768, hi = want(warp(f) / warp(edge)), lo = want(warp(edge) / warp(f));
            if (hi > -70) CHECK(std::abs(level({ { "highCut", (float) edge }, { "slope", (float) k } }, hz) - hi) < 0.5);
            if (lo > -70) CHECK(std::abs(level({ { "lowCut", (float) edge }, { "slope", (float) k } }, hz) - lo) < 0.5);
        }
    }
    CHECK(level({ { "lowCut", 500 }, { "highCut", 4000 } }, 1500) > -0.5 && level({ { "lowCut", 500 }, { "highCut", 4000 } }, 100) < -50);
    CHECK(level({ { "lowCut", 500 }, { "highCut", 4000 }, { "invert", 1 }, { "slope", 5 } }, 1500) < -40 && level({ { "lowCut", 500 }, { "highCut", 4000 }, { "invert", 1 }, { "slope", 5 } }, 12000) > -1);
    CHECK(level({ { "highCut", 2000 }, { "reso", 80 } }, 2000) > level({ { "highCut", 2000 } }, 2000) + 12);
    // Open edges are the input bit for bit, inverted or not, and place decides what the crush hears.
    for (float invert : { 0.0f, 1.0f })
    {
        Proc p;
        set(p, cleanCrush), set(p, { { "invert", invert }, { "slope", 7 }, { "reso", 100 } });
        prepare(p, 44100, 512, 2);
        const auto y = run(p, 512, 2, 8192, [](int i) { return signal(i, 44100); });
        for (size_t i = 0; i < y.size(); ++i) CHECK(y[i] == signal((int) i, 44100));
    }
    const auto place = [](float order) {
        Proc p;
        set(p, { { "bits", 3 }, { "rate", 44100 }, { "approxOn", 0 }, { "dcShift", 1 }, { "highCut", 800 }, { "fOrder", order } });
        prepare(p, 44100, 512, 1);
        const auto y = run(p, 512, 1, 8192, [](int i) { return signal(i, 44100); });
        float step = 0;
        for (size_t i = 4096; i < y.size(); ++i) step = std::max(step, std::abs(y[i] - y[i - 1]));
        return step;
    };
    CHECK(place(0) > 0.2f && place(1) < 0.15f); // before the crush: 3-bit steps stay; after it: smoothed
    // Gate: under the threshold the wet path is silent (also the mid-riser's idle), over it untouched.
    Proc p;
    set(p, { { "bits", 8 }, { "gate", -30 }, { "rate", 44100 } });
    prepare(p, 44100, 512, 1);
    const auto quiet = run(p, 512, 1, 44100, [](int i) { return 0.01f * (float) std::sin(0.1 * i); });
    const auto loud = run(p, 512, 1, 44100, [](int i) { return 0.4f * (float) std::sin(0.1 * i); });
    CHECK(energy(quiet, 40000, 44100) < 1e-8 && energy(loud, 22050, 44100) > 0.05 * 22050);
}

// A session saved by 0.2.0 itself (preset 18 with auto gain on in slot B, a custom slot A with it off; both use the old
// filter), and the hashes of what 0.2.0 played: 0.3.0 loads it, keeps the parameters and sounds the same.
static juce::String sha(const juce::MemoryOutputStream&);
static float groove(int i);
[[maybe_unused]] static juce::String playHash(Proc& p)
{
    prepare(p, 48000, 512, 2);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    juce::MemoryOutputStream bytes;
    for (int b = 0; b < 188; ++b)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 512; ++i) buffer.setSample(c, i, groove(b * 512 + i) * (c == 0 ? 1.0f : -0.7f));
        p.processBlock(buffer, midi);
        for (int c = 0; c < 2; ++c) bytes.write(buffer.getReadPointer(c), 512 * sizeof(float));
    }
    return sha(bytes);
}
static juce::MemoryBlock v2Fixture()
{
    return blob(R"(<?xml version="1.0" encoding="UTF-8"?> <CRUMB stateVersion="2" preset="18" ab="1" slotA="5;0.291666657,0.416666657,0,0,0.853275359,0.219999999,1,0.349999994,1,0,0.25,1,0.743482947,0.349999994,1,0.666666687,0" slotNameA=""><PARAM id="approxDev" value="0.0"/><PARAM id="approxOn" value="1.0"/><PARAM id="autoGain" value="1.0"/><PARAM id="bits" value="8.0"/><PARAM id="bypass" value="0.0"/><PARAM id="cutoff" value="1800.0"/><PARAM id="dcShift" value="0.0"/><PARAM id="dither" value="20.0"/><PARAM id="fOrder" value="1.0"/><PARAM id="fType" value="2.0"/><PARAM id="imagesOn" value="1.0"/><PARAM id="imagesShift" value="0.0"/><PARAM id="jitter" value="0.0"/><PARAM id="mix" value="80.0"/><PARAM id="output" value="-2.0"/><PARAM id="preamp" value="10.0"/><PARAM id="rate" value="11025.0"/><PARAM id="reso" value="40.0"/></CRUMB>)");
}
[[maybe_unused]] static const char* const v2Hashes[] = { "7f8de87d275eeccf9f725e86dcb315a3a7b2b72c7522d10f8438b7b25c0796ef", "825925806d27227a109e96d879c513dd67cf52a8174dee1e02eb4305bb232acb" };
// The same from 0.3.0 itself: slot A dial tone with auto gain at 60 %, slot B (current) the old filter beside the codec, driven,
// with auto gain; and what 0.3.0 played of each.
static juce::MemoryBlock v3Fixture()
{
    return blob(R"(<?xml version="1.0" encoding="UTF-8"?> <CRUMB stateVersion="3" preset="1" ab="1" slotA="39;0.375,0.208333328,1,0,0.71133244,0.0799999982,0,0.5,0,0,0,1,1,0.349999994,0.850000024,0.5,1,0.819999993,0.75,0.100000001,1,0,1,0.479166657,1,0.392030418,0.743482947,0.571428597,0,0.600000024" slotNameA="" editorWidth="1320"><PARAM id="approxDev" value="0.0"/><PARAM id="approxOn" value="1.0"/><PARAM id="auto" value="60.0"/><PARAM id="autoGain" value="1.0"/><PARAM id="bits" value="12.0"/><PARAM id="bypass" value="0.0"/><PARAM id="cutoff" value="11000.0"/><PARAM id="dcShift" value="0.0"/><PARAM id="dither" value="0.0"/><PARAM id="drop" value="8.0"/><PARAM id="ear" value="1.0"/><PARAM id="fOrder" value="1.0"/><PARAM id="fType" value="1.0"/><PARAM id="fill" value="1.0"/><PARAM id="gate" value="-96.0"/><PARAM id="highCut" value="20000.0"/><PARAM id="imagesOn" value="1.0"/><PARAM id="imagesShift" value="10.0"/><PARAM id="invert" value="0.0"/><PARAM id="jitter" value="4.0"/><PARAM id="keep" value="1.0"/><PARAM id="loss" value="30.0"/><PARAM id="lowCut" value="20.0"/><PARAM id="mix" value="100.0"/><PARAM id="output" value="-1.0"/><PARAM id="preamp" value="15.0"/><PARAM id="rate" value="26040.0"/><PARAM id="reso" value="8.0"/><PARAM id="slope" value="3.0"/><PARAM id="speed" value="40.0"/><PARAM id="stereo" value="1.0"/></CRUMB>)");
}
[[maybe_unused]] static const char* const v3Hashes[] = { "08bf8e3ba625613df74bf502b9f79e728b6d0b82865cd800f89b8e2c499bb6a1", "03a649c3c3725f97aaa7695f98db32838047c68c818c440175f991c92609112d" };
static void testMigration()
{
    Proc p;
    set(p, { { "loss", 70 }, { "drop", 30 }, { "highCut", 900 }, { "gate", -20 }, { "stereo", 2 }, { "invert", 1 }, { "auto", 10 } }); // nothing of this may survive
    load(p, v2Fixture());
    CHECK(p.abSlot() == 1 && p.getCurrentProgram() == 18 && get(p, "bits") == 8 && get(p, "fType") == 2 && get(p, "cutoff") == 1800 && get(p, "reso") == 40 && get(p, "mix") == 80 && get(p, "autoGain") == 1);
    for (int k = 18; k < ids.size(); ++k)
        CHECK(p.apvts.getParameter(ids[k])->getValue() == p.apvts.getParameter(ids[k])->getDefaultValue());
    CHECK(p.getLatencySamples() == 0 && get(p, "auto") == 100);
    // the band shows the old band-pass; nothing changes until it is edited, then it moves onto the new controls
    const auto b = p.band();
    CHECK(b.legacy && b.low == 1800 && b.high == 1800 && !b.invert);
    p.selectAb(0); // its 17 values, 0.3.0's neutral
    CHECK(get(p, "fType") == 1 && std::abs(get(p, "cutoff") - 3400) < 1 && get(p, "autoGain") == 0 && get(p, "loss") == 0 && get(p, "highCut") == 20000 && get(p, "auto") == 100);
    CHECK(p.band().legacy && p.band().low == 20 && std::abs(p.band().high - 3400) < 1);
    p.adoptBand();
    CHECK(!p.band().legacy && get(p, "fType") == 0 && std::abs(get(p, "highCut") - 3400) < 1 && get(p, "lowCut") == 20 && get(p, "slope") == 1 && get(p, "reso") == 35);
    // a 0.2 user preset file (the parameters alone) loads the same way
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("crumb-v2", "", false);
    dir.createDirectory(), dir.getChildFile("mine.xml").replaceWithText("<CRUMB stateVersion='2'><PARAM id='bits' value='6'/><PARAM id='autoGain' value='1'/><PARAM id='fType' value='3'/><PARAM id='cutoff' value='250'/></CRUMB>");
    Proc user(dir);
    set(user, "loss", 50), user.loadUserPreset("mine");
    CHECK(user.presetName() == "mine" && get(user, "bits") == 6 && get(user, "loss") == 0 && user.band().legacy && user.band().low == 250 && user.band().high == 20000);
    dir.deleteRecursively();
    // round trip: what 0.4.0 saves of it loads identically
    juce::MemoryBlock m, m2;
    Proc a, c;
    load(a, v2Fixture()), a.getStateInformation(m), load(c, m), c.getStateInformation(m2);
    CHECK(m == m2 && juce::AudioProcessor::getXmlFromBinary(m.getData(), (int) m.getSize())->getIntAttribute("stateVersion") == 4);

    // 0.3.0: every parameter and both slots come back as they were, auto gain on included (it runs, off the panel), and stay
    // through a save; testGoldens plays them.
    Proc three;
    set(three, { { "autoGain", 0 }, { "auto", 10 }, { "bits", 3 } }), load(three, v3Fixture());
    const Settings slotB { { "preamp", 15 }, { "bits", 12 }, { "rate", 26040 }, { "jitter", 4 }, { "imagesOn", 1 }, { "imagesShift", 10 }, { "fType", 1 }, { "cutoff", 11000 }, { "reso", 8 }, { "mix", 100 }, { "output", -1 }, { "autoGain", 1 },
        { "loss", 30 }, { "speed", 40 }, { "drop", 8 }, { "fill", 1 }, { "keep", 1 }, { "ear", 1 }, { "gate", -96 }, { "stereo", 1 }, { "lowCut", 20 }, { "highCut", 20000 }, { "slope", 3 }, { "invert", 0 }, { "auto", 60 } };
    const Settings slotA { { "preamp", 18 }, { "bits", 5 }, { "dcShift", 1 }, { "rate", 6000 }, { "jitter", 8 }, { "approxOn", 0 }, { "reso", 35 }, { "mix", 85 }, { "output", -6 }, { "autoGain", 1 },
        { "loss", 82 }, { "speed", 75 }, { "drop", 10 }, { "fill", 1 }, { "ear", 1 }, { "gate", -50 }, { "stereo", 2 }, { "lowCut", 300 }, { "highCut", 3400 }, { "slope", 4 }, { "auto", 60 } };
    const auto holds = [](Proc& q, const Settings& want) {
        for (auto& [id, v] : want)
            if (std::abs(get(q, id) - v) > 1e-3f * std::max(1.0f, std::abs(v))) return false;
        return true;
    };
    CHECK(three.abSlot() == 1 && three.getCurrentProgram() == 1 && three.editorWidth == 1320 && holds(three, slotB) && three.getLatencySamples() == 2048);
    three.selectAb(0), CHECK(holds(three, slotA) && three.getCurrentProgram() == 39);
    three.selectAb(1), CHECK(holds(three, slotB));
    load(a, v3Fixture()), a.getStateInformation(m), load(c, m), c.getStateInformation(m2);
    CHECK(m == m2 && holds(c, slotB));
    c.selectAb(0), CHECK(holds(c, slotA));
    // a new sound has no auto gain: a factory preset, randomize and a 0.4 instance; the session's other slot keeps it
    c.setCurrentProgram(28), CHECK(get(c, "autoGain") == 0);
    c.selectAb(1), CHECK(get(c, "autoGain") == 1 && get(c, "auto") == 60);
    c.randomize(3), CHECK(get(c, "autoGain") == 0);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("crumb-v3", "", false);
    folder.createDirectory(), folder.getChildFile("mine.xml").replaceWithText("<CRUMB stateVersion='3'><PARAM id='preamp' value='12'/><PARAM id='autoGain' value='1'/><PARAM id='auto' value='80'/><PARAM id='loss' value='20'/></CRUMB>");
    Proc mine(folder);
    mine.loadUserPreset("mine"), CHECK(get(mine, "preamp") == 12 && get(mine, "autoGain") == 1 && get(mine, "auto") == 80 && get(mine, "loss") == 20);
    folder.deleteRecursively();
}

// 0.3.0's mix and output of every preset that auto gain (on by default until then) turned down or up: 0.4.0 bakes that in.
struct Baked { int program; float mix, output; };
static const Baked baked[] = {
    { 1, 100, -1 }, { 2, 100, -2 }, { 3, 100, -4 }, { 5, 100, 0 }, { 6, 100, -6 }, { 7, 100, -4 },
    { 8, 100, -1 }, { 9, 100, -3 }, { 10, 100, -6 }, { 11, 100, -2 }, { 12, 100, -3 }, { 13, 70, -3 },
    { 14, 100, -2 }, { 15, 100, -3 }, { 16, 60, -4 }, { 17, 100, 0 }, { 18, 100, -2 }, { 19, 100, -1 },
    { 21, 100, 0 }, { 23, 100, -8 }, { 24, 35, 0 }, { 28, 100, -1.5f }, { 29, 100, 0 },
    { 30, 100, 0 }, { 31, 100, -2 }, { 32, 100, 0 }, { 35, 100, 0 }, { 36, 100, -2 }, { 37, 100, -3 },
    { 38, 100, -2 }, { 39, 100, -6 }, { 40, 100, -8 },
};
static void testBaked()
{
    for (auto& b : baked)
    {
        Proc was, is;
        was.setCurrentProgram(b.program), set(was, { { "mix", b.mix }, { "output", b.output }, { "autoGain", 1 }, { "auto", 100 } });
        is.setCurrentProgram(b.program);
        CHECK(get(is, "autoGain") == 0 && get(is, "output") != b.output);
        prepare(was, 48000, 512, 2), prepare(is, 48000, 512, 2);
        const auto x = run(was, 512, 2, 144000, [](int i) { return groove(i); }), y = run(is, 512, 2, 144000, [](int i) { return groove(i); });
        double exx = 0, eyy = 0, ed = 0;
        for (size_t i = 48000; i < x.size(); ++i) exx += x[i] * x[i], eyy += y[i] * y[i], ed += (y[i] - x[i]) * (y[i] - x[i]);
        const double level = 10 * std::log10(eyy / exx), null = 10 * std::log10(ed / exx);
        std::cout << "preset " << b.program << " against 0.3.0 with auto gain: level " << level << " dB, null " << null << " dB\n";
        if (get(is, "loss") == 0) CHECK(null < -42); // drive's share exactly, to the 0.1 dB of output's grid
        else CHECK(std::abs(level - (b.program == 39 ? 2.0 : b.program == 40 ? 4.9 : 0.0)) < 2.5); // the codec's is an average (the demo loop and this one)
    }
    // the others are what they were
    Proc p;
    for (int i : { 0, 4, 20, 22, 25, 26, 27, 33, 34 })
    {
        p.setCurrentProgram(i);
        const std::map<int, std::pair<float, float>> old { { 0, { 100, 0 } }, { 4, { 100, 0 } }, { 20, { 50, -2 } }, { 22, { 100, -3 } }, { 25, { 100, 0 } }, { 26, { 100, 0 } }, { 27, { 100, 0 } }, { 33, { 100, 0 } }, { 34, { 100, -1 } } };
        CHECK(get(p, "mix") == old.at(i).first && get(p, "output") == old.at(i).second);
    }
}

static void testRandomize()
{
    Proc a, b;
    set(a, { { "mix", 40 }, { "output", -3 }, { "bypass", 1 }, { "autoGain", 1 }, { "auto", 25 } }), set(b, "mix", 100);
    a.randomize(5), b.randomize(5);
    CHECK(a.presetName() == "random" && a.preset == -1 && get(a, "mix") == 40 && get(a, "output") == -3 && get(a, "bypass") == 1 && get(a, "autoGain") == 0 && get(a, "auto") == 100 && get(b, "mix") == 100);
    for (auto* id : { "preamp", "bits", "rate", "loss", "speed", "drop", "fill", "lowCut", "highCut", "stereo", "fType" }) CHECK(get(a, id) == get(b, id));
    std::set<float> losses;
    for (int k = 0; k < 20; ++k) a.randomize(k), losses.insert(get(a, "loss")), CHECK(get(a, "preamp") <= 9);
    CHECK(losses.size() > 10 && get(a, "fType") == 0);
}

// The picture: columns hold the true lowest and highest samples, the traces sit on them, steps and lost frames show.
static void testWave()
{
    crumb::Wave w;
    for (int i = 0; i < 40000; ++i) w.add(0.5f * (float) std::sin(0.02 * i), i % 600 < 300 ? 0.25f : -0.5f, 0, i >= 25000 && i < 26000 ? 2 : 0);
    w.build(256, 3, false, 0, 40000);
    CHECK(std::abs(w.peak - 0.5f) < 0.01f && !w.lost.empty() && !w.line.empty() && w.taken.empty());
    for (int k = 0; k < crumb::Wave::columns; ++k)
    {
        float lo = 9, hi = -9;
        bool mark = false;
        for (int s0 = (40000 / 256 - 84 + k / 3) * 256, i = s0; i < s0 + 256; ++i)
            lo = std::min(lo, i % 600 < 300 ? 0.25f : -0.5f), hi = std::max(hi, i % 600 < 300 ? 0.25f : -0.5f), mark = mark || (i >= 25000 && i < 26000);
        CHECK(w.column[(size_t) k].wet.lo == lo && w.column[(size_t) k].wet.hi == hi && (w.column[(size_t) k].mark != 0) == mark);
    }
    // steps stand on the codec's frames wherever its count began: lost frames fill whole steps
    w.build(1000, 3, false, 0, 39000);
    for (int k = 0; k < crumb::Wave::columns; ++k)
        CHECK((w.column[(size_t) k].mark != 0) == (39 - 84 + k / 3 == 24));
    // a hold of 8 samples at 4 columns a hold: flat runs of 4 columns or more, one row each
    crumb::Wave h;
    for (int i = 0; i < 20000; ++i) h.add(0.5f * (float) std::sin(0.01 * i), 0.5f * (float) std::sin(0.01 * (i - i % 8)), 0, 0);
    h.build(2, 1, true, 1920);
    std::map<int, int> top;
    for (auto [k, row] : h.line) top[k] = top.count(k) ? std::min(top[k], row) : row;
    int run = 1, runs = 0, shortRuns = 0;
    for (int k = 1; k < crumb::Wave::columns; ++k)
    {
        if (h.column[(size_t) k].wet.hi == h.column[(size_t) k - 1].wet.hi) ++run;
        else if (k > 6) ++runs, shortRuns += run < 4, run = 1;
        else run = 1;
    }
    CHECK(runs > 40 && shortRuns == 0);
    CHECK(h.column[0].dry.lo > 0 && h.column[0].dry.lo < 0.02f); // triggered: starts just past a rising zero
    // clipping: hot dots hang from the crest of the steps whose samples clipped, on their own side, deeper by the dB; the rails
    // where the output is past 0 dBFS; and nowhere else
    crumb::Wave z;
    for (int i = 0; i < 40000; ++i)
    {
        const bool up = i >= 30000 && i < 30300, down = i >= 35000 && i < 35300;
        z.add(0.5f, i % 2 ? 0.5f : -0.5f, 0, 0, up ? 3.0f : down ? -7.0f : 0.0f, i >= 37000 && i < 37300 ? 1.0f : 0.0f);
    }
    z.build(256, 3, false, 0, 40000);
    std::map<int, std::pair<int, int>> dots; // per column: dots above and below the middle
    for (auto [k, row] : z.hot) ++(row < 24 ? dots[k].first : dots[k].second);
    for (int k = 0; k < crumb::Wave::columns; ++k)
    {
        const int from = (40000 / 256 - 84 + k / 3) * 256, to = from + 256;
        const auto in = [&](int a, int b) { return from < b && to > a; };
        const int above = (in(30000, 30300) ? 4 : 0) + (in(37000, 37300) ? 2 : 0), below = in(35000, 35300) ? 6 : 0;
        CHECK(dots[k].first == above && dots[k].second == below);
    }
    w.clear(), w.build(256, 3, false, 0), CHECK(w.hot.empty());
    // silence, and no audio at all: one flat line, no ghost fill, nothing taken
    w.clear(), w.build(256, 3, false, 0);
    for (auto [k, row] : w.line) CHECK(row == 23 || row == 24);
    CHECK(w.line.size() == 2 * crumb::Wave::columns && w.lost.empty() && w.taken.empty() && w.water.empty());
}

static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(5); }
// Asynchronous updates (attachments, timers) can lag on loaded CI machines: dispatch until done() or a 2 s deadline.
template <typename Done> static void pumpUntil(Done done)
{
    for (auto end = juce::Time::getMillisecondCounter() + 2000; !done() && juce::Time::getMillisecondCounter() < end;)
        pump(), juce::Timer::callPendingTimersSynchronously();
}

static double testMs = 0;
static double testClock() { return testMs; }
static juce::String pixels(const juce::Image&);

// Observe real Component::repaint invalidations without a window or an extra snapshot paint advancing scope gain.
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
        g.addTransform(juce::AffineTransform::scale(scale));
        g.reduceClipRegion(pending);
        paint(g), pending = {};
        return true;
    }
};

// The editor redraws only cells that changed and repaints only their area: after any run of frames, the image built
// from those partial repaints equals a render from scratch, and an idle editor (audio stopped) repaints nothing.
static void testScopeRepaints()
{
    for (int percent : { 50, 100, 200 })
        for (float loss : { 0.0f, 60.0f })
        {
            Proc p;
            testMs = 0;
            p.scope.clock = testClock;
            set(p, "loss", loss), set(p, "drop", loss / 4);
            prepare(p, 48000, 512, 2);
            run(p, 512, 2, 8192, [](int i) { return (float) (0.4 * std::sin(i * 0.1)); });
            std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
            auto& timer = dynamic_cast<juce::Timer&>(*e);
            timer.stopTimer();
            e->setSize(1056 * percent / 100, 656 * percent / 100), e->setVisible(true);
            auto* capture = new RepaintCapture(*e);
            e->setCachedComponentImage(capture); // the editor takes ownership
            const float scale = (float) percent / 50; // 2x pixels
            juce::Image image(juce::Image::RGB, 1056 * percent / 50, 656 * percent / 50, true);
            capture->consume(image, scale);
            int paints = 0;
            for (int frame = 0; frame < 60; ++frame)
            {
                testMs += 1000.0 / 30;
                if (frame < 30) run(p, 512, 2, 1600, [frame](int i) { return 0.3f * groove(frame * 1600 + i); });
                timer.timerCallback();
                const bool painted = capture->consume(image, scale);
                paints += painted;
                if (frame < 29) CHECK(painted);
                if (frame > 50) CHECK(!painted); // the host stopped: a flat line, no motion
                if (frame == 10) set(p, "bits", 6), set(p, "fOrder", 0); // value changes repaint their cells
            }
            CHECK(paints > 25);
            testMs += 1000.0 / 30;
            run(p, 512, 2, 8192, [](int i) { return (float) (0.01 * std::sin(i * 0.1)); });
            timer.timerCallback();
            CHECK(capture->consume(image, scale));
            juce::Image other(juce::Image::RGB, 8, 8, true), fresh(juce::Image::RGB, image.getWidth(), image.getHeight(), true);
            { juce::Graphics g(other); g.addTransform(juce::AffineTransform::scale(0.01f)); e->paint(g); } // drops the cached image
            { juce::Graphics g(fresh); g.addTransform(juce::AffineTransform::scale(scale)); e->paint(g); }   // so this renders all cells
            CHECK(pixels(image) == pixels(fresh));
        }
}

// Low and high host rates build, run and paint; an overlong user preset name stays between the preset arrows and leaves
// them as with a short one; with no audio ever the picture is the flat line.
static void testEditorEdges()
{
    const auto frames = [](juce::AudioProcessorEditor& e) {
        for (int k = 0; k < 3; ++k) dynamic_cast<juce::Timer&>(e).timerCallback();
        return e.createComponentSnapshot(e.getLocalBounds());
    };
    for (double fs : { 8000.0, 11025.0, 22050.0, 192000.0 })
        for (float loss : { 0.0f, 50.0f })
        {
            Proc p;
            set(p, "loss", loss), prepare(p, fs, 512, 2);
            run(p, 512, 2, 8192, [fs](int i) { return signal(i, fs); });
            std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
            CHECK(frames(*e).isValid());
        }
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("crumb-names", "", false);
    dir.createDirectory(), dir.getChildFile("a.xml").replaceWithText("<CRUMB/>");
    dir.getChildFile("b" + juce::String::repeatedString("long name ", 6) + "x.xml").replaceWithText("<CRUMB/>");
    Proc p(dir);
    p.setCurrentProgram(24), p.stepPreset(1);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    const auto shortName = frames(*e);
    p.stepPreset(1);
    const auto longName = frames(*e);
    CHECK(p.presetName().length() == 62);
    for (juce::Rectangle<int> r : { juce::Rectangle<int> { 24, 16, 88, 16 }, { 304, 16, 160, 16 } }) // crumb <, and > A B randomize
        CHECK(pixels(shortName.getClippedImage(r)) == pixels(longName.getClippedImage(r)));
    dir.deleteRecursively();
}

// Mouse events at 100 % and 200 %: every control reacts at its hit region of cells, the regions do not
// overlap, dials drag with the slider's 250 px sensitivity, double-click resets, and clicking a value types one; the
// selectors step both ways and wrap, and their slots are the longest name and a cell either side.
static void testInteraction()
{
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("crumb-ui", "", false);
    for (int percent : { 100, 200 })
    {
        Proc p(folder);
        prepare(p, 48000, 512, 2);
        std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
        auto& timer = dynamic_cast<juce::Timer&>(*e);
        e->setSize(1056 * percent / 100, 656 * percent / 100), e->setVisible(true); // getComponentAt skips hidden components
        const auto click = [&](juce::Point<float> at) {
            // Through the component tree: the peer path hit-tests the on-screen window, which stays racy on macOS CI.
            auto* c = e->getComponentAt(at.roundToInt());
            if (auto* b = dynamic_cast<juce::Button*>(c))
            {
                auto done = std::make_shared<bool>(false); // triggerClick posts a message: dispatch until a later one arrives
                b->triggerClick(), juce::MessageManager::callAsync([done] { *done = true; }), pumpUntil([&] { return *done; });
            }
            else if (auto* l = dynamic_cast<juce::Label*>(c)) // what Label::mouseUp does, minus its peer contains() test
            {
                CHECK(l->isEditableOnSingleClick() && l->isEnabled());
                l->showEditor();
            }
            CHECK(dynamic_cast<juce::Button*>(c) != nullptr || dynamic_cast<juce::Label*>(c) != nullptr);
            timer.timerCallback();
        };
        const auto centre = [&](juce::Component& c, juce::Rectangle<int> area) { return e->getLocalArea(&c, area).toFloat().getCentre(); };
        const auto at = [&](const juce::String& id) { auto* c = find(*e, id); CHECK(c != nullptr); return centre(*c, c->getLocalBounds()); };
        const auto cellsOf = [&](const juce::String& id) { const auto r = find(*e, id)->getBoundsInParent(); return juce::Rectangle<int>(r.getX() / 8, r.getY() / 16, r.getWidth() / 8, r.getHeight() / 16); };
        // Hit regions: the columns of 0.3.0 for crush, loss and low / high cut, two rows of dials.
        CHECK(cellsOf("A") == juce::Rectangle<int>(43, 1, 1, 1) && cellsOf("B") == juce::Rectangle<int>(45, 1, 1, 1) && cellsOf("randomize") == juce::Rectangle<int>(49, 1, 9, 1));
        // no page to open, no auto knob, no bypass button, no other slots, no reverb
        for (auto* gone : { "C", "D", "verb", "more", "on", "auto", "autoGain", "bypass", "fType", "cutoff" }) CHECK(find(*e, gone) == nullptr);
        const std::tuple<const char*, const char*, int, int> dials[] = { { "preamp", "drive", 3, 21 }, { "gate", "gate", 3, 28 }, { "bits", "bits", 15, 21 }, { "rate", "rate", 26, 21 }, { "jitter", "jitter", 37, 21 }, { "dither", "dither", 15, 28 },
            { "approxDev", "anti-alias", 26, 28 }, { "imagesShift", "anti-image", 37, 28 }, { "loss", "loss", 48, 21 }, { "speed", "speed", 58, 21 }, { "drop", "packet loss", 68, 21 }, { "lowCut", "low cut", 79, 21 }, { "highCut", "high cut", 89, 21 },
            { "reso", "resonance", 99, 21 }, { "mix", "mix", 110, 21 }, { "output", "output", 120, 21 } };
        // `< name >`: longest name + a cell either side + the arrows, for every selector.
        const std::tuple<const char*, const char*, juce::StringArray, int, int> selectors[] = { { "stereo", "stereo", { "stereo", "joint", "mono" }, 3, 36 }, { "dcShift", "dc shift", { "off", "on" }, 32, 36 },
            { "ear", "weighting", { "perceptual", "flat" }, 61, 30 }, { "fill", "packets", { "drop", "repeat" }, 61, 33 }, { "keep", "delta", { "off", "on" }, 61, 36 },
            { "slope", "slope", { "6 dB", "12 dB", "18 dB", "24 dB", "36 dB", "48 dB", "72 dB", "96 dB" }, 94, 30 }, { "fOrder", "position", { "pre", "post" }, 94, 33 }, { "invert", "invert", { "off", "on" }, 94, 36 } };
        juce::StringArray all { "prev", "preset", "next", "A", "B", "randomize" };
        for (auto& [id, label, x, y] : dials)
        {
            all.add(id);
            CHECK(cellsOf(id) == juce::Rectangle<int>(x, y, 10, 6) && dynamic_cast<juce::Slider*>(find(*e, id))->getTitle() == label);
        }
        for (auto& [id, label, names, x, y] : selectors)
        {
            int longest = 0;
            for (auto& n : names) longest = std::max(longest, n.length());
            const auto minus = cellsOf(juce::String(id) + "-"), plus = cellsOf(juce::String(id) + "+");
            CHECK(minus.getPosition() == juce::Point<int>(x, y) && plus.getRight() - minus.getX() == longest + 4 && plus.getY() == y);
            CHECK(dynamic_cast<juce::Button*>(find(*e, juce::String(id) + "+"))->getTitle() == juce::String("next ") + label);
            all.add(juce::String(id) + "-"), all.add(juce::String(id) + "+");
        }
        // every control is there at once: shown, inside the window, none over another, and nothing else
        for (int i = 0; i < all.size(); ++i)
        {
            auto* c = find(*e, all[i]);
            CHECK(c != nullptr && c->isVisible() && c->getParentComponent()->getLocalBounds().contains(c->getBoundsInParent()));
            for (int j = i + 1; j < all.size(); ++j) CHECK(!c->getBoundsInParent().intersects(find(*e, all[j])->getBoundsInParent()));
        }
        CHECK(find(*e, "bits")->getParentComponent()->getNumChildComponents() == all.size() && all.size() == 6 + 16 + 16);
        for (auto& [idc, label, x, y] : dials)
        {
            const juce::String id = idc;
            auto* slider = dynamic_cast<juce::Slider*>(find(*e, id));
            CHECK(slider->getSliderStyle() == juce::Slider::RotaryVerticalDrag); // a dial, no fader
            auto* param = p.apvts.getParameter(id);
            juce::Label* box = nullptr;
            for (auto* child : slider->getChildren()) box = box != nullptr ? box : dynamic_cast<juce::Label*>(child);
            CHECK(box != nullptr);
            const auto dial = centre(*slider, slider->getLocalBounds().withTrimmedBottom(box->getHeight()));
            set(p, "approxOn", 1), set(p, "imagesOn", 1);
            param->setValueNotifyingHost(0.3f);
            timer.timerCallback();
            const double before = slider->getValue();
            // Drag the Slider directly, like click().
            const auto from = slider->getLocalPoint(e.get(), dial), to = slider->getLocalPoint(e.get(), dial - juce::Point<float>(0, 50.0f * (float) percent / 100)); // 50 design px up = 0.2 of the range
            const auto time = juce::Time::getCurrentTime();
            const auto event = [&](juce::Point<float> pos, int clicks = 1) { return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys::leftButtonModifier,
                                                                                      1, 0, 0, 0, 0, slider, slider, time, from, time, clicks, pos != from); };
            slider->mouseDown(event(from)), slider->mouseDrag(event(to)), slider->mouseUp(event(to));
            CHECK(std::abs(slider->getValue() - before - 0.2 * slider->getRange().getLength()) < 0.01);
            CHECK(id == "bits" || id == "approxDev" || id == "imagesShift" || std::abs(param->getValue() - 0.5f) < 0.011f);
            slider->mouseDoubleClick(event(from, 2));
            CHECK(id == "imagesShift" ? get(p, "imagesOn") == 0 : param->getValue() == param->getDefaultValue());
            timer.timerCallback();
            click(centre(*box, box->getLocalBounds()));
            CHECK(box->isBeingEdited());
            const auto typed = param->getText(0.7f, 0);
            if (auto* editor = box->getCurrentTextEditor()) editor->setText(typed);
            box->hideEditor(false);
            CHECK(param->getCurrentValueAsText() == typed);
        }
        for (auto& [idc, label, names, x, y] : selectors)
        {
            const juce::String id = idc;
            const int n = names.size();
            set(p, idc, 0);
            for (int k = 1; k <= n; ++k) click(at(id + "+")), CHECK((int) get(p, id) == k % n);
            for (int k = 1; k <= n; ++k) click(at(id + "-")), CHECK((int) get(p, id) == (n - k) % n);
            auto* key = find(*e, id + "+"); // the arrow keys step too
            CHECK(key->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && (int) get(p, id) == 1 % n);
            CHECK(key->keyPressed(juce::KeyPress(juce::KeyPress::downKey)) && (int) get(p, id) == 0);
        }
        // off: past bits' top, under approx's and images' bottom, at gate's
        auto* bits = dynamic_cast<juce::Slider*>(find(*e, "bits"));
        auto* approx = dynamic_cast<juce::Slider*>(find(*e, "approxDev"));
        auto* images = dynamic_cast<juce::Slider*>(find(*e, "imagesShift"));
        bits->setValue(1.06, juce::sendNotificationSync), CHECK(get(p, "bits") == 0 && bits->getTextFromValue(0) == "off");
        bits->setValue(1.0, juce::sendNotificationSync), CHECK(get(p, "bits") == 24);
        bits->setValue(0.0, juce::sendNotificationSync), CHECK(get(p, "bits") == 1);
        approx->setValue(-0.06, juce::sendNotificationSync), CHECK(get(p, "approxOn") == 0 && approx->getTextFromValue(0) == "off");
        approx->setValue(0.75, juce::sendNotificationSync), CHECK(get(p, "approxOn") == 1 && get(p, "approxDev") == 50);
        images->setValue(0.0, juce::sendNotificationSync), CHECK(get(p, "imagesOn") == 1 && get(p, "imagesShift") == 0);
        images->setValue(-0.06, juce::sendNotificationSync), CHECK(get(p, "imagesOn") == 0);
        // the top bar
        p.setCurrentProgram(3);
        click(at("next")), CHECK(p.getCurrentProgram() == 4);
        click(at("prev")), click(at("prev")), CHECK(p.getCurrentProgram() == 2);
        click(at("B")), CHECK(p.abSlot() == 1);
        click(at("A")), CHECK(p.abSlot() == 0);
        click(at("randomize")), CHECK(p.presetName() == "random");
    }
    folder.deleteRecursively();
}

// A 0.2 session in the editor: the filter's controls show its filter, and editing one moves it over.
static void testLegacyEditor()
{
    Proc p;
    load(p, v2Fixture());
    p.selectAb(0);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    dynamic_cast<juce::Timer&>(*e).timerCallback();
    auto knob = [&](const char* id) { return dynamic_cast<juce::Slider*>(find(*e, id)); };
    CHECK(knob("highCut")->getTextFromValue(0) == "3.4 kHz" && knob("lowCut")->getTextFromValue(0) == "20 Hz");
    knob("lowCut")->onDragStart(), knob("lowCut")->setValue(0.3, juce::sendNotificationSync), knob("lowCut")->onDragEnd();
    CHECK(get(p, "fType") == 0 && std::abs(get(p, "highCut") - 3400) < 1 && get(p, "lowCut") > 100);
    load(p, v2Fixture()), p.selectAb(0);
    dynamic_cast<juce::Button*>(find(*e, "slope+"))->onClick();
    CHECK(get(p, "fType") == 0 && std::abs(get(p, "highCut") - 3400) < 1 && get(p, "slope") == 2);
}

// The editor draws the audio: the output's peak and the clip meters read what left the plug-in, the picture changes with the
// sound and only with it, and nothing is lit once it stops.
static void testMeter()
{
    const juce::Colour hot(0xffff2424);
    const juce::Rectangle<int> picture(24, 80, 1008, 192), light(888, 448, 144, 16), drive(888, 480, 144, 16), output(888, 528, 144, 16), peak(888, 576, 144, 16);
    Proc p;
    p.scope.clock = testClock, testMs = 0;
    set(p, cleanCrush), prepare(p, 48000, 512, 2);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    auto& timer = dynamic_cast<juce::Timer&>(*e);
    const auto shot = [&](juce::Rectangle<int> r) { return pixels(e->createComponentSnapshot(r)); };
    const auto lit = [&](juce::Rectangle<int> r) { // cells of r with a pixel of the hot ink
        const auto image = e->createComponentSnapshot(r);
        int n = 0;
        for (int cy = 0; cy < r.getHeight() / 16; ++cy)
            for (int cx = 0; cx < r.getWidth() / 8; ++cx)
            {
                bool any = false;
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 8; ++x) { const auto c = image.getPixelAt(cx * 8 + x, cy * 16 + y); any = any || (c.getRed() > 150 && c.getGreen() < 60 && c.getBlue() < 60); }
                n += any;
            }
        return n;
    };
    const auto play = [&](const Settings& s, float level, int frames = 10) {
        set(p, s);
        for (int f = 0; f < frames; ++f) run(p, 512, 2, 1600, [&](int i) { return level * (float) std::sin(i * 0.05); }), testMs += 1000.0 / 30, timer.timerCallback();
    };
    const auto stop = [&] { for (int k = 0; k < 10; ++k) testMs += 1000.0 / 30, timer.timerCallback(); };
    timer.timerCallback();
    const auto flat = shot(picture);
    CHECK(lit(picture) == 0 && lit(light) == 0 && lit(drive) == 0 && lit(output) == 0);
    play({}, 0.8f); // under full scale: nothing clips
    CHECK(shot(picture) != flat && lit(picture) == 0 && lit(light) == 0 && lit(drive) == 0 && lit(output) == 0);
    play({ { "preamp", 6 } }, 0.8f); // 1.6 x full scale: 4.2 dB off: the drive's meter and the crests
    CHECK(lit(picture) > 4 && lit(light) == 1 && lit(drive) == 4 && lit(output) == 0);
    play({ { "preamp", 48 } }, 0.8f);
    CHECK(lit(drive) == 11); // the meter's end
    play({ { "preamp", 0 }, { "output", 3 } }, 0.95f, 40); // the output past 0 dBFS by 2.5 dB: its meter and the rails
    CHECK(lit(drive) == 0 && lit(output) >= 2 && lit(output) <= 3 && lit(light) == 1 && lit(picture.withHeight(16)) > 4);
    set(p, "output", 0); // held 0.6 s, then dark, while it plays on
    play({}, 0.5f, 6), CHECK(lit(output) >= 2 && lit(light) == 1);
    play({}, 0.5f, 20), CHECK(lit(output) == 0 && lit(light) == 0);
    play({ { "output", 12 }, { "preamp", 12 } }, 0.8f);
    CHECK(lit(drive) > 0 && lit(output) > 0);
    stop();
    CHECK(shot(picture) == flat && lit(picture) == 0 && lit(light) == 0 && lit(drive) == 0 && lit(output) == 0); // stopped: the flat line, nothing lit
    const auto inf = shot(peak);
    testMs += 1000, timer.timerCallback();
    CHECK(shot(picture) == flat && shot(peak) == inf); // nothing plays, nothing moves
    play({ { "bypass", 1 }, { "preamp", 30 } }, 0.8f, 20);
    CHECK(lit(drive) == 0 && shot(peak) != inf); // bypassed: the drive is not heard
    run(p, 512, 2, 512, [](int) { return 0.5f; });
    CHECK(std::abs(p.scope.peak.load() - 0.5f) < 1e-6f);
}

static void testEditorState()
{
    Proc p;
    prepare(p, 48000, 128, 2);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    CHECK(e->getWidth() == 1056 && e->getHeight() == 656 && e->isResizable());
    for (int pct : { 50, 200, 100 }) // scaled 50-200 %, aspect kept; the size is stored for the next editor
    {
        e->setSize(1056 * pct / 100, 656 * pct / 100);
        CHECK(e->getWidth() == 1056 * pct / 100 && e->getHeight() == 656 * pct / 100 && p.editorWidth == e->getWidth());
        CHECK(e->createComponentSnapshot(e->getLocalBounds()).isValid());
    }
    e->setSize(1584, 984);
    e.reset(p.createEditor());
    CHECK(e->getWidth() == 1584 && e->getHeight() == 984);

    // Dials follow the parameters: host changes (message thread and other threads), presets, A/B, state loads.
    auto& ed = *e;
    auto& timer = dynamic_cast<juce::Timer&>(ed);
    const auto dialsMatch = [&] {
        timer.timerCallback();
        bool ok = true;
        for (auto* id : { "preamp", "rate", "jitter", "loss", "speed", "drop", "mix", "output", "gate", "dither", "reso" })
            ok = ok && std::abs(dynamic_cast<juce::Slider*>(find(ed, id))->getValue() - p.apvts.getParameter(id)->getValue()) < 1e-5;
        return ok;
    };
    CHECK(dialsMatch());
    juce::Random r(5);
    for (int n = 0; n < 40; ++n)
    {
        for (auto* id : { "preamp", "loss", "drop", "slope", "rate" })
        {
            const float v = r.nextFloat();
            if (n % 2 == 0) p.apvts.getParameter(id)->setValueNotifyingHost(v);
            else std::thread([&] { p.apvts.getParameter(id)->setValueNotifyingHost(v); }).join(); // an automation thread
        }
        CHECK(dialsMatch());
    }
    for (int i = 0; i < 41; ++i) p.setCurrentProgram(i), CHECK(dialsMatch());
    p.setCurrentProgram(28), p.selectAb(1), p.setCurrentProgram(18), CHECK(dialsMatch());
    p.selectAb(0), CHECK(dialsMatch() && get(p, "loss") == 70);
    auto* knob = dynamic_cast<juce::Slider*>(find(ed, "rate"));
    CHECK(knob != nullptr && knob->getTextFromValue(knob->getValue()) == p.apvts.getParameter("rate")->getCurrentValueAsText());
    knob->setValue(knob->getValueFromText("9.2k"), juce::sendNotificationSync);
    CHECK(get(p, "rate") == 9200);
    knob->setValue(knob->getDoubleClickReturnValue(), juce::sendNotificationSync);
    CHECK(get(p, "rate") == 26040);
    // Exercise JUCE's actual mouse shortcuts, including its implicit Alt/Option single-click default reset.
    const auto mouseClick = [&](int modifiers, int clicks) {
        knob->setValue(p.apvts.getParameter("rate")->convertTo0to1(9200), juce::sendNotificationSync);
        const auto time = juce::Time::getCurrentTime();
        const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), { 30, 30 },
                                    juce::ModifierKeys(modifiers | juce::ModifierKeys::leftButtonModifier),
                                    1, 0, 0, 0, 0, knob, knob, time, { 30, 30 }, time, clicks, false);
        if (clicks == 2) knob->mouseDoubleClick(event);
        else { knob->mouseDown(event); knob->mouseUp(event); }
        return get(p, "rate");
    };
    CHECK(mouseClick(0, 1) == 9200);
    CHECK(mouseClick(juce::ModifierKeys::altModifier, 1) == 26040);
    CHECK(mouseClick(0, 2) == 26040);
    // Restore on a host thread while dispatching real editor timers and the latency's asynchronous update.
    juce::MemoryBlock states[2];
    p.getStateInformation(states[0]);
    p.selectAb(1), p.setCurrentProgram(18), p.getStateInformation(states[1]);
    std::thread restore([&] {
        for (int i = 0; i < 20; ++i)
        {
            load(p, states[i % 2]);
            juce::Thread::sleep(1);
        }
    });
    for (int i = 0; i < 25; ++i)
    {
        pump();
        juce::Timer::callPendingTimersSynchronously();
        juce::Thread::sleep(1);
    }
    restore.join();
    pumpUntil([&] { return dialsMatch() && p.getLatencySamples() == (p.codecOn() ? 2048 : 0); });
    CHECK(dialsMatch() && p.abSlot() == 1 && p.getLatencySamples() == 0);
    e.reset();
}

// A drum + bass loop at 48 kHz: a kick (150 -> 50 Hz) on the beat, a snare (185 Hz body + noise) between, a 55 Hz bass.
static float groove(int i)
{
    const double t = i / 48000.0, kickT = std::fmod(t, 0.25), snareT = std::fmod(t + 0.125, 0.25), pi = juce::MathConstants<double>::pi;
    juce::uint32 h = (juce::uint32) i * 2654435761u;
    h = (h ^ (h >> 15)) * 0x2c1b3c6du, h ^= h >> 12;
    const double kick = std::sin(2 * pi * (50 * kickT + 2 * (1 - std::exp(-kickT / 0.02)))) * std::exp(-kickT / 0.12);
    const double snare = std::sin(2 * pi * 185 * snareT) * std::exp(-snareT / 0.08) + ((h >> 8) * 0x1p-23 - 1) * 0.15 * std::exp(-snareT / 0.01);
    return (float) (0.4 * kick + 0.45 * snare + 0.25 * std::sin(2 * pi * 55 * t) + 0.08 * std::sin(2 * pi * 110 * t + 0.5));
}

// The screenshot fixture: 56k stream after a drum + bass loop, then one second of 30 fps editor frames. Variants: main,
// clipping (driven hard, output up), quiet (no audio), crush (a crushed preset: the sample zoom).
enum Variant { mainPage, clipping, quiet, crush };
static std::unique_ptr<juce::AudioProcessorEditor> fixture(Proc& p, Variant variant = mainPage, int width = 1056)
{
    p.setCurrentProgram(variant == crush ? 3 : SCREENSHOT_PRESET);
    if (variant == crush) set(p, { { "bits", 4 }, { "rate", 6000 } });
    if (variant == clipping) set(p, { { "preamp", 14 }, { "output", 12 } });
    prepare(p, 48000, 512, 2);
    p.scope.clock = testClock, testMs = 0;
    const bool play = variant != quiet;
    if (play) run(p, 512, 2, 24000, groove);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    e->setSize(width, juce::roundToInt(width * 656 / 1056.0));
    for (int frame = 0; frame < 30; ++frame)
    {
        testMs += 1000.0 / 30;
        if (play) run(p, 512, 2, 1600, [frame](int i) { return groove(24000 + frame * 1600 + i); });
        dynamic_cast<juce::Timer&>(*e).timerCallback();
    }
    return e;
}

static int savePng(const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream out(file);
    return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out) ? 0 : 1;
}

static std::vector<float> loadFloats(const juce::String& path)
{
    juce::MemoryBlock m;
    juce::File::getCurrentWorkingDirectory().getChildFile(path).loadFileAsData(m);
    std::vector<float> v(m.getSize() / 4);
    std::memcpy(v.data(), m.getData(), v.size() * 4);
    return v;
}
// --render / --feed: the plug-in's own processor over a file, for tools/fidelity.mjs. --picture out.png width scale in.f32
// frames preset [id=value ...]: the editor after that many 1536-sample frames of the file (the site's picture, tools/qa/crumb-native.mjs).
static int picture(const juce::StringArray& a)
{
    const auto in = loadFloats(a[4]);
    const int width = a[2].getIntValue();
    Proc p;
    p.setCurrentProgram(a[6].getIntValue());
    for (int k = 7; k < a.size(); ++k) set(p, a[k].upToFirstOccurrenceOf("=", false, false).toRawUTF8(), a[k].fromFirstOccurrenceOf("=", false, false).getFloatValue());
    prepare(p, 48000, 128, 2);
    p.scope.clock = testClock, testMs = 0;
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    dynamic_cast<juce::Timer&>(*e).stopTimer();
    e->setSize(width, juce::roundToInt(width * 656 / 1056.0));
    juce::AudioBuffer<float> b(2, 128);
    juce::MidiBuffer midi;
    for (int frame = 0, at = 0; frame < a[5].getIntValue(); ++frame)
    {
        for (int k = 0; k < 12; ++k, at += 128)
        {
            for (int i = 0; i < 128; ++i)
                for (int c = 0; c < 2; ++c) b.setSample(c, i, in[(size_t) ((2 * (at + i) + c) % (int) in.size())]);
            p.processBlock(b, midi);
        }
        testMs += 32, dynamic_cast<juce::Timer&>(*e).timerCallback();
    }
    return savePng(e->createComponentSnapshot(e->getLocalBounds(), true, a[3].getFloatValue()), juce::File::getCurrentWorkingDirectory().getChildFile(a[1]));
}
static int renderFile(const juce::StringArray& args)
{
    const auto in = loadFloats(args[1]);
    const bool feed = args[0] == "--feed";
    double rate = 48000;
    Proc p;
    for (int k = 3; k < args.size(); ++k)
    {
        const auto id = args[k].upToFirstOccurrenceOf("=", false, false), v = args[k].fromFirstOccurrenceOf("=", false, false);
        if (id == "rate_hz") rate = v.getDoubleValue();
        else if (id == "preset") p.setCurrentProgram(v.getIntValue());
        else if (p.apvts.getParameter(id) != nullptr) set(p, id.toRawUTF8(), v.getFloatValue());
        else { std::cerr << "no " << id << "\n"; return 1; }
    }
    prepare(p, rate, 128, 2);
    const int frames = (int) in.size() / 2;
    juce::AudioBuffer<float> b(2, 128);
    juce::MidiBuffer midi;
    juce::MemoryOutputStream out;
    for (int start = 0; start + 128 <= frames; start += 128)
    {
        const auto w = p.scope.write.load();
        for (int i = 0; i < 128; ++i)
            for (int c = 0; c < 2; ++c) b.setSample(c, i, in[(size_t) (2 * (start + i) + c)]);
        p.processBlock(b, midi);
        for (uint32_t i = 0; i < 128; ++i)
            if (feed)
            {
                const auto at = (w + i) & (crumb::Scope::size - 1);
                for (float v : { p.scope.dry[at].load(), p.scope.wet[at].load(), p.scope.gone[at].load(), (float) p.scope.mark[at].load(), p.scope.cut[at].load(), p.scope.over[at].load() }) out.writeFloat(v);
            }
            else
                for (int c = 0; c < 2; ++c) out.writeFloat(b.getSample(c, (int) i));
    }
    return juce::File::getCurrentWorkingDirectory().getChildFile(args[2]).replaceWithData(out.getData(), out.getDataSize()) ? 0 : 1;
}

// Sound and pixel contract with 81593d3, the build before the coefficient cache, the shared minBLEP and the editor
// caches: SHA-256 of rendered audio and of editor snapshots. Captured on Debian 13 x86-64, GCC 14.2, glibc 2.41,
// JUCE's software renderer; only matching libm and renderer fingerprints enforce these absolute hashes.
static juce::String sha(const juce::MemoryOutputStream& m) { return juce::SHA256(m.getData(), m.getDataSize()).toHexString(); }
// The automation sweep: at the start of every 8th 512-sample block (85 ms: ramps and settled stretches) rate, cutoff,
// reso, approxDev, imagesShift and bits move; jitter is 30 % for blocks 96..191, then 0 (the channels' phases differ).
static void sweep(Proc& p, int b)
{
    if (b % 8 != 0)
        return;
    const double s = b / 8;
    set(p, "rate", (float) (2000 * std::pow(20.0, 0.5 + 0.5 * std::sin(0.7 * s))));
    set(p, "cutoff", (float) (200 * std::pow(100.0, 0.5 + 0.5 * std::cos(0.5 * s))));
    set(p, "reso", (float) (50 + 50 * std::sin(0.9 * s)));
    set(p, "approxDev", (float) (100 * std::sin(1.3 * s)));
    set(p, "imagesShift", (float) (50 + 50 * std::sin(0.4 * s)));
    set(p, "bits", (float) ((b / 8 * 7) % 25));
    set(p, "jitter", b >= 96 && b < 192 ? 30.0f : 0.0f);
}
// The groove (right = -0.7 x left) at 48 kHz in 512-sample blocks, every output sample hashed. program >= 0: that
// factory preset, 2 s from a settled start (auto gain off, as 0.1.x). -1: the sweep over 4 s from init with preamp 6 dB,
// dither 20 %, images on, lowpass post. -2: the same, re-prepared at 96 kHz at block 200. -3: the v1 session fixture, 2 s.
static juce::String render(int program, int channels)
{
    Proc p;
    if (program != -3)
        set(p, "autoGain", 0);
    if (program >= 0)
    {
        p.setCurrentProgram(program);
        for (auto& b : baked) if (b.program == program) set(p, { { "mix", b.mix }, { "output", b.output } }); // as 0.2 and 0.3 had it
    }
    else if (program == -3)
        load(p, v1Fixture());
    else
        set(p, { { "preamp", 6.0f }, { "dither", 20.0f }, { "imagesOn", 1.0f }, { "fType", 1.0f } });
    prepare(p, 48000, 512, channels);
    juce::AudioBuffer<float> buffer(channels, 512);
    juce::MidiBuffer midi;
    juce::MemoryOutputStream bytes;
    const bool swept = program == -1 || program == -2;
    for (int b = 0; b < (swept ? 375 : 188); ++b)
    {
        if (swept)
            sweep(p, b);
        if (program == -2 && b == 200)
            prepare(p, 96000, 512, channels);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < 512; ++i)
                buffer.setSample(c, i, groove(b * 512 + i) * (c == 0 ? 1.0f : -0.7f));
        p.processBlock(buffer, midi);
        for (int c = 0; c < channels; ++c)
            bytes.write(buffer.getReadPointer(c), 512 * sizeof(float));
    }
    return sha(bytes);
}
static juce::Image snapshot(Variant variant, int percent) // the fixture at an editor size, 2x pixels
{
    Proc p;
    const auto e = fixture(p, variant, 1056 * percent / 100);
    return e->createComponentSnapshot(e->getLocalBounds(), true, 2.0f);
}
static juce::String pixels(const juce::Image& image)
{
    const juce::Image argb = image.convertedToFormat(juce::Image::ARGB);
    const juce::Image::BitmapData d(argb, juce::Image::BitmapData::readOnly);
    juce::MemoryOutputStream m;
    for (int y = 0; y < d.height; ++y)
        m.write(d.getLinePointer(y), (size_t) (d.width * d.pixelStride));
    return sha(m);
}
struct Golden { int program, channels; const char* hash; };
[[maybe_unused]] static const Golden sounds[] = {
    { -2, 2, "6b4a7e3db119bd7953b329a5a71f07bf5c5e02f5050930ea45c399f2cc6a9e7f" },
    { -1, 2, "445d8085bf79a75d05fe72045b84b65e4c8aa564bd33a06d2f08e4b152a95311" },
    { -1, 1, "4119ae083e0464e315bb483ba720446203b872edbd4426f44fbb5bc78abb0597" },
    { -3, 2, "d161cef61d86431757df92a39862ac941fa4d5a93fe5819f42c31dc60c5350bc" }, // rendered by 0.1.x (d0329a6)
    { 0, 2, "9befbda2d1f1802ffc80fb83aa9b18bfaff9ff65a1f1b7d6eaee8e8045ff6f2e" },
    { 1, 2, "8201286ab87f8f6155c92a3f77afc490eb13130527b18c2f262927eaba6567cf" },
    { 2, 2, "cddbbccf343284426076b733aeb4e5fb425c7704b269ec2776e7c4cae6022d15" },
    { 3, 2, "0a5f81fb7826e543082c8481504d80d3aa7146abab6cbee700910980db691a6a" },
    { 4, 2, "b57fb3b7de48bc54b1041245a2d301854a4f4107e392bf2b5de3c4b3cded60e0" },
    { 5, 2, "20ec10c48bd208012aa4691732446e9537e2a7eab4d9af6d010b27b71aa6a4b1" },
    { 6, 2, "1c96c709f8f1f425e205fd0ff7a859b0110c9af41d7bf275a21fd5b811d433f7" },
    { 7, 2, "207c8748b4b37579268869a2ea55574577be87d247afa3a512db012432a77781" },
    { 8, 2, "12c942fa81196abd0c7311e5d5001912dc0c75b68b1c0d58e9db40f2df5b4e47" },
    { 9, 2, "f873954b81e969a87a89003ebcf3a51aa9f04f6b3e0eac8f91583d09a9266d5c" },
    { 10, 2, "1233cab4676deb4dcb18b7964bb1837627c65b34202b31e9326ec1f8e52c9f3e" },
    { 11, 2, "98deb8760d72eda696a018628daf102bacc34f273f29b974f0752ab8c920362c" },
    { 12, 2, "2425962ab4f3ed16c090d0cafddadf73193189401174132244129bb6a27e787c" },
    { 13, 2, "deb5a2d09f73e81cff6b3ce7be9e91942e1e241db177f0133be325c6a0bdb1a9" },
    { 14, 2, "316b5922275ce8868eaf307f474be3782ddc186ec3ebda014512817bbfd7f1ea" },
    { 15, 2, "95cfd9a6988c2ff01400487fe02c125518f5792cec852e5353e338ad7f8d4dc1" },
    { 16, 2, "81c483ba3abdfe99dd13a1a02d80fb866179ddb606f3f10a05f106e35ff0bed0" },
    { 17, 2, "aecf6ed0b128a3464ce0cec794f5d12c05cf583cf80894d7408243acb0d2e0d4" },
    { 18, 2, "d44c3e872917eac6034c05945aa92f0a12f32a07d034c897c7ce08eccfdabbc0" },
    { 19, 2, "694c5e0dcc767629da7070f9b1ab69297537b1b92b728607603bcdaa85111f34" },
    { 20, 2, "49549e25dd51d371407028e711478b009a3b05f5f526aa262a3df6dd08698d54" },
    { 21, 2, "79dcd24a86e1302fa90ae8a04ec22c94c69cc1707cc9cd4480536c428b51a258" },
    { 22, 2, "30bf28bc9557b511725e7e2dfc191c041ad8c6ce05bc300663c9ba8786de4b0d" },
    { 23, 2, "3dd38314799190f0b3a37c7cdacbc1465a9fc03864e2b7a1ba2d85def5c6e6e6" },
    { 24, 2, "4a59ca93339bccf65af8b3c5d2fead29d818041909037212529c3291d908666e" },
};
// Editor snapshots of the 0.4.0 wave UI, same environment gate as the audio.
struct Shot { Variant variant; int percent; const char* hash; };
[[maybe_unused]] static const Shot shots[] = {
    { mainPage, 100, "b4bd1bbbb7be32a547991bfd6e1bb08c3aab7fa3b6ba96bebb864256a55843a3" },
    { mainPage, 50, "9a260eb2935bff65ac8e9dbea6f3b856ae0eef9077915601fc9c3a7732202a5c" },
    { mainPage, 200, "0ca994abc4efc2b70eafc25d1a9a2f92d02e5ad70ebda9d33bc76f30489b2c8b" },
    { clipping, 100, "520190229da6b9096070aecec4972625c225d45a356537c1cdb30e82c93239f1" },
    { quiet, 100, "a85b0094b40159e322cc3de3da498429900bc077599a529cc85e693ad54f1d6d" },
    { crush, 100, "d4a4f48e6566c36d685ece1c133f0285a51f8de0c366d5e5a3f98c82ca71930d" },
};
static void testGoldens()
{
    const auto audio = render(-1, 2), image = pixels(snapshot(mainPage, 100));
    CHECK(audio == render(-1, 2));
    CHECK(image == pixels(snapshot(mainPage, 100)));
    std::cout << "Checked audio and 100 % snapshot determinism\n";
#if JUCE_LINUX && JUCE_INTEL && JUCE_64BIT
    // Independent probes: never use plug-in output as an environment fingerprint (that would hide regressions).
    juce::MemoryOutputStream math;
    for (int i = 1; i <= 256; ++i)
    {
        volatile double input = i * 0.123456789; // force runtime libm calls, not compiler constant folding
        const double x = input;
        const double values[] = { std::sin(x), std::cos(x), std::tan(x), std::exp(-x), std::exp2(x),
                                  std::log(x), std::log2(x), std::log1p(x), std::tanh(x), std::pow(x, 0.37) };
        math.write(values, sizeof(values));
    }
    juce::Image probe(juce::Image::ARGB, 128, 32, true);
    {
        juce::Graphics g(probe);
        g.setColour(juce::Colours::white);
        g.fillRoundedRectangle(0.25f, 0.75f, 127.0f, 31.0f, 4.5f);
        g.setColour(juce::Colours::black);
        g.setFont(juce::Font(juce::FontOptions(juce::Typeface::createSystemTypefaceFor(
            BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize)).withPointHeight(11.0f)));
        g.drawText("crumb 0123456789", probe.getBounds(), juce::Justification::centred, false);
    }
    const bool matchingMath = sha(math) == "07adb83962bd7f372226ea055bca705c159534fa4a19d3279488b12cd95eea5c";
    const bool matchingRenderer = pixels(probe) == "415fc224a6be9102f7093db72385bc65066fc503da351b5c5716ebcaaafe7f54";
    // Ubuntu 26 CI matched this scalar libm/renderer probe, but GCC codegen still changed processBlock bits.
    const bool ci = std::getenv("GITHUB_ACTIONS") != nullptr;
    if (matchingMath && !ci)
        for (const auto& s : sounds)
            if (render(s.program, s.channels) != s.hash)
                CHECK(!"bit-exact render"), std::cerr << "  program " << s.program << ", " << s.channels << " ch\n";
    if (matchingMath && !ci)
        for (int slot = 0; slot < 2; ++slot) // the 0.2.0 session sounds as 0.2.0 played it
        {
            Proc p;
            load(p, v2Fixture()), p.selectAb(slot);
            CHECK(playHash(p) == v2Hashes[slot]);
            Proc q;
            load(q, v3Fixture()), q.selectAb(slot);
            CHECK(playHash(q) == v3Hashes[slot]); // and 0.3.0's as 0.3.0 did
        }
    if (matchingMath && matchingRenderer && !ci) // snapshots also contain the audio-driven scopes and meters
        for (const auto& s : shots)
            if (const auto hash = pixels(snapshot(s.variant, s.percent)); hash != s.hash)
                CHECK(!"pixel-exact snapshot"), std::cerr << "  " << s.percent << " % variant " << (int) s.variant << ": " << hash << "\n";
    std::cout << "Environment fingerprints: " << sha(math) << " / " << pixels(probe) << "\nChecked "
              << (matchingMath && !ci ? std::size(sounds) : 0) << " audio hashes and "
              << (matchingMath && matchingRenderer && !ci ? std::size(shots) : 0)
              << " pixel hashes (audio from 81593d3, 0.2.0 and 0.3.0, editor from 0.4.0)"
              << (ci ? " (skipped on GitHub Actions)\n" : " (matching environments only)\n");
#endif
}
// CrumbTests --bench: processBlock ns per stereo sample frame (48 kHz, block 512, warmed; init = the defaults, the
// settled 26.04 kHz 64-tap case; the sweep). Means over repeated runs. The editor: --bench-editor.
static int bench()
{
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    for (int program : { 0, -1 })
    {
        Proc p;
        if (program < 0)
            set(p, { { "preamp", 6.0f }, { "dither", 20.0f }, { "imagesOn", 1.0f }, { "fType", 1.0f } });
        prepare(p, 48000, 512, 2);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        double total = 0;
        for (int r = -1, b = 0; r < 5; ++r) // run -1 warms up; 5 timed runs of 10 s
        {
            double elapsed = 0;
            for (int k = 0; k < 938; ++k, ++b)
            {
                if (program < 0)
                    sweep(p, b % 375);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 512; ++i)
                        buffer.setSample(c, i, groove(b * 512 + i) * (c == 0 ? 1.0f : -0.7f));
                const auto t0 = Clock::now();
                p.processBlock(buffer, midi);
                elapsed += ms(t0, Clock::now());
            }
            total += r >= 0 ? elapsed : 0;
        }
        std::cout << (program == 0 ? "processBlock init/default (26.04 kHz, 64 taps, settled)" : "processBlock automation sweep")
                  << ": " << total * 1e6 / (5.0 * 938 * 512) << " ns/sample\n";
    }
    return 0;
}

// CrumbTests --bench-editor: the fixture with the groove running, 200 frames at 100 % and 200 %: timerCallback ms, the
// paint of what it invalidated (1x pixels) and a full paint. Any editor: repaints are caught at the editor itself.
static int benchEditor()
{
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    for (int percent : { 100, 200 })
    {
        Proc p;
        auto e = fixture(p);
        e->setSize(1056 * percent / 100, 656 * percent / 100), e->setVisible(true);
        auto* capture = new RepaintCapture(*e);
        e->setCachedComponentImage(capture);
        juce::Image image(juce::Image::ARGB, e->getWidth(), e->getHeight(), true);
        double timer = 0, dirty = 0, full = 0;
        for (int frame = -20; frame < 200; ++frame)
        {
            testMs += 1000.0 / 30;
            run(p, 512, 2, 1600, [frame](int i) { return groove(60000 + frame * 1600 + i); });
            const auto t0 = Clock::now();
            dynamic_cast<juce::Timer&>(*e).timerCallback();
            const auto t1 = Clock::now();
            { juce::Graphics g(image); g.reduceClipRegion(capture->pending), capture->pending = {}, e->paintEntireComponent(g, false); }
            const auto t2 = Clock::now();
            { juce::Graphics g(image); e->paintEntireComponent(g, false); }
            if (frame >= 0) timer += ms(t0, t1), dirty += ms(t1, t2), full += ms(t2, Clock::now());
        }
        std::cout << percent << " %: timerCallback " << timer / 200 << " ms, invalidated-region paint " << dirty / 200
                  << " ms, full paint " << full / 200 << " ms\n";
        for (int frame = 0; frame < 120; ++frame)
            testMs += 1000.0 / 30, dynamic_cast<juce::Timer&>(*e).timerCallback();
        capture->pending = {};
        const auto t0 = Clock::now();
        for (int frame = 0; frame < 200; ++frame)
            testMs += 1000.0 / 30, dynamic_cast<juce::Timer&>(*e).timerCallback();
        std::cout << percent << " %: idle timerCallback " << ms(t0, Clock::now()) / 200 << " ms, repaint " << !capture->pending.isEmpty() << "\n";
    }
    return 0;
}

#if JUCE_LINUX
#include <dlfcn.h>
// JUCE ignores X errors only in its own apps (a host has them too); without that, Xlib's default handler exits on the
// benign error a window raises on a bare Xvfb. The interaction tests open a real window, so ignore them the same way.
static void ignoreXErrors()
{
    if (auto* x11 = dlopen("libX11.so.6", RTLD_NOW))
        if (auto set = (void* (*) (int (*)(void*, void*))) dlsym(x11, "XSetErrorHandler"))
            set([](void*, void*) { return 0; });
}
#endif

int main(int argc, char** argv)
{
#if JUCE_LINUX
    ignoreXErrors();
#endif
    const juce::ScopedJuceInitialiser_GUI gui;
    juce::StringArray args(argv + 1, argc - 1);
    if (args.size() >= 2 && args[0] == "--screenshot")
    {
        Proc p;
        const auto e = fixture(p, args[3] == "clip" ? clipping : args[3] == "quiet" ? quiet : args[3] == "crush" ? crush : mainPage, args.size() > 2 ? args[2].getIntValue() : 1056);
        return savePng(e->createComponentSnapshot(e->getLocalBounds()), juce::File::getCurrentWorkingDirectory().getChildFile(args[1]));
    }
    if (args.size() == 2 && args[0] == "--plates")
    {
        int bad = 0;
        for (int k = 0; k < 3; ++k) bad += savePng(crumb::plate(k), juce::File::getCurrentWorkingDirectory().getChildFile(args[1]).getChildFile(k == 0 ? "crumb-ink.png" : k == 1 ? "crumb-tint.png" : "crumb-paper.png"));
        return bad;
    }
    if (args.size() >= 7 && args[0] == "--picture") return picture(args);
    if (args.size() >= 3 && (args[0] == "--render" || args[0] == "--feed")) return renderFile(args);
    if (argc == 2 && juce::String(argv[1]) == "--bench") return bench();
    if (argc == 2 && juce::String(argv[1]) == "--bench-editor") return benchEditor();
    if (argc == 2 && juce::String(argv[1]) == "--golden-tests") { testGoldens(); return failures == 0 ? 0 : 1; }
    testGoldens();
    testIdentity();
    testState();
    testMigration();
    testBaked();
    testRandomize();
    testCodec();
    testBand();
    testWave();
    testTransactions();
    testRestoreAudio();
    testNull();
    testAlias();
    testSound();
    testConverterPath();
    testBounded();
    testAllocations();
    testEditor();
    testPresets();
    testScopeFifo();
    testClipFeed();
    testScopeRepaints();
    testEditorEdges();
    testInteraction();
    testLegacyEditor();
    testMeter();
    testEditorState();
    std::cout << (failures == 0 ? "All Crumb tests passed\n" : "Crumb tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
