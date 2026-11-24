// SnapTests: drives the real processor and editor without a host.
//   SnapTests                      run every test, exit code 1 on failure
//   SnapTests --screenshot a.png   render the editor after feeding it drums (--screenshot-browser: with the preset browser)
//   SnapTests --editor             the editor tests alone
//   SnapTests --bench              CPU benchmark
//   SnapTests --render <dir>       render the synthetic loops through a few settings to WAV files
//   SnapTests --render-file in.f32 out.f32 [--preset name] [--order ...] [id=value ...]   raw stereo render (site tools)
#include "PluginProcessor.h"
#include "ui/Braille.h"
#include "ui/PresetBrowser.h"
#include "ui/Layout.h"
#include "ui/Riso.h"
#include "ui/Visualizers.h"
#include "ui/Widgets.h"
#include "presets/PresetStore.h"
#include "presets/StateIO.h"
#include "chain/Latency.h"
#include "loops/Loops.h"
#include "dsp/modules/Drive.h"
#include "dsp/modules/Press.h"
#include "dsp/modules/Push.h"
#include "dsp/modules/Shift.h"
#include "Check.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

// Real-time check: count heap allocations made on a thread flagged as "audio". operator new is
// counted everywhere; on glibc (outside ASan, which owns malloc) the C allocators are too, which
// catches JUCE's HeapBlock / AudioBuffer storage.
static thread_local bool onAudioThread = false;
static std::atomic<int> audioAllocations { 0 };
static void countAllocation() { audioAllocations += onAudioThread ? 1 : 0; }
void setAudioThread(bool on) { onAudioThread = on; }
int takeAudioAllocations() { return audioAllocations.exchange(0); }
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#define SNAP_COUNTS_MALLOC 1
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
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
static void escape(void* p) { asm volatile("" : : "r"(p) : "memory"); } // p and its memory become observable

using Proc = SnapProcessor;

static void set(Proc& p, std::string_view id, float v)
{
    auto* param = p.apvts.getParameter(juce::String(id.data(), id.size()));
    param->setValueNotifyingHost(param->convertTo0to1(v));
}
static float get(Proc& p, std::string_view id) { return p.apvts.getRawParameterValue(juce::String(id.data(), id.size()))->load(); }
static bool near(float a, float b) { return std::abs(a - b) <= 1.0e-5f * std::max(1.0f, std::abs(b)); }
static float defaultOf(Proc& p, std::string_view id)
{
    auto* param = p.apvts.getParameter(juce::String(id.data(), id.size()));
    return param->convertFrom0to1(param->getDefaultValue());
}
static bool isModuleSwitch(std::string_view id)
{
    return std::any_of(snap::modules.begin(), snap::modules.end(), [&](auto& m) { return m.onId == id; });
}
// Every parameter at its default; `modulesOff` expects the six module switches off instead (a migrated v1 session).
static bool defaults(Proc& p, bool modulesOff = false)
{
    for (auto& s : snap::parameterTable())
        if (get(p, s.id) != (modulesOff && isModuleSwitch(s.id) ? 0.0f : defaultOf(p, s.id)))
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

// Kick (decaying 60 Hz sine) and snare (decaying noise) alternating every 250 ms. Sample i is always the same value,
// so separate renders can be compared.
static float drum(int i, double fs)
{
    const double t = std::fmod(i / fs, 0.5), u = t < 0.25 ? t : t - 0.25;
    return t < 0.25 ? (float) (0.9 * std::sin(2 * juce::MathConstants<double>::pi * 60 * u) * std::exp(-u / 0.08))
                    : (float) (0.6 * loops::noise((juce::uint32) i) * std::exp(-u / 0.05));
}

// Processes `seconds` of drums scaled by `level`; returns channel 0 out, advanced by the reported latency, and (via `in`) channel 0 in.
static std::vector<float> run(Proc& p, double fs, int block, int ch, double seconds, std::vector<float>* in = nullptr, bool poison = false,
                              float level = 1)
{
    juce::AudioBuffer<float> buffer(ch, block);
    juce::MidiBuffer midi;
    std::vector<float> out, input;
    for (int start = 0; start < (int) (seconds * fs); start += block)
    {
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i)
                buffer.setSample(c, i, level * drum(start + i, fs) * (c == 0 ? 1.0f : -0.7f));
        if (poison && (start / block) % 3 == 1)
            buffer.setSample(0, block / 2, start % 2 ? NAN : INFINITY), buffer.setSample(ch - 1, 0, -INFINITY);
        for (int i = 0; i < block; ++i)
            input.push_back(buffer.getSample(0, i));
        onAudioThread = true;
        p.processBlock(buffer, midi);
        onAudioThread = false;
        for (int i = 0; i < block; ++i)
            out.push_back(buffer.getSample(0, i));
    }
    if (in != nullptr)
        *in = input;
    out.erase(out.begin(), out.begin() + std::min((long) p.getLatencySamples(), (long) out.size())); // aligned with the input
    return out;
}

static float peak(const std::vector<float>& x, size_t from, size_t to)
{
    float m = 0;
    for (size_t i = from; i < std::min(to, x.size()); ++i)
        m = std::max(m, std::abs(x[i]));
    return m;
}
static double energy(const std::vector<float>& x, size_t from, size_t to)
{
    double e = 0;
    for (size_t i = from; i < std::min(to, x.size()); ++i)
        e += (double) x[i] * x[i];
    return e;
}
static float maxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    float d = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}
static juce::MemoryBlock blob(const juce::String& xml) // the copyXmlToBinary layout, text verbatim
{
    juce::MemoryOutputStream m;
    m.writeInt(0x21324356), m.writeInt((int) xml.getNumBytesAsUTF8()), m.write(xml.toRawUTF8(), xml.getNumBytesAsUTF8());
    return m.getMemoryBlock();
}
static void load(Proc& p, const juce::MemoryBlock& m) { p.setStateInformation(m.getData(), (int) m.getSize()); }

static void testState()
{
    juce::Random r(42);
    for (int n = 0; n < 50; ++n) // (1) random round trip into a brand-new instance, chain order included
    {
        Proc a, b, c;
        for (auto* param : a.getParameters())
            param->setValueNotifyingHost(r.nextFloat());
        auto order = a.order();
        std::swap(order[(size_t) r.nextInt(4)], order[(size_t) r.nextInt(4)]);
        a.setOrder(order);
        CHECK(!defaults(a));
        juce::MemoryBlock m, m2, m3;
        a.getStateInformation(m);
        load(b, m);
        b.getStateInformation(m2);
        load(c, m2);
        c.getStateInformation(m3);
        CHECK(m2 == m3 && b.order() == a.order()); // log-mapped values may move by one ulp once, then stay put
        for (auto& s : snap::parameterTable())
            CHECK(near(get(b, s.id), get(a, s.id)));
        CHECK(juce::AudioProcessor::getXmlFromBinary(m.getData(), (int) m.getSize())->getIntAttribute("stateVersion") == 2);
    }

    // (2) Frozen v1 fixture, exactly as snap 0.1.x writes it. Must load in every future version: Attack and Sustain
    // scaled by the old Mix, Output onto the master Output, every module switched off, everything else at default.
    Proc v1;
    const auto fixture = blob(R"(<SNAP stateVersion="1"><PARAM id="attack" value="60.0"/><PARAM id="sustain" value="-40.0"/>)"
                              R"(<PARAM id="mix" value="75.0"/><PARAM id="output" value="-3.5"/></SNAP>)");
    set(v1, "drive_amt", 50), set(v1, "magnitude", 20), v1.setOrder({ snap::Module::drive, snap::Module::press, snap::Module::space, snap::Module::shift });
    load(v1, fixture);
    CHECK(get(v1, "attack") == 45.0f && get(v1, "sustain") == -30.0f && get(v1, "out_gain") == -3.5f && v1.order() == snap::defaultChainOrder);
    set(v1, "attack", 0), set(v1, "sustain", 0), set(v1, "out_gain", 0);
    CHECK(defaults(v1, true));
    load(v1, fixture);
    // v1 ranges and defaults apply to v1 data: Output was -24..+12 dB; a missing Mix was 100 %.
    Proc w;
    load(w, blob(R"(<SNAP stateVersion="1"><PARAM id="attack" value="1e9"/><PARAM id="mix" value="abc"/><PARAM id="output" value="40"/></SNAP>)"));
    CHECK(get(w, "attack") == 100.0f && get(w, "sustain") == 0.0f && get(w, "out_gain") == 12.0f);
    for (auto* bad : { "12abc", "1-2", "5.5.5", "--3", "1e", ".", "&#x", "1e2147483648", "-1e4294967296" }) // not wholly a (finite) number
    {
        load(w, fixture);
        load(w, blob(juce::String(R"(<SNAP stateVersion="1"><PARAM id="attack" value="X"/><PARAM id="sustain" value="X"/><PARAM id="mix" value="X"/>)"
                                  R"(<PARAM id="output" value="X"/></SNAP>)").replace("X", bad)));
        CHECK(defaults(w, true));
        juce::String v2 = "<SNAP stateVersion=\"2\">";
        for (auto& s : snap::parameterTable())
            v2 << "<PARAM id=\"" << s.id << "\" value=\"" << bad << "\"/>";
        load(w, fixture);
        load(w, blob(v2 + "</SNAP>"));
        CHECK(defaults(w));
    }
    // A blob without stateVersion is a 0.1.x one.
    load(w, blob(R"(<SNAP><PARAM id="attack" value=" 1e0000000000000000002 "/><PARAM id="sustain" value="-1e-4294967296"/></SNAP>)"));
    CHECK(get(w, "attack") == 100.0f && get(w, "sustain") == 0.0f && get(w, "drive_on") == 0.0f);
    // v2 and later: unknown entries ignored, out-of-range clamped, missing and unparsable back to default, bad order to default.
    load(w, blob(R"(<SNAP stateVersion="7" future="x" order="press,press,space,drive"><PARAM id="attack" value="1e9"/>)"
                 R"(<PARAM id="out_gain" value="abc"/><PARAM id="in_gain" value="nan"/><PARAM id="wobble" value="3"/><PARAM id="mix" value="0"/>)"
                 R"(<PARAM id="hiss_type" value="17.4"/><PARAM id="cut_lo" value="1"/><EXTRA/></SNAP>)"));
    CHECK(get(w, "attack") == 100.0f && get(w, "out_gain") == 0.0f && get(w, "in_gain") == 0.0f && get(w, "drive_on") == 1.0f);
    CHECK(get(w, "hiss_type") == 17.0f && get(w, "cut_lo") == 20.0f && w.order() == snap::defaultChainOrder);

    // Hostile or incomplete blobs (deep nesting, DTD entities, truncated character references) leave the state alone.
    load(v1, blob("<SNAP>" + juce::String::repeatedString("<a>", 1000000)));
    load(v1, blob(R"(<!DOCTYPE SNAP [<!ENTITY a "&a;&a;">]><SNAP><PARAM id="attack" value="&a;"/></SNAP>)"));
    for (auto* bad : { "<SNAP>&#x", "<SNAP>&#", "<SNAP>&#12", "<SNAP>&#x4", "<SNAP><PARAM id=\"attack\" value=\"&#x", "</SNAP>", "<SNAP><a></SNAP>" })
        load(v1, blob(bad));
    CHECK(get(v1, "attack") == 45.0f && get(v1, "out_gain") == -3.5f);
    // Future-writer states restore: long escaped text (any payload length, e.g. 8508 = "<!" in the length field),
    // and a nested tree holding PARAM-like entries, which only top-level PARAMs may set.
    bool hit8508 = false;
    juce::MemoryBlock plain;
    v1.getStateInformation(plain);
    for (int extra = 8300 - (int) plain.getSize(); extra < 8700 - (int) plain.getSize(); extra += 1)
    {
        auto tree = v1.apvts.copyState();
        juce::ValueTree slot("SLOT");
        slot.appendChild(juce::ValueTree("PARAM").setProperty("id", "attack", nullptr).setProperty("value", -7, nullptr), nullptr);
        tree.addChild(slot, 0, nullptr);
        tree.setProperty("stateVersion", 2, nullptr).setProperty("notes", juce::String::repeatedString("x", extra) + juce::String::fromUTF8("<&>\"\n\xc3\xa9"), nullptr);
        juce::MemoryBlock m;
        juce::AudioProcessor::copyXmlToBinary(*tree.createXml(), m);
        hit8508 = hit8508 || juce::ByteOrder::littleEndianInt(m.begin() + 4) == 8508;
        Proc f;
        load(f, m);
        CHECK(get(f, "attack") == 45.0f && get(f, "sustain") == -30.0f && get(f, "out_gain") == -3.5f && get(f, "press_on") == 0.0f);
    }
    CHECK(hit8508);

    // (3) Garbage, empty, truncated and foreign data never crash and leave defaults.
    Proc g;
    juce::MemoryBlock good;
    v1.getStateInformation(good);
    g.setStateInformation(nullptr, 0);
    g.setStateInformation(good.getData(), 0);
    for (size_t cut = 1; cut < good.getSize() - 8; ++cut) // the last bytes only hold the closing tag
        g.setStateInformation(good.getData(), (int) cut);
    for (int n = 0; n < 200; ++n)
    {
        juce::MemoryBlock junk((size_t) r.nextInt(300));
        r.fillBitsRandomly(junk.getData(), junk.getSize());
        if (n % 2 == 0 && junk.getSize() >= 8) // a valid header with a random payload
            std::memcpy(junk.getData(), good.getData(), 8);
        g.setStateInformation(junk.getData(), (int) junk.getSize());
    }
    load(g, blob(R"(<ROOTNOTE><PARAM id="attack" value="50"/></ROOTNOTE>)"));
    CHECK(defaults(g));
    for (int n = 0; n < 5000; ++n) // a real state with random bytes overwritten
    {
        juce::MemoryBlock m(good);
        for (int k = 0; k <= n % 4; ++k)
            static_cast<char*>(m.getData())[r.nextInt((int) m.getSize())] = (char) r.nextInt(256);
        g.setStateInformation(m.getData(), (int) m.getSize());
    }
}

