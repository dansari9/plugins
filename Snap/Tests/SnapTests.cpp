// SnapTests: drives the real processor and editor without a host.
//   SnapTests                      run every test, exit code 1 on failure
//   SnapTests --screenshot a.png   render the editor after feeding it drums
//   SnapTests --render <dir>       render the synthetic loops through a few settings to WAV files
#include "PluginProcessor.h"
#include "presets/StateIO.h"
#include "loops/Loops.h"
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

static int failures = 0;
#define CHECK(c) ((c) ? (void) 0 : (void) (++failures, std::cerr << "FAIL " << __LINE__ << ": " #c "\n"))

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

// Processes `seconds` of drums scaled by `level`; returns channel 0 out and (via `in`) channel 0 in.
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
        std::swap(a.chainOrder[(size_t) r.nextInt(4)], a.chainOrder[(size_t) r.nextInt(4)]);
        CHECK(!defaults(a));
        juce::MemoryBlock m, m2, m3;
        a.getStateInformation(m);
        load(b, m);
        b.getStateInformation(m2);
        load(c, m2);
        c.getStateInformation(m3);
        CHECK(m2 == m3 && b.chainOrder == a.chainOrder); // log-mapped values may move by one ulp once, then stay put
        for (auto& s : snap::parameterTable())
            CHECK(near(get(b, s.id), get(a, s.id)));
        CHECK(juce::AudioProcessor::getXmlFromBinary(m.getData(), (int) m.getSize())->getIntAttribute("stateVersion") == 2);
    }

    // (2) Frozen v1 fixture, exactly as snap 0.1.x writes it. Must load in every future version: Attack and Sustain
    // scaled by the old Mix, Output onto the master Output, every module switched off, everything else at default.
    Proc v1;
    const auto fixture = blob(R"(<SNAP stateVersion="1"><PARAM id="attack" value="60.0"/><PARAM id="sustain" value="-40.0"/>)"
                              R"(<PARAM id="mix" value="75.0"/><PARAM id="output" value="-3.5"/></SNAP>)");
    set(v1, "drive_amt", 50), set(v1, "magnitude", 20), v1.chainOrder = { snap::Module::drive, snap::Module::press, snap::Module::space, snap::Module::shift };
    load(v1, fixture);
    CHECK(get(v1, "attack") == 45.0f && get(v1, "sustain") == -30.0f && get(v1, "out_gain") == -3.5f && v1.chainOrder == snap::defaultChainOrder);
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
    CHECK(get(w, "hiss_type") == 17.0f && get(w, "cut_lo") == 20.0f && w.chainOrder == snap::defaultChainOrder);

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

static void testMigratedSound() // a v1 session renders as 0.1.x did: neutral = pass-through, Mix 100 % = same shaping
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
    CHECK(juce::String(JucePlugin_VersionString) == "0.2.0");
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
        CHECK(withId != nullptr && withId->getVersionHint() == (withId->paramID == "attack" || withId->paramID == "sustain" ? 1 : 2));
        found.add(withId->paramID);
    }
    CHECK(found == expected && found.size() == 86 && !found.contains("mix") && !found.contains("output"));
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
    CHECK(p.getLatencySamples() == 0 && p.getTailLengthSeconds() == 0 && !p.acceptsMidi() && !p.producesMidi() && p.getNumPrograms() == 1);
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

    // The display's gain trace shows the shaping actually applied, so none at Magnitude 0.
    auto traced = [&](float magnitude) {
        Proc p;
        set(p, "attack", 100), set(p, "magnitude", magnitude);
        prepare(p, fs, 256, 2);
        run(p, fs, 256, 2, 1.0);
        float m = 0;
        for (auto& g : p.history.gainDb)
            m = std::max(m, std::abs(g.load()));
        return m;
    };
    CHECK(traced(100) > 3 && traced(0) == 0);

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

static void testEditor() // (9)
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
            p.processBlock(b, midi);
        }
    });
    for (int n = 0; n < 25; ++n)
    {
        std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
        CHECK(e != nullptr && e->getWidth() == 520);
        set(p, "attack", (float) (n * 8 - 100));
        CHECK(e->createComponentSnapshot(e->getLocalBounds()).isValid());
        if (auto* t = dynamic_cast<juce::Timer*>(e.get()))
            t->timerCallback();
    }
    stop = true;
    audio.join();
}

static int screenshot(const juce::File& file)
{
    Proc p;
    set(p, "attack", 60), set(p, "sustain", -40);
    prepare(p, 48000, 512, 2);
    run(p, 48000, 512, 2, 2.7);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    const auto image = e->createComponentSnapshot(e->getLocalBounds(), true, 2.0f);
    file.deleteFile();
    juce::FileOutputStream out(file);
    return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out) ? 0 : 1;
}

// Offline renders for listening between milestones: <loop>-dry.wav plus one file per setting.
static int render(const juce::File& dir)
{
    const std::pair<const char*, std::vector<std::pair<const char*, float>>> settings[] = {
        { "default", {} },
        { "punch", { { "attack", 60 }, { "sustain", -40 } } },
        { "punch-magnitude50", { { "attack", 60 }, { "sustain", -40 }, { "magnitude", 50 } } },
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
    for (int which = 0; which < 4; ++which)
    {
        const auto dry = loops::make(which, 48000, 8);
        ok = ok && write(dry, juce::String(loops::names[which]) + "-dry");
        for (auto& [name, values] : settings)
        {
            Proc p;
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
            ok = ok && write(wet, juce::String(loops::names[which]) + "-" + name);
        }
    }
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    if (argc == 3 && juce::String(argv[1]) == "--screenshot")
        return screenshot(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
    if (argc == 3 && juce::String(argv[1]) == "--render")
        return render(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
    testIdentity();
    testChainOrder();
    testState();
    testMigratedSound();
    testMagnitude();
    testBounded();
    testSound();
    testEditor();
    std::cout << (failures == 0 ? "All Snap tests passed\n" : "Snap tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