static void testMigratedSound() // a v1 session maps onto 0.2: neutral = exact pass-through, Mix scales Attack/Sustain
{
    auto render = [](const juce::String& xml, std::vector<float>* in = nullptr) {
        Proc p;
        load(p, blob(xml));
        prepare(p, 48000, 256, 2);
        return run(p, 48000, 256, 2, 1.0, in);
    };
    std::vector<float> in;
    const auto neutral = render(R"(<SNAP stateVersion="1"><PARAM id="attack" value="0.0"/><PARAM id="sustain" value="0.0"/>)"
                                R"(<PARAM id="mix" value="100.0"/><PARAM id="output" value="0.0"/></SNAP>)", &in);
    CHECK(maxDiff(neutral, in) == 0);
    // Mix 50 % with Attack 80 is the same as a v2 session with Attack 40.
    const auto halved = render(R"(<SNAP stateVersion="1"><PARAM id="attack" value="80"/><PARAM id="mix" value="50"/></SNAP>)");
    const auto direct = render(R"(<SNAP stateVersion="2"><PARAM id="attack" value="40"/></SNAP>)");
    CHECK(maxDiff(halved, direct) == 0 && maxDiff(halved, in) > 0.01f);
}

static void testChainOrder()
{
    using snap::Module;
    CHECK(snap::toString(snap::defaultChainOrder) == "shift,space,press,drive");
    const snap::ChainOrder reordered { Module::drive, Module::shift, Module::press, Module::space };
    CHECK(snap::parseChainOrder("drive,shift,press,space") == reordered);
    for (auto* bad : { "", "shift", "shift,space,press", "shift,space,press,drive,", "shift,space,press,drive,push", "shift,shift,press,drive",
                       "sub/hiss,space,press,drive", "shift,space,press,push", "shift, space,press,drive", ",shift,space,press" })
        CHECK(!snap::parseChainOrder(bad));
}

static void testIdentity() // (4)
{
    CHECK(JucePlugin_PluginCode == 0x536e6170);       // 'Snap'
    CHECK(JucePlugin_ManufacturerCode == 0x44776b73); // 'Dwks'
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_AUMainType)) == "'aufx'");
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_CFBundleIdentifier)) == "com.dripworks.snap");
    CHECK(juce::String(JucePlugin_Name) == "snap" && juce::String(JucePlugin_Manufacturer) == "daveydidit");
    CHECK(juce::String(JucePlugin_VersionString) == "0.5.2");
    // The wrappers derive the VST3 class IDs and the AU/VST3 numeric parameter IDs from the codes and IDs only under these settings.
    static_assert(JUCE_VST3_CAN_REPLACE_VST2 == 0 && JUCE_FORCE_USE_LEGACY_PARAM_IDS == 0, "frozen host-facing IDs");
    CHECK(!JucePlugin_IsSynth && !JucePlugin_WantsMidiInput);

    // The parameter table, in host order. IDs from 0.1.x keep version hint 1; 0.1.x's "mix" and "output" are retired, never reused.
    Proc p;
    juce::StringArray found, expected;
    for (auto& s : snap::parameterTable())
        expected.add(s.id);
    for (auto* param : p.getParameters())
    {
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(param);
        CHECK(withId != nullptr && withId->getVersionHint() == (withId->paramID == "attack" || withId->paramID == "sustain" ? 1 : withId->paramID == "press_attack" ? 3 : 2));
        found.add(withId->paramID);
    }
    CHECK(found == expected && found.size() == 87 && found[86] == "press_attack" && !found.contains("mix") && !found.contains("output"));
    auto unique = found;
    unique.removeDuplicates(false);
    CHECK(unique == found);
    // Numeric IDs hosts store automation under: VST3 ParamID and AU parameter ID, both hashes of the string ID.
    CHECK(juce::VST3ClientExtensions::convertJuceParameterId("attack") == 0x2c1eee48 && juce::VST3ClientExtensions::convertJuceParameterId("sustain") == 0x119e6223);

    // Magnitude scales exactly the six amounts and the master continuous controls; four controls are not automatable.
    const juce::StringArray scaled { "sub_amt", "hiss_amt", "shift_amt", "space_amt", "press_amt", "drive_amt", "push_amt", "in_gain",
                                     "out_gain", "cut_lo", "cut_hi", "tone", "air", "attack", "sustain" };
    const juce::StringArray fixed { "low_latency", "oversampling", "space_decay", "space_stretch" };
    for (auto& s : snap::parameterTable())
    {
        CHECK(s.magnitude == scaled.contains(s.id));
        CHECK(p.apvts.getParameter(s.id)->isAutomatable() == !fixed.contains(s.id));
    }
    CHECK(p.getBypassParameter() == p.apvts.getParameter("bypass"));
    // Spot-check ranges and defaults from the spec.
    CHECK(defaults(p) && get(p, "magnitude") == 100 && near(get(p, "sub_det_lo"), 30) && near(get(p, "hiss_det_hi"), 8000) && get(p, "sub_pitch") == 31);
    CHECK(near(get(p, "sub_decay"), 300) && get(p, "space_width") == 100 && get(p, "press_downup") == -1 && get(p, "cut_hi") == 20000);
    CHECK(near(get(p, "space_focus_lo"), 150) && near(get(p, "space_focus_hi"), 12000) && get(p, "subhiss_on") == 1 && get(p, "clip_on") == 0);
    CHECK(p.apvts.getParameter("hiss_type")->getNumSteps() == 18 && p.apvts.getParameter("space_type")->getNumSteps() == 20);
    CHECK(p.apvts.getParameter("tone_shape")->getText(1.0f, 32) == "band focus" && p.apvts.getParameter("cut_lo")->getText(1.0f, 32) == "2.00 kHz");
    CHECK(p.getTailLengthSeconds() > 5 && p.getTailLengthSeconds() < 20 && !p.acceptsMidi() && !p.producesMidi() && p.getNumPrograms() == 1);
}

static void testLatency() // one constant latency; Low Latency Mode reports exactly 64 samples
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        Proc p;
        prepare(p, fs, 256, 2);
        const int normal = p.getLatencySamples();
        CHECK(normal == snap::chainLatency(fs, false) && normal >= snap::Shift::latencyFor(fs, false) && normal > 64);
        for (auto& m : snap::modules) // switches and amounts never move it
        {
            set(p, m.onId, 0), set(p, m.amountIds[0], 50);
            CHECK(p.getLatencySamples() == normal);
        }
        set(p, "low_latency", 1);
        CHECK(p.getLatencySamples() == 64 && snap::Shift::latencyFor(fs, true) == 0);
        CHECK(snap::SubHiss::latencyFor(fs, true) + 2 * snap::TargetFocusMix::lookaheadFor(fs, true) + snap::Master::latencyFor(fs, true) <= 64);
        prepare(p, fs, 256, 2);
        CHECK(p.getLatencySamples() == 64);
        set(p, "low_latency", 0);
        CHECK(p.getLatencySamples() == normal);
    }
    for (bool low : { false, true }) // the dry path lands exactly on the reported latency, also when bypassed
        for (bool bypassed : { false, true })
        {
            Proc p;
            set(p, "low_latency", low), set(p, "bypass", bypassed), set(p, "shift_amt", 0);
            prepare(p, 48000, 100, 2);
            std::vector<float> in;
            const auto out = run(p, 48000, 100, 2, 0.5, &in);
            CHECK(maxDiff(out, in) == 0 && out.size() + (size_t) p.getLatencySamples() == in.size());
        }
}

static void testMasterInProcessor() // processor level: exact neutral paths, and one Loudness Match click lands within 1 dB
{
    for (auto& [id, v] : std::initializer_list<std::pair<const char*, float>> { { "eq_on", 0 }, { "lc_shape", 1 }, { "hc_shape", 1 }, { "tone_shape", 3 } })
    {
        Proc p;
        set(p, id, v);
        prepare(p, 48000, 100, 2);
        std::vector<float> in;
        CHECK(maxDiff(run(p, 48000, 100, 2, 0.3, &in), in) == 0);
    }
    for (float magnitude : { 100.0f, 40.0f })
    {
        Proc p;
        set(p, "magnitude", magnitude), set(p, "in_gain", 9), set(p, "tone", 60), set(p, "air", 70), set(p, "attack", 50), set(p, "clip_on", 1);
        prepare(p, 48000, 512, 2);
        const auto loop = loops::make(1, 48000, 12);
        juce::AudioBuffer<float> block(2, 512);
        juce::MidiBuffer midi;
        snap::LoudnessMeter in, out;
        in.prepare(48000, 2), out.prepare(48000, 2);
        for (int start = 0; start + 512 <= loop.getNumSamples(); start += 512)
        {
            if (start == 6 * 48000 / 512 * 512)
                p.matchLoudness();
            for (int c = 0; c < 2; ++c)
                block.copyFrom(c, 0, loop, c, start, 512);
            in.process(block, 512);
            p.processBlock(block, midi);
            out.process(block, 512);
        }
        CHECK(std::abs(out.lufs() - in.lufs()) <= 1.0f && std::abs(get(p, "out_gain")) > 1.0f);
    }
}

// Every module and the sub/hiss Post bus in the processor: switches, Magnitude 0 and bypass give back the dry signal, Post
// meets the chain in time, and the full chain stays finite and allocation-free while modes and rates change.
static void testFullChain()
{
    const std::pair<const char*, float> pushed[] = { { "sub_amt", 70 }, { "hiss_amt", 60 }, { "shift_amt", 40 }, { "space_amt", 50 },
                                                     { "press_amt", 60 }, { "drive_amt", 60 }, { "push_amt", 60 },
                                                     { "sub_sens", 90 }, { "hiss_sens", 90 }, { "space_type", 11 } };
    auto make = [&](std::initializer_list<std::pair<const char*, float>> extra, bool low = false) {
        auto p = std::make_unique<Proc>();
        for (auto& [id, v] : pushed)
            set(*p, id, v);
        for (auto& [id, v] : extra)
            set(*p, id, v);
        set(*p, "low_latency", low);
        prepare(*p, 48000, 256, 2);
        return p;
    };
    for (bool low : { false, true })
    {
        std::vector<float> in;
        auto off = make({ { "subhiss_on", 0 }, { "shift_on", 0 }, { "space_on", 0 }, { "press_on", 0 }, { "drive_on", 0 }, { "push_on", 0 } }, low);
        CHECK(maxDiff(run(*off, 48000, 256, 2, 1.0, &in), in) <= 1.0e-6f); // all modules off: <= -120 dB
        CHECK(maxDiff(run(*make({ { "bypass", 1 } }, low), 48000, 256, 2, 1.0), in) <= 1.0e-6f);
        CHECK(maxDiff(run(*make({ { "magnitude", 0 } }, low), 48000, 256, 2, 1.0), in) <= 1.0e-3f);

        // Post vs Pre with only sub/hiss on: the layers land on the same samples either way.
        auto solo = [&](float post) {
            auto p = make({ { "shift_on", 0 }, { "space_on", 0 }, { "press_on", 0 }, { "drive_on", 0 }, { "push_on", 0 }, { "sub_post", post },
                            { "hiss_post", post } }, low);
            return run(*p, 48000, 256, 2, 1.0);
        };
        const auto pre = solo(0);
        CHECK(maxDiff(pre, in) > 0.01f && maxDiff(solo(1), pre) <= 1.0e-5f);
    }
    for (double fs : { 44100.0, 96000.0 })
        for (int block : { 1, 64, 511 })
        {
            auto p = make({ { "sub_post", 1 }, { "clip_on", 1 }, { "air", 50 } });
            prepare(*p, fs, block, 2);
            bool ok = true;
            for (int k = 0; k < 3; ++k)
            {
                set(*p, "low_latency", k == 1), set(*p, "space_type", (float) (k * 7)), set(*p, "drive_mode", (float) k);
                for (float y : run(*p, fs, block, 2, 0.4))
                    ok = ok && std::isfinite(y) && std::abs(y) <= 8.0f;
            }
            CHECK(ok && audioAllocations.exchange(0) == 0);
        }
}

static void testMagnitude()
{
    CHECK(snap::applyMagnitude(snap::spec("attack"), 80, 0.5f) == 40 && snap::applyMagnitude(snap::spec("attack"), 80, 0) == 0);
    CHECK(std::abs(snap::applyMagnitude(snap::spec("cut_lo"), 2000, 0.5f) - 200) < 0.01f && snap::applyMagnitude(snap::spec("cut_hi"), 1000, 0) == 20000);
    CHECK(snap::applyMagnitude(snap::spec("press_mix"), 30, 0) == 30); // not scaled

    // Magnitude 0 with everything pushed, input peaking at -6 dBFS: the output stays the dry signal (<= -60 dB residual).
    Proc p;
    set(p, "magnitude", 0), set(p, "attack", 100), set(p, "sustain", -100), set(p, "in_gain", 18), set(p, "out_gain", -12);
    prepare(p, 48000, 256, 2);
    std::vector<float> in;
    const auto out = run(p, 48000, 256, 2, 1.0, &in, false, 0.5f / 0.9f);
    CHECK(maxDiff(out, in) <= 0.001f && peak(in, 0, in.size()) <= 0.5f);
}

static void testBounded() // (5) and (10)
{
    const char* ids[] = { "attack", "sustain", "in_gain", "out_gain" };
    const float settings[][4] = { { 100, 100, 24, 24 }, { -100, -100, -24, 24 }, { 100, -100, 0, -24 }, { -100, 100, 24, 0 } };
    for (double fs : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int ch : { 1, 2 })
            for (int block : { 1, 7, 64, 511, 1023, 4096 })
                for (auto& s : settings)
                {
                    Proc p;
                    for (int k = 0; k < 4; ++k)
                        set(p, ids[k], s[k]);
                    prepare(p, fs, block, ch);
                    bool ok = true;
                    for (float y : run(p, fs, block, ch, block == 1 ? 0.1 : 0.4, nullptr, true))
                        ok = ok && std::isfinite(y) && std::abs(y) <= 4000.0f; // input peak <= 1, max gain 72 dB
                    CHECK(ok);
                }
    for (double fs : { 0.0, -48000.0, 1.0e300, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
    {
        Proc q; // a broken host's sample rate must not break anything (UBSan: no invalid casts)
        set(q, "attack", 100), set(q, "sustain", 100);
        prepare(q, fs, 64, 2);
        bool ok = true;
        for (float y : run(q, 48000, 64, 2, 0.1))
            ok = ok && std::isfinite(y) && std::abs(y) <= 64.0f;
        CHECK(ok);
    }
    Proc p; // repeated prepare / release with changing rates and block sizes
    set(p, "attack", 100);
    for (int n = 0; n < 30; ++n)
    {
        const double fs = std::array<double, 4> { 22050, 44100, 96000, 192000 }[(size_t) n % 4];
        const int block = 1 + (n * 397) % 2048;
        prepare(p, fs, block, 1 + n % 2);
        for (float y : run(p, fs, block, 1 + n % 2, 0.05))
            CHECK(std::isfinite(y));
        if (n % 3 == 0)
            p.releaseResources();
    }
}

static void testSound() // (6), (7), (8)
{
    const double fs = 48000;
    auto render = [&](float attack, float sustain, std::vector<float>* in = nullptr, bool bypass = false) {
        Proc p;
        set(p, "attack", attack), set(p, "sustain", sustain), set(p, "bypass", bypass);
        prepare(p, fs, 256, 2);
        return run(p, fs, 256, 2, 2.0, in);
    };
    std::vector<float> in;
    const auto unity = render(0, 0, &in);
    CHECK(maxDiff(unity, in) == 0); // all modules off by default amounts: bit-exact pass-through
    CHECK(maxDiff(render(100, 100, nullptr, true), in) == 0);

    // Kick at 1 s: transient = first 10 ms, tail = 60..200 ms after the hit.
    const size_t hit = (size_t) fs, head = hit + (size_t) (0.01 * fs), t0 = hit + (size_t) (0.06 * fs), t1 = hit + (size_t) (0.2 * fs);
    CHECK(peak(render(100, 0), hit, head) > 1.5f * peak(unity, hit, head));
    CHECK(peak(render(-100, 0), hit, head) < 0.7f * peak(unity, hit, head));
    CHECK(energy(render(0, 100), t0, t1) > 2.0 * energy(unity, t0, t1));
    CHECK(energy(render(0, -100), t0, t1) < 0.5 * energy(unity, t0, t1));


    // (8) No allocation inside processBlock: in every rate / layout / block size run so far, and below while
    // parameters move. The counter itself is checked. The probes must survive the optimiser (GCC 15 elides a
    // new/delete expression pair even through a volatile pointer): they call the replaceable functions
    // directly, which is never elidable, and escape every pointer through an opaque asm.
    CHECK(audioAllocations.exchange(0) == 0);
    auto counted = [](auto allocate) {
        onAudioThread = true;
        allocate();
        onAudioThread = false;
        return audioAllocations.exchange(0) > 0;
    };
    CHECK(counted([] { void* q = ::operator new(sizeof(int)); escape(q); ::operator delete(q); }));
    CHECK(counted([] { void* q = ::operator new(64, std::align_val_t(64)); escape(q); ::operator delete(q, std::align_val_t(64)); }));
#ifdef SNAP_COUNTS_MALLOC
    CHECK(counted([] { void* q = std::malloc(64); escape(q); std::free(q); }));
    CHECK(counted([] { juce::AudioBuffer<float> b(2, 512); escape(b.getWritePointer(0)); })); // JUCE's malloc buffers
#endif
    Proc p;
    prepare(p, fs, 512, 2);
    for (int n = 0; n < 20; ++n)
    {
        set(p, "attack", (float) (n * 10 - 100)), set(p, "sustain", (float) (100 - n * 10)), set(p, "out_gain", (float) n - 12);
        set(p, "in_gain", (float) -n), set(p, "magnitude", (float) n * 5), set(p, "bypass", n % 7 == 3);
        run(p, fs, 512, 2, 0.05);
    }
    CHECK(audioAllocations.load() == 0);
}

#if JUCE_LINUX
namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue(bool returnIfNoPendingMessages); // JUCE's message loop step (Linux)
}
#endif

// The editor's widgets, found by title (the accessible name) or component ID.
template <typename T> static T* find(juce::Component& root, const juce::String& name)
{
    if (auto* t = dynamic_cast<T*>(&root); t != nullptr && (root.getTitle() == name || root.getComponentID() == name))
        return t;
    for (auto* child : root.getChildren())
        if (auto* t = find<T>(*child, name))
            return t;
    return nullptr;
}
static void click(juce::Component& root, const juce::String& title, juce::ModifierKeys mods = {})
{
    auto* k = find<snap::ui::Key>(root, title);
    CHECK(k != nullptr);
    if (k != nullptr)
        k->onClick(mods);
}
static void tick(juce::AudioProcessorEditor& e)
{
    if (auto* t = dynamic_cast<juce::Timer*>(&e))
        t->timerCallback();
}

// (9) The v1 editor: opens and closes repeatedly while audio runs (no audio-thread allocation with the 30 Hz feed
// running), scales 75-200 % at a fixed aspect, every dial reads and writes its parameter, keys toggle, choose and lock,
// the browser opens over the columns and cancels back, snapshots, undo and reorder work, and typed values parse.
// The braille cell writer: U+2800 bit order, dots snapped to the 8 x 16 lattice, merge / under / over between strokes.
static void testBraille()
{
    using namespace snap::ui;
    CHECK(brailleBit(0, 0) == 0x01 && brailleBit(0, 2) == 0x04 && brailleBit(1, 0) == 0x08 && brailleBit(1, 2) == 0x20);
    CHECK(brailleBit(0, 3) == 0x40 && brailleBit(1, 3) == 0x80);
    const juce::Colour red { 0xffff0000 }, white { 0xffffffff };
    // Paints a red dot top-left, then (a new stroke) a white one top-right of the same cell; returns those two pixels.
    auto two = [&](Mode mode) {
        juce::Image image(juce::Image::ARGB, 16, 16, true);
        {
            juce::Graphics g(image);
            Braille b(g);
            b.dot(1, 1, red), b.stroke(), b.dot(6, 3, white, mode);
        }
        return std::pair { image.getPixelAt(2, 2), image.getPixelAt(6, 2) }; // dots at x 2.4 / 5.6, y 2
    };
    auto [mergeL, mergeR] = two(Mode::merge);
    CHECK(mergeL.getRed() > 200 && mergeL.getGreen() > 200 && mergeR.getGreen() > 200); // or'd, one colour per cell
    auto [overL, overR] = two(Mode::over);
    CHECK(overL.getAlpha() == 0 && overR.getGreen() > 200);
    auto [underL, underR] = two(Mode::under);
    CHECK(underL.getRed() > 200 && underL.getGreen() == 0 && underR.getAlpha() == 0);
    juce::Image image(juce::Image::ARGB, 24, 40, true);
    {
        juce::Graphics g(image);
        Braille b(g);
        b.dot(13, 31, white);                         // lands on cell (1, 1), dot (1, 3): x 8 + 5.6, y 16 + 14
        b.box({ 0, 0, 8, 8 }, red);                   // a dotted outline on the lattice
    }
    CHECK(image.getPixelAt(13, 30).getGreen() > 200 && image.getPixelAt(13, 31).getAlpha() > 0);
    CHECK(image.getPixelAt(2, 2).getRed() > 200 && image.getPixelAt(5, 6).getRed() > 200);
}

// The riso ink field: deterministic, one ink per 240 px column (push grey), bright neutrals in paper ink, others untouched.
static void testRiso()
{
    using namespace snap::ui;
    CHECK(riso::field().getWidth() == 1440 && riso::field().getHeight() == 840 && riso::backdrop().getWidth() == 1440 && riso::backdrop().getHeight() == 808);
    CHECK(riso::ink(colours::accent, { 700, 300 }) == riso::ink(colours::accent, { 700, 300 }));
    const auto nearest = [](juce::Colour c) { // the ink this pixel was printed from (the field dims inks by up to 12 %)
        int best = 0;
        float bestDistance = 1e9f;
        for (int k = 0; k < 6; ++k)
        {
            const auto i = riso::inks[k];
            const float d = juce::square(c.getFloatRed() - 0.94f * i.getFloatRed()) + juce::square(c.getFloatGreen() - 0.94f * i.getFloatGreen())
                          + juce::square(c.getFloatBlue() - 0.94f * i.getFloatBlue());
            if (d < bestDistance)
                bestDistance = d, best = k;
        }
        return best;
    };
    for (int k = 0; k < 6; ++k)
    {
        int hits = 0, n = 0;
        for (int y = 0; y < 840; y += 7, ++n)
            hits += nearest(riso::ink(colours::accent, { 240.0f * (float) k + 120, (float) y })) == k ? 1 : 0;
        CHECK(hits * 10 > n * 7); // at least 70 % of the column's centre line
    }
    const auto push = riso::ink(colours::accent, { 1320, 420 });
    CHECK(push.getRed() == push.getGreen() && push.getGreen() == push.getBlue());
    CHECK(riso::ink(colours::accent.withAlpha(0.5f), { 10, 10 }).getAlpha() == colours::accent.withAlpha(0.5f).getAlpha());
    CHECK(riso::ink(colours::primary, { 10, 10 }) == riso::paper);
    CHECK(riso::ink(colours::muted, { 10, 10 }) == colours::muted && riso::ink(colours::dim, { 10, 10 }) == colours::dim);
    const int gy = (int) snap::ui::layout::amountTop + 50; // the amount dials' centre line
    const auto glow = [gy](int x0) { // any ink in the 120 px glow box behind a column's amount dial
        for (int y = gy - 60; y < gy + 60; ++y)
            for (int x = x0 - 60; x < x0 + 60; ++x)
                if (const auto c = riso::backdrop().getPixelAt(x, y); c.getBlue() != c.getRed())
                    return true;
        return false;
    };
    CHECK(glow(132) && glow(1072) && !glow(1307));
    // The glow ends at the dial ring's outer edge: no ink in the box outside that circle.
    for (const int cx : { 132, 367, 602, 837, 1072 })
    {
        bool outside = false;
        for (int y = gy - 60; y < gy + 60; ++y)
            for (int x = cx - 60; x < cx + 60; ++x)
                if (const auto c = riso::backdrop().getPixelAt(x, y); c.getBlue() != c.getRed() && std::hypot(x - cx + 0.5, y - gy + 0.5) > riso::dialGlowEdge + 1)
                    outside = true;
        CHECK(!outside);
    }
}


// Each module's on/off switch: switched off mid-render, the output ramps (no step larger than the material's own) and
// within 50 ms matches the module never having been on; the editor dims the column and stills its visualizer.
static void testModuleSwitches()
{
    const std::pair<const char*, std::vector<std::pair<const char*, float>>> cases[] = {
        { "subhiss_on", { { "sub_amt", 70 }, { "hiss_amt", 70 } } }, { "shift_on", { { "shift_amt", 60 } } },
        { "space_on", { { "space_amt", 60 } } }, { "press_on", { { "press_amt", 80 } } },
        { "drive_on", { { "drive_amt", 80 } } }, { "push_on", { { "push_amt", 80 } } } };
    const double fs = 48000;
    const int block = 256, total = (int) fs, at = (int) (0.5 * fs) / block * block;
    for (auto& [onId, values] : cases)
    {
        auto render = [&](bool switching, bool on) {
            Proc p;
            for (auto& [id, v] : values)
                set(p, id, v);
            set(p, onId, on ? 1.0f : 0.0f);
            prepare(p, fs, block, 2);
            juce::AudioBuffer<float> b(2, block);
            juce::MidiBuffer midi;
            std::vector<float> out;
            for (int start = 0; start < total; start += block)
            {
                if (switching && start == at)
                    set(p, onId, 0);
                for (int i = 0; i < block; ++i)
                    b.setSample(0, i, drum(start + i, fs)), b.setSample(1, i, -0.7f * drum(start + i, fs));
                p.processBlock(b, midi);
                for (int i = 0; i < block; ++i)
                    out.push_back(b.getSample(0, i));
            }
            return out;
        };
        const auto switched = render(true, true), off = render(false, false), on = render(false, true);
        const size_t settled = (size_t) (at + 0.05 * fs + 2 * 2685);
        float after = 0, step = 0, own = 0;
        for (size_t i = settled; i < switched.size(); ++i)
            after = std::max(after, std::abs(switched[i] - off[i]));
        for (size_t i = 1; i < switched.size(); ++i)
            step = std::max(step, std::abs(switched[i] - switched[i - 1])), own = std::max({ own, std::abs(on[i] - on[i - 1]), std::abs(off[i] - off[i - 1]) });
        if (!(after < 1.0e-4f && step <= own * 1.05f))
            std::printf("switch %s: after %.3g, step %.3g vs %.3g\n", onId, after, step, own);
        CHECK(after < 1.0e-4f);
        CHECK(step <= own * 1.05f);
        CHECK(maxDiff(on, off) > 1.0e-3f); // the module was doing something to switch off
    }
}


// Audibility (snap 0.5): every control on the editor must make a clearly audible change on drums. Each control's
// min, mid and max (every choice, both toggle states) are rendered on the synthetic kit loop with its module at a typical
// amount; the widest change between any two (for a selector: the weakest choice against the default) must reach 2 dB on
// the largest of: level, the three one-third-octave bands that moved most (within 50 dB of the loudest), the 5 ms
// envelope (within 40 dB of its peak), half the side/mid width change, half the left/right balance change. The same
// measure and contexts as the site's tools/snap-audit.mjs (on the site's drum loop), which writes snap-audit.md.
namespace audibility
{
struct Measure { double rms = 0, mid = 0, side = 0, left = 0, right = 0; std::vector<double> bands, env; };
static Measure analyse(const juce::AudioBuffer<float>& y)
{
    constexpr int N = 4096, skip = 9600;
    const int n = y.getNumSamples();
    Measure m;
    std::vector<double> mid((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double l = y.getSample(0, i), r = y.getSample(1, i);
        mid[(size_t) i] = 0.5 * (l + r);
        if (i >= skip)
            m.mid += 0.25 * (l + r) * (l + r), m.side += 0.25 * (l - r) * (l - r), m.left += l * l, m.right += r * r;
    }
    m.rms = std::sqrt((m.left + m.right) / (2.0 * (n - skip)));
    std::vector<double> bandHz;
    for (double f = 40; f <= 16000; f *= std::pow(2.0, 1.0 / 3))
        bandHz.push_back(f);
    m.bands.assign(bandHz.size(), 0.0);
    juce::dsp::FFT fft(12);
    std::vector<float> buf(2 * N);
    for (int s = skip; s + N <= n; s += N / 2)
    {
        for (int i = 0; i < N; ++i)
            buf[(size_t) i] = (float) (mid[(size_t) (s + i)] * (0.5 - 0.5 * std::cos(2 * juce::MathConstants<double>::pi * i / N)));
        std::fill(buf.begin() + N, buf.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform(buf.data());
        for (int k = 1; k < N / 2; ++k)
            if (const int b = (int) std::floor(std::log2(k * 48000.0 / N / 40 * std::pow(2.0, 1.0 / 6)) * 3); b >= 0 && b < (int) bandHz.size())
                m.bands[(size_t) b] += (double) buf[(size_t) k] * buf[(size_t) k];
    }
    for (int s = skip; s + 240 <= n; s += 240)
    {
        double q = 0;
        for (int i = s; i < s + 240; ++i)
            q += (double) y.getSample(0, i) * y.getSample(0, i) + (double) y.getSample(1, i) * y.getSample(1, i);
        m.env.push_back(10 * std::log10(q / 480 + 1e-12));
    }
    return m;
}
static double effect(const juce::AudioBuffer<float>& ya, const juce::AudioBuffer<float>& yr)
{
    const auto a = analyse(ya), r = analyse(yr);
    const auto dB = [](double x) { return 10 * std::log10(x + 1e-20); };
    const double level = std::abs(20 * std::log10((a.rms + 1e-12) / (r.rms + 1e-12)));
    double top = 0;
    for (size_t b = 0; b < r.bands.size(); ++b)
        top = std::max({ top, r.bands[b], a.bands[b] });
    std::vector<double> changes;
    for (size_t b = 0; b < r.bands.size(); ++b)
        if (std::max(r.bands[b], a.bands[b]) > top * 1e-5)
            changes.push_back(std::abs(dB(a.bands[b]) - dB(r.bands[b])));
    std::sort(changes.rbegin(), changes.rend());
    double spectrum = 0;
    for (size_t k = 0; k < std::min<size_t>(3, changes.size()); ++k)
        spectrum += changes[k] / (double) std::min<size_t>(3, changes.size());
    double peak = -1e9, es = 0;
    int ec = 0;
    for (double v : r.env) peak = std::max(peak, v);
    for (double v : a.env) peak = std::max(peak, v);
    for (size_t k = 0; k < std::min(a.env.size(), r.env.size()); ++k)
        if (std::max(a.env[k], r.env[k]) > peak - 40)
            es += std::abs(std::max(a.env[k], peak - 60) - std::max(r.env[k], peak - 60)), ++ec;
    const double envelope = ec ? es / ec : 0;
    const double width = std::abs(std::max(-40.0, dB(a.side / a.mid)) - std::max(-40.0, dB(r.side / r.mid)));
    const double balance = std::abs(dB(a.left / a.right) - dB(r.left / r.right));
    return std::max({ level, spectrum, envelope, width / 2, balance / 2 });
}
} // namespace audibility

static void testAudibility()
{
#if defined(__SANITIZE_ADDRESS__)
    return; // minutes of renders: the Release suite covers it
#else
    using Values = std::vector<std::pair<const char*, float>>;
    const Values sub { { "sub_amt", 70 } }, hiss { { "hiss_amt", 70 } }, shift { { "shift_amt", 80 } }, space { { "space_amt", 50 } },
        press { { "press_amt", 70 } }, drive { { "drive_amt", 70 } }, push { { "push_amt", 70 } };
    const auto with = [](Values a, const Values& b) { a.insert(a.end(), b.begin(), b.end()); return a; };
    const std::vector<std::pair<const char*, Values>> controls = {
        { "magnitude", {} }, { "bypass", {} }, { "subhiss_on", with(sub, hiss) }, { "sub_amt", {} }, { "hiss_amt", {} }, { "sub_sens", sub },
        { "hiss_sens", hiss }, { "sub_det_lo", sub }, { "sub_det_hi", sub }, { "hiss_det_lo", hiss }, { "hiss_det_hi", hiss }, { "sub_pitch", sub },
        { "sub_decay", sub }, { "hiss_decay", hiss }, { "sub_duck", sub }, { "hiss_duck", hiss }, { "sub_stereo", sub }, { "sub_color", sub },
        { "sub_acid", sub }, { "hiss_type", hiss }, { "hiss_pitch", hiss }, { "hiss_tone", hiss }, { "hiss_env", hiss }, { "hiss_res", hiss },
        { "sub_solo", sub }, { "sub_post", with(sub, { { "drive_amt", 80 }, { "press_amt", 80 } }) }, { "hiss_solo", hiss },
        { "hiss_post", with(hiss, { { "shift_amt", 80 }, { "space_amt", 50 } }) }, { "shift_on", shift }, { "shift_amt", {} },
        { "shift_balance", shift }, { "shift_invert", with(shift, { { "shift_balance", 0 } }) }, { "shift_stereo", shift },
        { "shift_stretch", shift }, { "shift_fmix", shift }, { "shift_target", shift }, { "shift_focus_lo", shift }, { "shift_focus_hi", shift },
        { "space_on", space }, { "space_amt", {} }, { "space_type", space }, { "space_decay", space }, { "space_stretch", space },
        { "space_predelay", space }, { "space_duck", space }, { "space_pan", space }, { "space_width", space }, { "space_focus_lo", space },
        { "space_focus_hi", space }, { "press_on", press }, { "press_amt", {} }, { "press_mode", press }, { "press_downup", press },
        { "press_mix", press }, { "press_bands", press }, { "press_attack", press }, { "drive_on", drive }, { "drive_amt", {} },
        { "drive_mode", drive }, { "drive_flavor", drive }, { "drive_mix", drive }, { "drive_target", drive }, { "drive_focus_lo", drive },
        { "drive_focus_hi", drive }, { "push_on", push }, { "push_amt", {} }, { "push_mult", push }, { "push_tone", push }, { "push_mix", push },
        { "push_target", push }, { "push_focus_lo", push }, { "push_focus_hi", push }, { "in_gain", {} }, { "out_gain", {} },
        { "eq_on", { { "cut_lo", 150 }, { "tone", 60 } } }, { "cut_lo", {} }, { "cut_hi", {} }, { "lc_shape", { { "cut_lo", 200 } } },
        { "hc_shape", { { "cut_hi", 4000 } } }, { "tone", {} }, { "tone_shape", { { "tone", 60 } } }, { "air", {} }, { "attack", {} },
        { "sustain", {} }, { "clip_on", { { "in_gain", 12 } } } };
    const auto dry = loops::make(2, 48000, 6);
    const snap::PresetStore presets(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("snap-audibility-no-user-presets"));
    const auto* hitHard = presets.find(juce::String("factory/") + snap::defaultPresetName);
    const auto render = [&](const Values& values, bool preset) {
        Proc p;
        if (preset && hitHard != nullptr)
            snap::loadPresetXml(p, hitHard->xml);
        for (auto& [id, v] : values)
            set(p, id, v);
        prepare(p, 48000, 512, 2);
        auto wet = dry;
        juce::MidiBuffer midi;
        for (int start = 0; start < wet.getNumSamples(); start += 512)
        {
            juce::AudioBuffer<float> block(wet.getArrayOfWritePointers(), 2, start, std::min(512, wet.getNumSamples() - start));
            p.processBlock(block, midi);
        }
        return wet;
    };
    int weak = 0;
    for (auto& [id, context] : controls)
    {
        const auto& s = snap::spec(id);
        const bool preset = std::string_view(id) == "magnitude" || std::string_view(id) == "bypass";
        std::vector<float> values;
        if (s.unit == snap::Unit::toggle)
            values = { 0, 1 };
        else if (s.unit == snap::Unit::choice)
            for (size_t k = 0; k < s.choices.size(); ++k)
                values.push_back((float) k);
        else
        {
            float lo = s.lo, hi = s.hi;
            if (std::string_view(id).starts_with("sub_det") || std::string_view(id).starts_with("hiss_det")) // up to the other edge's default
            {
                const auto other = juce::String(id).endsWith("_lo") ? juce::String(id).replace("_lo", "_hi") : juce::String(id).replace("_hi", "_lo");
                (juce::String(id).endsWith("_lo") ? hi : lo) = snap::spec(other.toStdString()).def;
            }
            const float mid = s.logScale() ? std::sqrt(lo * hi) : 0.5f * (lo + hi);
            values = { lo, s.unit == snap::Unit::note ? std::round(mid) : mid, hi };
        }
        std::vector<juce::AudioBuffer<float>> out;
        for (float v : values)
            out.push_back(render(with(context, { { id, v } }), preset));
        double best = 0;
        if (s.unit == snap::Unit::choice) // the weakest choice against the default
        {
            best = 1e9;
            for (size_t k = 0; k < values.size(); ++k)
                if (k != (size_t) s.def)
                    best = std::min(best, audibility::effect(out[k], out[(size_t) s.def]));
        }
        else
            for (size_t a = 0; a < out.size(); ++a)
                for (size_t b = a + 1; b < out.size(); ++b)
                    best = std::max(best, audibility::effect(out[b], out[a]));
        if (best < 2.0)
            ++weak, std::printf("audibility: %s changes the kit loop by %.2f dB\n", id, best);
        CHECK(best >= 2.0);
    }
    std::printf("audibility: %d of %d controls under 2 dB\n", weak, (int) controls.size());
#endif
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
                b.setSample(0, i, drum(n * 128 + i, 48000)), b.setSample(1, i, drum(n * 128 + i, 48000));
            onAudioThread = true;
            p.processBlock(b, midi);
            onAudioThread = false;
        }
    });
    for (int n = 0; n < 8; ++n)
    {
        std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
        CHECK(e != nullptr && e->getWidth() == 1440 && e->getHeight() == 808);
        set(p, "attack", (float) (n * 8 - 100)), set(p, "press_amt", (float) n * 10);
        for (int t = 0; t < 10; ++t)
            tick(*e), juce::Thread::sleep(3);
        CHECK(e->createComponentSnapshot(e->getLocalBounds()).isValid());
    }
    stop = true;
    audio.join();
    CHECK(audioAllocations.exchange(0) == 0);

    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    auto& ed = *e;
    for (const auto& [w, h] : { std::pair { 1080, 606 }, std::pair { 2880, 1616 } })
    {
        ed.setSize(w, h);
        CHECK(ed.getWidth() == w && ed.getHeight() == h);
        CHECK(ed.createComponentSnapshot(ed.getLocalBounds()).isValid());
    }
    ed.setBoundsConstrained({ 0, 0, 500, 500 }); // the constrainer keeps the limits and the aspect
    CHECK(ed.getWidth() >= 1080 && std::abs(ed.getWidth() * 808 - ed.getHeight() * 1440) <= 1440);
    ed.setSize(1440, 808);

    // Every parameter but the session toggles has a widget; dials parse typed text with units.
    int controls = 0;
    std::function<void(juce::Component&)> count = [&](juce::Component& c) {
        controls += dynamic_cast<snap::ui::Control*>(&c) != nullptr;
        for (auto* child : c.getChildren())
            count(*child);
    };
    count(ed);
    CHECK(controls >= 50);

    // A pointer reaches every visible key and control: the component under the middle of each (and of each quarter,
    // for the wide ones) is that widget or one of its children, at 100 % and at the 75 % and 150 % scales.
    ed.setVisible(true);
    for (const int width : { 1440, 1080, 2160 })
    {
        ed.setSize(width, width * 808 / 1440);
        tick(ed);
        int widgets = 0, missed = 0;
        std::function<void(juce::Component&)> probe = [&](juce::Component& c) {
            if (!c.isVisible())
                return;
            if (dynamic_cast<snap::ui::Key*>(&c) != nullptr || dynamic_cast<snap::ui::Control*>(&c) != nullptr)
            {
                ++widgets;
                const auto b = c.getLocalBounds().toFloat();
                for (const float fx : { 0.5f, 0.25f, 0.75f })
                {
                    const auto at = ed.getLocalPoint(&c, juce::Point<float>(b.getX() + b.getWidth() * fx, b.getCentreY())).roundToInt();
                    auto* under = ed.getComponentAt(at);
                    if (under != &c && !c.isParentOf(under))
                    {
                        ++missed;
                        std::printf("  hit miss at %d px: %s (%s) at %d,%d -> %s\n", width, c.getTitle().toRawUTF8(), c.getComponentID().toRawUTF8(), at.x, at.y,
                                    under != nullptr ? (under->getTitle() + " / " + under->getComponentID()).toRawUTF8() : "nothing");
                        break;
                    }
                }
            }
            for (auto* child : c.getChildren())
                probe(*child);
        };
        for (auto* child : ed.getChildren())
            probe(*child);
        std::printf("  hit test at %d px: %d widgets, %d missed\n", width, widgets, missed);
        CHECK(widgets >= 90 && missed == 0);
    }
    ed.setVisible(false), ed.setSize(1440, 808);
    using snap::ui::parse;
    CHECK(parse(snap::spec("out_gain"), juce::String::fromUTF8("\xe2\x88\x92" "5.3 dB")) == -5.3f);
    CHECK(parse(snap::spec("space_predelay"), "0.012 s") == 12.0f);
    CHECK(parse(snap::spec("cut_hi"), "12k") == 12000.0f && parse(snap::spec("cut_hi"), "12 kHz") == 12000.0f);
    CHECK(parse(snap::spec("drive_flavor"), "-20%") == -0.2f && parse(snap::spec("drive_flavor"), "0.5") == 0.5f);
    CHECK(parse(snap::spec("press_mode"), "glue") == 1.0f && !parse(snap::spec("air"), "loud"));
    CHECK(snap::ui::format(snap::spec("out_gain"), -1.5f) == juce::String::fromUTF8("\xe2\x88\x92" "1.5 dB"));
    for (bool bold : { false, true }) // the bundled faces draw every UI glyph themselves: no system fallback
    {
        const auto font = snap::ui::mono(12, bold);
        CHECK(!font.getFallbackEnabled());
        for (auto c : juce::String::fromUTF8("-\xc2\xb7\xe2\x88\x92")) // hyphen, middle dot, minus
            CHECK(font.getTypefacePtr()->getNominalGlyphForCodepoint(c).has_value());
    }
    if (auto* air = find<snap::ui::Control>(ed, "air"))
    {
        air->edit();
        auto* field = find<juce::TextEditor>(*air, "");
        if (field == nullptr)
            for (auto* child : air->getChildren())
                field = dynamic_cast<juce::TextEditor*>(child);
        CHECK(field != nullptr && field->isVisible());
        if (field != nullptr)
            field->setText("42%"), air->commit();
        CHECK(std::abs(p.apvts.getRawParameterValue("air")->load() - 42) < 0.01f);
        CHECK(p.undo() && p.apvts.getRawParameterValue("air")->load() == 0);
    }
    else
        CHECK(false);

    // Keys: a toggle, a choice word, a lock, the match key, snapshots, undo.
    click(ed, "clip");
    CHECK(p.apvts.getRawParameterValue("clip_on")->load() > 0.5f);
    click(ed, "next press mode");
    CHECK(p.apvts.getRawParameterValue("press_mode")->load() == 1);
    click(ed, "previous press mode");
    CHECK(p.apvts.getRawParameterValue("press_mode")->load() == 0);
    click(ed, "press lock");
    CHECK(p.locked((int) snap::Module::press));
    click(ed, "press lock");
    CHECK(!p.locked((int) snap::Module::press));
    click(ed, "next tone shape"), click(ed, "next tone shape");
    CHECK(p.apvts.getRawParameterValue("tone_shape")->load() == 2);

    // v3 layout, 0.5.1 top bar (as silt's): < preset > A B C D randomize save undo redo on one row, magnitude by the menu,
    // master in / out are the big dials, segmented choices step with the arrow keys (clamped) and the `< >` selectors wrap.
    {
        using snap::ui::Key;
        auto *save = find<Key>(ed, "save preset"), *undo = find<Key>(ed, "undo"), *redo = find<Key>(ed, "redo"), *menu = find<Key>(ed, "menu");
        auto *next = find<Key>(ed, "next preset"), *a = find<Key>(ed, "snapshot A"), *d = find<Key>(ed, "snapshot D"), *random = find<Key>(ed, "random preset");
        auto* magnitude = find<snap::ui::Control>(ed, "magnitude");
        CHECK(save != nullptr && undo != nullptr && redo != nullptr && menu != nullptr && magnitude != nullptr && next != nullptr && a != nullptr && d != nullptr
              && random != nullptr);
        if (save != nullptr && undo != nullptr && redo != nullptr && menu != nullptr && magnitude != nullptr && next != nullptr && a != nullptr && d != nullptr
            && random != nullptr)
        {
            CHECK(undo->getY() == save->getY() && redo->getY() == save->getY() && a->getY() == save->getY() && random->getY() == save->getY());
            CHECK(a->getX() > next->getRight() && d->getX() > a->getRight() && random->getX() > d->getRight() && save->getX() > random->getRight()
                  && undo->getX() >= save->getRight() && redo->getX() >= undo->getRight());
            CHECK(magnitude->getX() > redo->getRight() + 300 && menu->getX() - magnitude->getRight() < 64);
        }
        CHECK(find<Key>(ed, "bypass") == nullptr && find<Key>(ed, "store snapshot") == nullptr); // the help line's bar is gone
        for (const char* id : { "in_gain", "out_gain" })
        {
            auto* big = find<snap::ui::Control>(ed, id);
            CHECK(big != nullptr && big->knob == 72 && big->getHeight() == 100);
        }
        // 0.5.2 (David's notes 3, 4, 6, 8): press and drive modes, push's stages and the tone shape are `< name >` selectors as
        // space's type: the arrows step through every value in order and wrap at both ends (click and arrow keys); the old
        // choice cells and pan pad are gone, and space has a width dial.
        for (const auto& [id, what, n] : { std::tuple { "press_mode", "press mode", 6 }, std::tuple { "drive_mode", "drive mode", 6 },
                                            std::tuple { "push_mult", "push stages", 3 }, std::tuple { "tone_shape", "tone shape", 4 } })
        {
            auto *before = find<Key>(ed, juce::String("previous ") + what), *after = find<Key>(ed, juce::String("next ") + what);
            CHECK(before != nullptr && after != nullptr);
            if (before == nullptr || after == nullptr)
                continue;
            CHECK(before->getY() == after->getY() && after->getX() > before->getRight());
            const auto value = [&, id = id] { return juce::roundToInt(p.apvts.getRawParameterValue(id)->load()); };
            const int was = value();
            set(p, id, 0);
            for (int i = 1; i <= n; ++i)
                click(ed, juce::String("next ") + what), CHECK(value() == i % n);
            click(ed, juce::String("previous ") + what);
            CHECK(value() == n - 1);
            after->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
            CHECK(value() == 0);
            before->keyPressed(juce::KeyPress(juce::KeyPress::leftKey));
            CHECK(value() == n - 1);
            set(p, id, (float) was);
        }
        for (const char* gone : { "press_mode glue", "drive_mode tape", "push_mult 2x", "tone shape mid", "space pan and width" })
            CHECK(find<Key>(ed, gone) == nullptr && find<juce::Component>(ed, gone) == nullptr);
        auto *width = find<snap::ui::Control>(ed, "space_width"), *duck = find<snap::ui::Control>(ed, "space_duck"), *stretch = find<snap::ui::Control>(ed, "space_stretch");
        CHECK(width != nullptr && duck != nullptr && stretch != nullptr && find<snap::ui::Control>(ed, "space_pan") == nullptr);
        if (width != nullptr && duck != nullptr && stretch != nullptr)
            CHECK(width->getY() == duck->getY() && width->getX() == stretch->getX() && width->getBounds().getHeight() == stretch->getHeight());
        set(p, "space_type", 0);
        click(ed, "previous space type");
        CHECK(p.apvts.getRawParameterValue("space_type")->load() == 19);
        click(ed, "next space type");
        CHECK(p.apvts.getRawParameterValue("space_type")->load() == 0);
    }
    click(ed, "snapshot B"); // empty: stores
    CHECK(p.hasSnapshot(1));
    set(p, "air", 70);
    click(ed, "snapshot B"); // stored: recalls
    CHECK(p.apvts.getRawParameterValue("air")->load() == 0);
    click(ed, "undo");
    CHECK(p.apvts.getRawParameterValue("air")->load() == 70);
    click(ed, "sub/hiss on");
    CHECK(p.apvts.getRawParameterValue("subhiss_on")->load() < 0.5f);
    tick(ed);
    CHECK(ed.createComponentSnapshot(ed.getLocalBounds()).isValid()); // dimmed column

    // The browser covers only the module columns; Esc closes it and leaves the sound alone.
    auto* browser = find<snap::ui::PresetBrowser>(ed, "preset browser");
    CHECK(browser != nullptr && !browser->isVisible());
    click(ed, "preset browser");
    CHECK(browser != nullptr && browser->isVisible() && browser->getBounds() == juce::Rectangle<int>(24, 58, 1392, (int) snap::ui::layout::columnH + 2));
    if (browser != nullptr)
        browser->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    CHECK(browser != nullptr && !browser->isVisible());
    CHECK(p.apvts.getRawParameterValue("clip_on")->load() > 0.5f && p.apvts.getRawParameterValue("tone_shape")->load() == 2);
    click(ed, "next preset");
    CHECK(p.apvts.state.getProperty("preset").toString().isNotEmpty());

    // Review fixes. Cancel undoes the auditions and live edits since the browser opened, adds no undo step and
    // restores the name; keyboard browsing auditions; ok keeps it, and undo / redo keep the name with the sound.
    if (browser != nullptr)
    {
        const auto key = p.apvts.state.getProperty("preset").toString();
        const auto before = std::vector<float> { p.apvts.getRawParameterValue("air")->load(), p.apvts.getRawParameterValue("sub_amt")->load(),
                                                 p.apvts.getRawParameterValue("press_amt")->load() };
        click(ed, "preset browser");
        const int depth = p.undoManager.getUndoDescriptions().size();
        browser->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        browser->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        CHECK(p.apvts.state.getProperty("preset").toString() != key);
        set(p, "air", 55); // a live edit in the amount row / master while the browser is open
        p.beginUndoStep();
        browser->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        CHECK(!browser->isVisible() && p.apvts.state.getProperty("preset").toString() == key);
        CHECK(p.apvts.getRawParameterValue("air")->load() == before[0] && p.apvts.getRawParameterValue("sub_amt")->load() == before[1]
              && p.apvts.getRawParameterValue("press_amt")->load() == before[2]);
        CHECK(p.undoManager.getUndoDescriptions().size() <= depth);

        click(ed, "preset browser");
        browser->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        const auto chosen = p.apvts.state.getProperty("preset").toString();
        browser->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        CHECK(!browser->isVisible() && chosen != key && p.apvts.state.getProperty("preset").toString() == chosen);
        CHECK(p.undo() && p.apvts.state.getProperty("preset").toString() == key);
        CHECK(p.redo() && p.apvts.state.getProperty("preset").toString() == chosen);
        click(ed, "preset browser"), click(ed, "preset browser"); // the name key closes it like ok
        CHECK(!browser->isVisible());
    }

    // The preset stepper keeps its place: < and > stay put whatever the name, and the box fits the longest factory name.
    {
        auto *prev = find<snap::ui::Key>(ed, "previous preset"), *next = find<snap::ui::Key>(ed, "next preset"), *name = find<snap::ui::Key>(ed, "preset browser");
        CHECK(prev != nullptr && next != nullptr && name != nullptr);
        if (prev != nullptr && next != nullptr && name != nullptr)
        {
            const auto prevAt = prev->getBounds(), nextAt = next->getBounds();
            int longest = 0;
            for (int i = 0; i < 60; ++i)
            {
                click(ed, "next preset"), tick(ed);
                CHECK(prev->getBounds() == prevAt && next->getBounds() == nextAt);
                const snap::PresetStore names;
                if (const auto* now = names.find(p.apvts.state.getProperty("preset").toString()))
                    longest = std::max(longest, now->name.length());
            }
            CHECK(longest >= 20 && (float) name->getWidth() >= snap::ui::textWidth(juce::String::repeatedString("x", longest)));
        }
    }

    // Automation from the audio thread with the editor open: no allocation (the editor polls, it never listens).
    {
        onAudioThread = true;
        for (int n = 0; n < 50; ++n)
            for (const char* id : { "air", "attack", "press_amt", "drive_flavor", "space_pan", "cut_lo", "magnitude", "sub_amt" })
                p.apvts.getParameter(id)->setValueNotifyingHost((float) ((n * 7) % 11) / 10.0f);
        onAudioThread = false;
        CHECK(audioAllocations.exchange(0) == 0);
        tick(ed);
    }

    // A latency toggle changed off the message thread is reported from the message thread.
    {
        const int normal = p.getLatencySamples();
        std::thread([&] { p.apvts.getParameter("low_latency")->setValueNotifyingHost(1); }).join();
        CHECK(p.getLatencySamples() == normal);
#if JUCE_LINUX
        while (juce::detail::dispatchNextMessageOnSystemQueue(true)) // the pending async update runs here
            ;
        CHECK(p.getLatencySamples() == 64);
#endif
        set(p, "low_latency", 0);
        CHECK(p.getLatencySamples() == normal);
    }

    // Reorder by order change: the columns follow on the next tick.
    p.setOrder({ snap::Module::drive, snap::Module::press, snap::Module::space, snap::Module::shift });
    tick(ed);
    auto* drive = find<juce::Component>(ed, "drive");
    CHECK(drive != nullptr && drive->getX() == 24 + 235);

    // A new instance knows it is on the default preset.
    std::unique_ptr<juce::AudioProcessor> fresh(createPluginFilter());
    CHECK(static_cast<Proc&>(*fresh).apvts.state.getProperty("preset").toString() == juce::String("factory/") + snap::defaultPresetName);

    // Two stores on one library: interleaved edits both survive.
    juce::TemporaryFile dir;
    {
        snap::PresetStore a(dir.getFile()), b(dir.getFile());
        a.setFavorite("factory/x", true), b.setFavorite("factory/y", true);
        a.addCollection("kits"), b.addCollection("loops"), a.addToCollection("loops", "factory/z");
        snap::PresetStore c(dir.getFile());
        CHECK(c.isFavorite("factory/x") && c.isFavorite("factory/y") && c.collections().size() == 2 && c.collectionsOf("factory/z").contains("loops"));
    }
    dir.getFile().deleteRecursively();

    // Any valid XML spelling loads the same; a preset without a version is a preset, not a 0.1.x session.
    Proc q;
    q.setLocked((int) snap::Module::press, true), set(q, "press_amt", 33), set(q, "bypass", 1);
    CHECK(snap::loadPresetXml(q, "<SNAP name = 'x' order='press,shift,space,drive'><PARAM id = 'air' value = '25'/><PARAM id='press_amt' value='90'/></SNAP>"));
    CHECK(q.apvts.getRawParameterValue("air")->load() == 25 && q.order()[0] == snap::Module::shift);
    CHECK(q.apvts.getRawParameterValue("press_amt")->load() == 33 && q.apvts.getRawParameterValue("bypass")->load() > 0.5f); // lock + session toggle kept
    CHECK(!snap::loadPresetXml(q, "<NOPE/>"));
}

// Every module's visualizer shows every one of its controls (0.5.1, David's note 6): with no audio running, changing any
// control of the module (its amount dial and switch included) redraws the module's still picture differently.
static void testVisualizerControls()
{
    Proc p;
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    auto& ed = *e;
    tick(ed);
    const auto viz = [&](const char* column) -> juce::Component* {
        if (auto* c = find<juce::Component>(ed, column))
            for (auto* child : c->getChildren())
                if (dynamic_cast<snap::ui::Visualizer*>(child) != nullptr)
                    return child;
        return nullptr;
    };
    const auto picture = [&](juce::Component& v) {
        tick(ed), tick(ed);
        return v.createComponentSnapshot(v.getLocalBounds(), true, 1.0f);
    };
    const auto same = [](const juce::Image& a, const juce::Image& b) {
        for (int y = 0; y < a.getHeight(); ++y)
            for (int x = 0; x < a.getWidth(); ++x)
                if (a.getPixelAt(x, y) != b.getPixelAt(x, y))
                    return false;
        return true;
    };
    const std::pair<const char*, std::vector<const char*>> modules[] = {
        { "sub/hiss", { "subhiss_on", "sub_amt", "sub_sens", "sub_det_lo", "sub_det_hi", "sub_pitch", "sub_decay", "sub_duck", "sub_stereo", "sub_color", "sub_acid",
                        "sub_solo", "sub_post", "|hiss", "hiss_amt", "hiss_sens", "hiss_det_lo", "hiss_det_hi", "hiss_pitch", "hiss_decay", "hiss_duck", "hiss_type",
                        "hiss_tone", "hiss_env", "hiss_res", "hiss_solo", "hiss_post" } },
        { "shift", { "shift_on", "shift_amt", "shift_balance", "shift_invert", "shift_stereo", "shift_stretch", "shift_fmix", "shift_target", "shift_focus_lo",
                     "shift_focus_hi" } },
        { "space", { "space_on", "space_amt", "space_type", "space_decay", "space_stretch", "space_predelay", "space_duck", "space_pan", "space_width",
                     "space_focus_lo", "space_focus_hi" } },
        { "press", { "press_on", "press_amt", "press_mode", "press_downup", "press_mix", "press_bands", "press_attack" } },
        { "drive", { "drive_on", "drive_amt", "drive_mode", "drive_flavor", "drive_mix", "drive_target", "drive_focus_lo", "drive_focus_hi" } },
        { "push", { "push_on", "push_amt", "push_mult", "push_tone", "push_mix", "push_target", "push_focus_lo", "push_focus_hi" } } };
    int tested = 0, still = 0;
    for (auto& [column, ids] : modules)
    {
        auto* v = viz(column);
        CHECK(v != nullptr);
        if (v == nullptr)
            continue;
        for (const char* id : ids)
        {
            if (id[0] == '|') // the hiss tab: the column shows the hiss layer's controls
            {
                click(ed, "hiss tab");
                continue;
            }
            auto* q = p.apvts.getParameter(id);
            const float was = q->getValue();
            const auto before = picture(*v);
            const int steps = q->getNumSteps();
            const float to = steps > 1 && steps < 64 ? (float) ((juce::roundToInt(was * (float) (steps - 1)) + 1) % steps) / (float) (steps - 1) : was < 0.5f ? 0.85f : 0.15f;
            q->setValueNotifyingHost(to);
            const auto after = picture(*v);
            ++tested;
            if (same(before, after))
                ++still, std::printf("visualizer %s does not show %s\n", column, id);
            q->setValueNotifyingHost(was);
        }
    }
    std::printf("visualizers: %d controls changed, %d left a picture unchanged\n", tested, still);
    CHECK(tested == 70 && still == 0);
}

// The display feed carries the modules' own levels for their visualizers (space's wet, drive's and push's input and change).
static void testDisplayFeed()
{
    Proc p;
    for (auto [id, v] : { std::pair { "space_amt", 50.0f }, { "drive_amt", 70.0f }, { "push_amt", 70.0f } })
        set(p, id, v);
    prepare(p, 48000, 512, 2);
    run(p, 48000, 512, 2, 1.0);
    auto& d = p.display;
    const float wet = d.spaceWet.exchange(0), in = d.driveIn.exchange(0), added = d.driveAdded.exchange(0), pin = d.pushIn.exchange(0), padded = d.pushAdded.exchange(0);
    std::printf("display feed: space wet %.3f, drive in %.3f added %.3f, push in %.3f added %.3f\n", wet, in, added, pin, padded);
    CHECK(wet > 0.01f && in > 0.1f && added > 0.01f && pin > 0.1f && padded > 0.01f);
}

static int screenshot(const juce::File& file, bool browser)
{
    Proc p;
    for (auto [id, v] : std::initializer_list<std::pair<const char*, float>> {
             { "sub_amt", 70 }, { "hiss_amt", 40 }, { "sub_pitch", 26 }, { "sub_decay", 320 }, { "sub_duck", 25 }, { "sub_color", 40 },
             { "sub_acid", 1 }, { "shift_amt", 65 }, { "shift_balance", -0.16f }, { "shift_stretch", 30 }, { "space_amt", 35 },
             { "space_type", 2 }, { "space_decay", 40 }, { "space_predelay", 12 }, { "space_duck", 30 }, { "press_amt", 60 },
             { "press_mode", 1 }, { "press_mix", 80 }, { "press_downup", -0.3f }, { "press_bands", 0.6f }, { "drive_amt", 45 },
             { "drive_mode", 2 }, { "drive_flavor", -0.2f }, { "drive_target", 0.3f }, { "drive_focus_lo", 250 }, { "push_amt", 30 },
             { "push_on", 0 }, { "push_mult", 1 }, { "push_tone", 0.15f }, { "push_focus_lo", 2500 }, { "cut_lo", 80 },
             { "cut_hi", 12000 }, { "hc_shape", 1 }, { "tone", 24 }, { "tone_shape", 1 }, { "air", 18 }, { "attack", 30 },
             { "sustain", -16 }, { "clip_on", 1 }, { "out_gain", -1.5f }, { "shift_focus_lo", 40 }, { "shift_focus_hi", 16000 } })
        set(p, id, v);
    p.setOrder({ snap::Module::shift, snap::Module::space, snap::Module::press, snap::Module::drive });
    p.setLocked((int) snap::Module::press, true);
    snap::PresetStore store;
    if (const auto* preset = store.find(juce::String("factory/") + snap::defaultPresetName))
        p.apvts.state.setProperty("preset", preset->key(), nullptr);
    prepare(p, 48000, 512, 2);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    for (int n = 0; n < 60; ++n) // 2 s of drums with the 30 Hz feed running
        run(p, 48000, 512, 2, 1.0 / 30), tick(*e);
    if (browser)
        click(*e, "preset browser"), tick(*e);
    const auto image = e->createComponentSnapshot(e->getLocalBounds(), true, 1.0f);
    file.deleteFile();
    juce::FileOutputStream out(file);
    return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out) ? 0 : 1;
}

// CPU benchmark: real-time factor of the default preset (and every module pushed), 48 kHz stereo, 512-sample blocks,
// normal / oversampled / Low Latency, plus the editor's 30 Hz tick and a full repaint at 100 %.
static int benchmark()
{
    const double fs = 48000, seconds = 20;
    auto time = [](auto&& f) {
        const auto t0 = juce::Time::getHighResolutionTicks();
        f();
        return juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
    };
    for (auto [name, os, low, all] : { std::tuple { "hit hard", 0.0f, 0.0f, false }, std::tuple { "hit hard, oversampling", 1.0f, 0.0f, false },
                                       std::tuple { "hit hard, low latency", 0.0f, 1.0f, false }, std::tuple { "all six modules", 0.0f, 0.0f, true } })
    {
        std::unique_ptr<Proc> p(static_cast<Proc*>(createPluginFilter()));
        set(*p, "oversampling", os), set(*p, "low_latency", low);
        if (all)
            for (auto [id, v] : { std::pair { "sub_amt", 50.0f }, { "hiss_amt", 40.0f }, { "shift_amt", 30.0f }, { "space_amt", 40.0f },
                                  { "press_amt", 60.0f }, { "drive_amt", 50.0f }, { "push_amt", 50.0f }, { "push_on", 1.0f } })
                set(*p, id, v);
        prepare(*p, fs, 512, 2);
        const double t = time([&] { run(*p, fs, 512, 2, seconds); });
        std::printf("%-26s %6.2f %% of one core (%.0fx real time)\n", name, 100 * t / seconds, seconds / t);
    }
    Proc p;
    prepare(p, fs, 512, 2);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    run(p, fs, 512, 2, 1);
    const double ticks = time([&] { for (int i = 0; i < 300; ++i) run(p, fs, 512, 2, 1.0 / 30), tick(*e); }) - time([&] { for (int i = 0; i < 300; ++i) run(p, fs, 512, 2, 1.0 / 30); });
    const double paint = time([&] { for (int i = 0; i < 20; ++i) e->createComponentSnapshot(e->getLocalBounds(), true, 1.0f); }) / 20;
    std::printf("editor tick %.3f ms, full repaint %.1f ms (software renderer, 1440 x 840)\n", 1000 * ticks / 300, 1000 * paint);
    return 0;
}

static float loudness(const juce::AudioBuffer<float>& b) // K-weighted, over the last loudnessWindowSeconds
{
    snap::LoudnessMeter meter;
    meter.prepare(48000, 2);
    meter.process(b, b.getNumSamples());
    return meter.lufs();
}

// Offline renders for listening between milestones: <loop>-dry.wav plus one file per setting.
static int render(const juce::File& dir)
{
    const std::pair<const char*, std::vector<std::pair<const char*, float>>> settings[] = {
        { "default", {} },
        { "punch", { { "attack", 60 }, { "sustain", -40 } } },
        { "punch-magnitude50", { { "attack", 60 }, { "sustain", -40 }, { "magnitude", 50 } } },
        { "master-cut-steep", { { "cut_lo", 150 }, { "cut_hi", 7000 }, { "lc_shape", 1 }, { "hc_shape", 1 } } },
        { "master-tilt-bright", { { "tone", 70 } } },
        { "master-low-bump", { { "tone", 80 }, { "tone_shape", 1 } } },
        { "master-smile-scoop", { { "tone", -80 }, { "tone_shape", 2 } } },
        { "master-band-focus", { { "tone", 80 }, { "tone_shape", 3 } } },
        { "master-air", { { "air", 80 } } },
        { "master-sustain", { { "attack", -30 }, { "sustain", 70 } } },
        { "master-clip-hot", { { "in_gain", 12 }, { "clip_on", 1 } } },
        { "chain-sub-hiss-post", { { "sub_amt", 45 }, { "hiss_amt", 40 }, { "hiss_post", 1 }, { "push_amt", 50 } } },
        { "chain-space-press", { { "space_amt", 40 }, { "space_type", 6 }, { "press_amt", 60 } } },
        { "chain-all", { { "sub_amt", 40 }, { "hiss_amt", 30 }, { "shift_amt", -20 }, { "space_amt", 30 }, { "press_amt", 50 },
                         { "drive_amt", 40 }, { "push_amt", 40 }, { "attack", 30 }, { "clip_on", 1 } } },
        { "chain-all-magnitude50", { { "sub_amt", 40 }, { "hiss_amt", 30 }, { "shift_amt", -20 }, { "space_amt", 30 }, { "press_amt", 50 },
                                     { "drive_amt", 40 }, { "push_amt", 40 }, { "attack", 30 }, { "clip_on", 1 }, { "magnitude", 50 } } },
    };
    dir.createDirectory();
    auto write = [&](const juce::AudioBuffer<float>& b, const juce::String& name) {
        auto file = dir.getChildFile(name + ".wav");
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
        const auto w = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
        return w != nullptr && w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    };
    bool ok = true;
    const snap::PresetStore presets(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("snap-render-no-user-presets"));
    for (int which = 0; which < 4; ++which)
    {
        const auto dry = loops::make(which, 48000, 8);
        ok = ok && write(dry, juce::String(loops::names[which]) + "-dry");
        // A setting, optionally in another middle order from the start and/or reordered half way (the M7 crossfade).
        auto chain = [&](const juce::String& name, const std::vector<std::pair<const char*, float>>& values, std::optional<snap::ChainOrder> from = {},
                         std::optional<snap::ChainOrder> midway = {}, const snap::Preset* preset = nullptr) {
            Proc p;
            if (preset != nullptr)
                snap::loadPresetXml(p, preset->xml);
            for (auto& [id, v] : values)
                set(p, id, v);
            if (from)
                p.setOrder(*from);
            prepare(p, 48000, 512, 2);
            auto wet = dry;
            juce::MidiBuffer midi;
            for (int start = 0; start < wet.getNumSamples(); start += 512)
            {
                if (midway && start == 4 * 48000 / 512 * 512)
                    p.setOrder(*midway);
                juce::AudioBuffer<float> block(wet.getArrayOfWritePointers(), 2, start, std::min(512, wet.getNumSamples() - start));
                p.processBlock(block, midi);
            }
            ok = ok && write(wet, juce::String(loops::names[which]) + "-" + name);
            return wet;
        };
        for (auto& [name, values] : settings)
            chain(name, values);
        using enum snap::Module;
        const auto& all = settings[13].second; // chain-all
        chain("chain-all-reordered", all, snap::ChainOrder { drive, press, space, shift });
        chain("chain-all-reorder-midway", all, {}, snap::ChainOrder { drive, press, space, shift });
        auto oversampled = all;
        oversampled.emplace_back("oversampling", 1);
        chain("chain-all-oversampled", oversampled);
        // M8: the bypassed chain, and the factory presets: every one on the kit loop and on its own loop (with its peak and
        // loudness against dry there), the default on every loop.
        chain("bypass", { { "bypass", 1 }, { "sub_amt", 60 }, { "press_amt", 80 }, { "drive_amt", 80 } });
        for (auto& preset : presets.presets())
            if (const bool own = loops::forTags(preset.tags) == which; preset.factory && (which == 2 || own || preset.name == snap::defaultPresetName))
            {
                const auto wet = chain("preset-" + preset.name.replaceCharacter(' ', '-'), {}, {}, {}, &preset);
                if (own)
                    std::printf("preset %-24s %-5s peak %6.2f dBFS, loudness %+5.2f dB vs dry\n", preset.name.toRawUTF8(), loops::names[which],
                                juce::Decibels::gainToDecibels(wet.getMagnitude(0, wet.getNumSamples())), loudness(wet) - loudness(dry));
            }
        // The modules on their own, outside the chain: one file per setting.
        auto module = [&](auto m, typename decltype(m)::Params params, const char* name) {
            m.setParams(params);
            m.prepare(48000, 2, 512, false);
            auto wet = dry;
            for (int start = 0; start < wet.getNumSamples(); start += 512)
            {
                juce::AudioBuffer<float> block(wet.getArrayOfWritePointers(), 2, start, std::min(512, wet.getNumSamples() - start));
                m.process(block, block.getNumSamples());
            }
            ok = ok && write(wet, juce::String(loops::names[which]) + "-" + name);
        };
        module(snap::Press {}, { .amount = 0.7f, .mode = 0 }, "press-tight");
        module(snap::Press {}, { .amount = 1, .mode = 2, .mix = 0.4f }, "press-crush-parallel");
        module(snap::Press {}, { .amount = 0.6f, .mode = 5, .downUp = 0, .bands = 1 }, "press-stack-multiband");
        module(snap::Press {}, { .amount = 0.8f, .mode = 4, .downUp = 1 }, "press-lull-up");
        for (int mode = 0; mode < 6; ++mode)
            module(snap::Drive {}, { .amount = 0.6f, .mode = mode, .flavor = mode % 2 ? 0.5f : -0.5f },
                   (juce::String("drive-") + snap::spec("drive_mode").choices[(size_t) mode]).toRawUTF8());
        module(snap::Drive {}, { .amount = 0.8f, .focusLo = 2000, .mode = 0 }, "drive-crunch-focus-high");
        module(snap::Push {}, { .amount = 0.6f }, "push-x1");
        module(snap::Push {}, { .amount = 0.8f, .mult = 2, .tone = 0.5f }, "push-x4-bright");
        module(snap::Push {}, { .amount = 1, .target = -1 }, "push-transients");
        module(snap::Shift {}, { .amount = 0.7f }, "shift-up");
        module(snap::Shift {}, { .amount = -0.8f, .balance = -1 }, "shift-down-freq");
        module(snap::Shift {}, { .amount = 0.7f, .balance = 1 }, "shift-formant-up");
        module(snap::Shift {}, { .amount = -0.7f, .balance = 1, .stretch = 1 }, "shift-formant-down-stretch");
        module(snap::Shift {}, { .amount = 0.5f, .stereo = -1 }, "shift-wide");
        module(snap::Shift {}, { .amount = -1, .target = -1 }, "shift-drop-transients");
    }
    return ok ? 0 : 1;
}


// Offline render of a raw file for the site's fidelity and audit tools: interleaved stereo float32 in, the same out
// (latency not removed), 48 kHz unless --rate, 128-sample blocks unless --block. Options: --preset <name>,
// --order shift,space,press,drive, and id=value pairs in host units, applied in order after the preset.
static int renderFile(int argc, char** argv)
{
    juce::File in(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2])), out(juce::File::getCurrentWorkingDirectory().getChildFile(argv[3]));
    juce::MemoryBlock data;
    if (!in.loadFileAsData(data))
        return 2;
    double fs = 48000;
    int block = 128;
    Proc p;
    snap::PresetStore store(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("snap-render-no-user-presets"));
    for (auto& preset : store.presets()) // a fresh instance starts on the default preset; --preset or neutral replaces it
        if (preset.factory && preset.name == "init")
            snap::loadPresetXml(p, preset.xml);
    for (auto& s : snap::parameterTable())
        if (!snap::isSessionToggle(s.id))
            set(p, s.id, s.def);
    for (int i = 4; i < argc; ++i)
    {
        const juce::String a(argv[i]);
        if (a == "--rate" && i + 1 < argc)
            fs = juce::String(argv[++i]).getDoubleValue();
        else if (a == "--block" && i + 1 < argc)
            block = juce::String(argv[++i]).getIntValue();
        else if (a == "--preset" && i + 1 < argc)
        {
            const juce::String name(argv[++i]);
            bool found = false;
            for (auto& preset : store.presets())
                if (preset.factory && preset.name == name)
                    found = snap::loadPresetXml(p, preset.xml);
            if (!found)
                return std::fprintf(stderr, "no preset %s\n", name.toRawUTF8()), 3;
        }
        else if (a == "--order" && i + 1 < argc)
        {
            const auto o = snap::parseChainOrder(argv[++i]);
            if (!o)
                return 4;
            p.setOrder(*o);
        }
        else if (a.containsChar('='))
        {
            const auto id = a.upToFirstOccurrenceOf("=", false, false);
            if (p.apvts.getParameter(id) == nullptr)
                return std::fprintf(stderr, "no parameter %s\n", id.toRawUTF8()), 5;
            set(p, id.toStdString(), a.fromFirstOccurrenceOf("=", false, false).getFloatValue());
        }
    }
    prepare(p, fs, block, 2);
    const auto* x = static_cast<const float*>(data.getData());
    const int n = (int) (data.getSize() / (2 * sizeof(float)));
    std::vector<float> y((size_t) n * 2);
    juce::AudioBuffer<float> b(2, block);
    juce::MidiBuffer midi;
    for (int start = 0; start < n; start += block)
    {
        const int len = std::min(block, n - start);
        b.setSize(2, len, false, false, true);
        for (int i = 0; i < len; ++i)
            b.setSample(0, i, x[2 * (start + i)]), b.setSample(1, i, x[2 * (start + i) + 1]);
        p.processBlock(b, midi);
        for (int i = 0; i < len; ++i)
            y[2 * (size_t) (start + i)] = b.getSample(0, i), y[2 * (size_t) (start + i) + 1] = b.getSample(1, i);
    }
    out.deleteFile();
    return out.replaceWithData(y.data(), y.size() * sizeof(float)) ? 0 : 1;
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    if (argc >= 4 && juce::String(argv[1]) == "--render-file")
        return renderFile(argc, argv);
    if (argc == 3 && (juce::String(argv[1]) == "--screenshot" || juce::String(argv[1]) == "--screenshot-browser"))
        return screenshot(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]), juce::String(argv[1]) == "--screenshot-browser");
    if (argc == 2 && juce::String(argv[1]) == "--bench")
        return benchmark();
    if (argc == 2 && juce::String(argv[1]) == "--feed")
        return testDisplayFeed(), failures == 0 ? 0 : 1;
    if (argc == 2 && juce::String(argv[1]) == "--audibility") // the control audibility check on its own
        return testAudibility(), failures == 0 ? 0 : 1;
    if (argc == 2 && juce::String(argv[1]) == "--editor") // the editor tests on their own
        testBraille(), testRiso(), testEditor(), testVisualizerControls();
    else
    if (argc == 3 && juce::String(argv[1]) == "--render")
        return render(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
    if (argc == 3 && juce::String(argv[1]) == "--render-space")
        return renderSpace(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
    if (argc == 3 && juce::String(argv[1]) == "--render-subhiss")
        return renderSubHiss(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
    if (argc == 2 && juce::String(argv[1]) == "--chain") // the M7 chain tests on their own
        testChain();
    else if (argc == 2 && juce::String(argv[1]) == "--presets") // the M8 preset tests on their own
        testPresets();
    else
    {
    testIdentity();
    testChainOrder();
    testLatency();
    testState();
    testMigratedSound();
    testMasterInProcessor();
    testFullChain();
    testMagnitude();
    testBounded();
    testSound();
    testBraille();
    testRiso();
    testEditor();
    testVisualizerControls();
    testModuleSwitches();
    testDisplayFeed();
    testAudibility();
    testDsp();
    testPress();
    testDrive();
    testPush();
    testShift();
    testMaster();
    testSpace();
    testSubHiss();
    testChain();
    testPresets();
    }
    std::cout << (failures == 0 ? "All Snap tests passed\n" : "Snap tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
