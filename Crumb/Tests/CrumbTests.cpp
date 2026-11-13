// CrumbTests: drives the real processor and editor without a host. Exit code 1 on failure.
//   CrumbTests --screenshot a.png [pre]   render the editor (100 %, 2x pixels) after a drum + bass loop (pre: filter pre)
#include "Crumb.h"
#include "BinaryData.h"
#include <juce_cryptography/juce_cryptography.h>
#define SCREENSHOT_PRESET 14 // "drums · break dust": 12 bit, 26 kHz, images on, lp post
#include <cstdlib>
#include <cstring>
#include <future>
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
                                     "imagesShift", "fType", "fOrder", "cutoff", "reso", "mix",  "output", "bypass" };

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
        CHECK(withId != nullptr && withId->getVersionHint() == 1);
        found.add(withId->paramID);
    }
    CHECK(found == ids && found.size() == 17 && p.getName() == "crumb");
    CHECK(p.getBypassParameter() == p.apvts.getParameter("bypass"));
    // Plan v2 defaults.
    CHECK(get(p, "preamp") == 0 && get(p, "bits") == 12 && get(p, "dcShift") == 0 && get(p, "dither") == 0 && get(p, "rate") == 26040
          && get(p, "jitter") == 0 && get(p, "approxOn") == 1 && get(p, "approxDev") == 0 && get(p, "imagesOn") == 0
          && get(p, "imagesShift") == 0 && get(p, "fType") == 0 && get(p, "fOrder") == 1 && get(p, "cutoff") == 20000 && get(p, "reso") == 0
          && get(p, "mix") == 100 && get(p, "output") == 0 && get(p, "bypass") == 0);
    CHECK(p.apvts.getParameter("bits")->getText(0, 16) == "OFF");
    CHECK(p.getLatencySamples() == 0 && p.getTailLengthSeconds() == 0 && !p.acceptsMidi() && !p.producesMidi());
    prepare(p, 44100, 512, 2);
    CHECK(p.getLatencySamples() == 0);
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
        CHECK(juce::AudioProcessor::getXmlFromBinary(m.getData(), (int) m.getSize())->getIntAttribute("stateVersion") == 1);
        if (n < 6)
        {
            prepare(a, 48000, 256, 2), prepare(b, 48000, 256, 2);
            const auto ya = run(a, 256, 2, 24000, [](int i) { return signal(i, 48000); });
            const auto yb = run(b, 256, 2, 24000, [](int i) { return signal(i, 48000); });
            CHECK(ya == yb);
        }
    }
    // Frozen v1 fixture, exactly as Crumb 0.1.0 writes it. Must load in every future version.
    Proc v1;
    const auto fixture = blob(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                              "\n"
                              R"(<CRUMB stateVersion="1"><PARAM id="approxDev" value="-25.0"/><PARAM id="approxOn" value="0.0"/>)"
                              R"(<PARAM id="bits" value="8.0"/><PARAM id="bypass" value="0.0"/><PARAM id="cutoff" value="1234.5"/>)"
                              R"(<PARAM id="dcShift" value="1.0"/><PARAM id="dither" value="40.0"/><PARAM id="fOrder" value="0.0"/>)"
                              R"(<PARAM id="fType" value="2.0"/><PARAM id="imagesOn" value="1.0"/><PARAM id="imagesShift" value="60.0"/>)"
                              R"(<PARAM id="jitter" value="15.0"/><PARAM id="mix" value="75.0"/><PARAM id="output" value="-3.5"/>)"
                              R"(<PARAM id="preamp" value="12.5"/><PARAM id="rate" value="22050.0"/><PARAM id="reso" value="33.3"/></CRUMB>)");
    auto fixtureLoaded = [&](Proc& p) {
        return get(p, "approxDev") == -25.0f && get(p, "approxOn") == 0 && get(p, "bits") == 8 && get(p, "cutoff") == 1234.5f && get(p, "dcShift") == 1
               && get(p, "dither") == 40 && get(p, "fOrder") == 0 && get(p, "fType") == 2 && get(p, "imagesOn") == 1 && get(p, "imagesShift") == 60
               && get(p, "jitter") == 15 && get(p, "mix") == 75 && get(p, "output") == -3.5f && get(p, "preamp") == 12.5f && get(p, "rate") == 22050
               && get(p, "reso") == 33.3f && get(p, "bypass") == 0;
    };
    load(v1, fixture);
    CHECK(fixtureLoaded(v1) && v1.preset == -1 && v1.presetName() == "custom" && v1.abSlot() == 0 && v1.editorWidth == 0);
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
            y.push_back((float) (amp * std::sin(w * std::floor(i * s[0].second / fs) * fs / s[0].second)));
    else
    {
        Proc p;
        set(p, s);
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
        set(p, { { "preamp", 48 }, { "bits", 0 }, { "rate", 44100 }, { "approxOn", 0 } });
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
    set(p, { { "preamp", 48 }, { "bits", 0 }, { "rate", 44100 }, { "approxOn", 0 } });
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
}

static const std::vector<Settings> extremes {
    { { "preamp", 48 }, { "bits", 1 }, { "dither", 100 }, { "rate", 44 }, { "jitter", 100 }, { "approxDev", -100 }, { "imagesOn", 1 },
      { "imagesShift", 100 }, { "fType", 1 }, { "fOrder", 0 }, { "cutoff", 20 }, { "reso", 100 }, { "output", 12 } },
    { { "bits", 24 }, { "dcShift", 1 }, { "rate", 44100 }, { "jitter", 100 }, { "approxDev", 100 }, { "fType", 2 }, { "cutoff", 20000 }, { "reso", 100 } },
    { { "bits", 4 }, { "rate", 3000 }, { "approxOn", 0 }, { "imagesOn", 1 }, { "fType", 3 }, { "cutoff", 1000 }, { "reso", 100 }, { "output", 12 } },
    { { "preamp", 24 }, { "bits", 0 }, { "rate", 11025 }, { "fType", 4 }, { "reso", 100 }, { "mix", 50 }, { "output", -24 } },
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
static bool lit(juce::Component& e, const char* id) { auto* b = dynamic_cast<juce::Button*>(find(e, id)); return b != nullptr && b->getToggleState(); }
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
    CHECK(p.getNumPrograms() == 25 && p.getCurrentProgram() == 0 && p.presetName() == "init" && defaults(p)); // init = defaults
    juce::StringArray names;
    for (int i = 0; i < p.getNumPrograms(); ++i)
    {
        names.addIfNotAlreadyThere(p.getProgramName(i));
        p.setCurrentProgram(i);
        CHECK(p.getCurrentProgram() == i && p.presetName() == p.getProgramName(i));
        for (int k = 0; k < 16; ++k) // every value is on its parameter's grid, so it is stored exactly
            CHECK(std::abs(get(p, ids[k]) - crumb::presets[i].v[k]) < 1e-3f * std::max(1.0f, std::abs(crumb::presets[i].v[k])));
        prepare(p, 48000, 512, 2);
        const auto y = run(p, 512, 2, 48000, [](int n) { return 1.5f * signal(n, 48000); });
        float peak = 0;
        for (float v : y)
            CHECK(std::isfinite(v)), peak = std::max(peak, std::abs(v));
        CHECK(peak > 1e-3f && peak < 4);
    }
    CHECK(names.size() == 25);

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
    // Best-effort display snapshot: split atomic samples may tear during concurrent overwrites; no allocations.
    auto scope = std::make_unique<crumb::Scope>();
    std::atomic<bool> stop { false };
    std::thread writer([&] {
        onAudioThread = true;
        for (uint32_t w = UINT32_MAX - 127; !stop; w += 64)
        {
            for (int i = 0; i < 64; ++i)
                scope->push(w + (uint32_t) i, (float) ((w + (uint32_t) i) % 1000), -(float) ((w + (uint32_t) i) % 1000));
            scope->write.store(w + 64, std::memory_order_release);
        }
        onAudioThread = false;
    });
    std::vector<float> in(4096), out(4096);
    int torn = 0;
    for (int n = 0; n < 2000; ++n)
    {
        scope->read(in.data(), out.data(), 4096);
        for (int i = 0; i < 4096; ++i)
            torn += in[(size_t) i] != -out[(size_t) i] ? 1 : 0;
    }
    stop = true;
    writer.join();
    CHECK(audioAllocations.exchange(0) == 0);
    CHECK(torn < 2000 * 8); // only the few slots overwritten during one read may mix
    for (uint32_t start : { uint32_t(INT32_MAX - 127), UINT32_MAX - 127 })
    {
        Proc p;
        prepare(p, 48000, 512, 1);
        p.scope.write = start;
        const auto y = run(p, 512, 1, 512, [](int i) { return (float) i / 2048; });
        CHECK(p.scope.write.load() == start + 512u);
        p.scope.read(in.data(), out.data(), 512);
        for (int i = 0; i < 512; ++i)
            CHECK(in[(size_t) i] == (float) i / 2048 && out[(size_t) i] == y[(size_t) i]);
    }
}

static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(5); }
// Asynchronous updates (attachments, timers) can lag on loaded CI machines: dispatch until done() or a 2 s deadline.
template <typename Done> static void pumpUntil(Done done)
{
    for (auto end = juce::Time::getMillisecondCounter() + 2000; !done() && juce::Time::getMillisecondCounter() < end;)
        pump(), juce::Timer::callPendingTimersSynchronously();
}

// VU meter on a test clock: the reading of a constant -12 dBFS (0 VU) signal is independent of the host block size,
// follows elapsed time rather than timer calls, and is never torn by a concurrent block. Clips expire by age.
static double testMs = 0;
static double testClock() { return testMs; }
static float vu(juce::Component& e, int c) { return (float) e.getProperties()["vu" + juce::String(c)]; }
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

static void testScopeRepaints()
{
    for (int percent : { 50, 100, 200 })
        for (float held : { 0.0f, 0.08f }) // decay to the floor, or to a retained nonzero waveform peak
        {
            Proc p[2];
            std::unique_ptr<juce::AudioProcessorEditor> editors[2];
            RepaintCapture* captures[2] {};
            juce::Image images[2];
            const float scale = (float) percent / 50; // same 2x pixel density as the immutable goldens
            testMs = 0;
            for (int k = 0; k < 2; ++k)
            {
                p[k].scope.clock = testClock;
                prepare(p[k], 48000, 512, 2), set(p[k], "bypass", 1);
                run(p[k], 512, 2, 8192, [](int i) { return (float) (0.4 * std::sin(i * 0.1)); });
                editors[k].reset(p[k].createEditor());
                dynamic_cast<juce::Timer&>(*editors[k]).stopTimer();
                editors[k]->setSize(1024 * percent / 100, 576 * percent / 100);
                auto& plate = *editors[k]->getChildComponent(0);
                captures[k] = new RepaintCapture(plate);
                plate.setCachedComponentImage(captures[k]); // component takes ownership
                images[k] = juce::Image(juce::Image::ARGB, 1024 * percent / 50, 576 * percent / 50, true);
                captures[k]->consume(images[k], scale);
                run(p[k], 512, 2, 8192, [held](int i) { return held * (float) std::sin(i * 0.1); });
            }
            int paints = 0;
            for (int frame = 0; frame < 120; ++frame)
            {
                testMs += 1000.0 / 30;
                for (int k = 0; k < 2; ++k)
                    dynamic_cast<juce::Timer&>(*editors[k]).timerCallback();
                // Reference consumes the original editor's unconditional 30 Hz screen repaint.
                captures[1]->invalidate({ 24, 56, 976, 216 });
                const bool painted = captures[0]->consume(images[0], scale);
                paints += painted;
                if (frame == 119) CHECK(!painted); // settled idle still suppresses screen repaints
                captures[1]->consume(images[1], scale);
            }
            CHECK(paints > 1 && paints < 120);
            testMs += 1000.0 / 30;
            for (int k = 0; k < 2; ++k)
            {
                run(p[k], 512, 2, 8192, [](int i) { return (float) (0.01 * std::sin(i * 0.1)); });
                dynamic_cast<juce::Timer&>(*editors[k]).timerCallback();
                CHECK(captures[k]->consume(images[k], scale));
            }
            const auto scope = (juce::Rectangle<int> { 24, 56, 632, 212 } * scale).toNearestInt();
            CHECK(pixels(images[0].getClippedImage(scope)) == pixels(images[1].getClippedImage(scope)));
        }
}

static void testMeter()
{
    const auto zeroVu = [](int) { return 0.25118864f; };
    for (int block : { 256, 2048, 4096 }) // shorter and longer than the 33 ms editor interval
    {
        Proc p;
        p.scope.clock = testClock, testMs = 0;
        prepare(p, 48000, block, 2), set(p, "bypass", 1);
        run(p, block, 2, 48000, zeroVu);
        std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
        auto& timer = dynamic_cast<juce::Timer&>(*e);
        float lo = 2, hi = 0;
        for (int frame = 1, done = 0; frame <= 90; ++frame) // 3 s; the audio runs ahead of each editor frame
        {
            for (testMs = frame * 1000.0 / 30; done * 1000.0 / 48000 < testMs; done += block)
                run(p, block, 2, block, zeroVu);
            timer.timerCallback();
            lo = frame > 45 ? juce::jmin(lo, vu(*e, 0)) : lo, hi = frame > 45 ? juce::jmax(hi, vu(*e, 0)) : hi;
        }
        CHECK(lo > 0.99f && hi < 1.01f && std::abs(vu(*e, 1) - 0.7f) < 0.01f);
        testMs += 600, timer.timerCallback(), testMs += 1000, timer.timerCallback(); // audio stopped: the needles fall
        CHECK(vu(*e, 0) < 0.01f);
    }
    {
        // A step seen after a 300 ms timer stall lands where nine regular frames take it.
        float at[2] {};
        for (int stalled = 0; stalled < 2; ++stalled)
        {
            Proc p;
            p.scope.clock = testClock, testMs = 0;
            prepare(p, 48000, 512, 2), set(p, "bypass", 1);
            std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
            run(p, 512, 2, 48000, zeroVu);
            for (int frame = 1; frame <= 9; ++frame)
                if (testMs = frame * 100.0 / 3; !stalled || frame == 9)
                    dynamic_cast<juce::Timer&>(*e).timerCallback();
            at[stalled] = vu(*e, 0);
        }
        CHECK(at[0] > 0.9f && std::abs(at[0] - at[1]) < 0.01f);
    }
    {
        // Close -> clip -> wait past the hold -> silence -> open: the LED is off and no old audio reaches the needles.
        Proc p;
        p.scope.clock = testClock, testMs = 0;
        prepare(p, 48000, 512, 2), set(p, "bypass", 1);
        run(p, 512, 2, 512, [](int) { return 1.25f; });
        testMs = 1150;
        run(p, 512, 2, 512, [](int) { return 0.0f; });
        std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
        for (int frame = 0; frame < 10; ++frame)
            testMs += 100.0 / 3, dynamic_cast<juce::Timer&>(*e).timerCallback();
        CHECK(!(bool) e->getProperties()["clip0"] && !(bool) e->getProperties()["clip1"] && vu(*e, 0) == 0);
        run(p, 512, 2, 512, [](int) { return 1.25f; }), dynamic_cast<juce::Timer&>(*e).timerCallback(); // a new clip lights
        CHECK((bool) e->getProperties()["clip0"]);
    }
    {
        // Mid-write the record is not read; under a concurrent writer every read delta is exactly one constant level.
        Proc p;
        prepare(p, 48000, 512, 2), set(p, "bypass", 1);
        run(p, 512, 2, 48000, zeroVu);
        crumb::Scope::Meter m, last;
        p.scope.seq += 1;
        CHECK(!p.scope.meter(m));
        p.scope.seq += 1;
        CHECK(p.scope.meter(last));
        std::atomic<bool> stop { false };
        std::thread audio([&] { while (!stop) run(p, 0, 2, 8192, zeroVu); }); // random block sizes 1..4096
        int reads = 0, torn = 0;
        for (auto end = juce::Time::getMillisecondCounter() + 300; juce::Time::getMillisecondCounter() < end;)
            if (p.scope.meter(m) && m.frames != last.frames)
            {
                const double l = std::sqrt((m.energy[0] - last.energy[0]) / (double) (m.frames - last.frames)), r = std::sqrt((m.energy[1] - last.energy[1]) / (double) (m.frames - last.frames));
                torn += std::abs(l / 0.25118864 - 1) > 1e-4 || std::abs(r / 0.25118864 / 0.7 - 1) > 1e-4, ++reads, last = m;
            }
        stop = true;
        audio.join();
        CHECK(reads > 10 && torn == 0);
    }
}

static void testEditorState()
{
    Proc p;
    prepare(p, 48000, 128, 2);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    CHECK(e->getWidth() == 1024 && e->getHeight() == 576 && e->isResizable());
    for (int pct : { 50, 200, 100 }) // scaled 50-200 %, aspect kept; the size is stored for the next editor
    {
        e->setSize(1024 * pct / 100, 576 * pct / 100);
        CHECK(e->getWidth() == 1024 * pct / 100 && e->getHeight() == 576 * pct / 100 && p.editorWidth == e->getWidth());
        CHECK(e->createComponentSnapshot(e->getLocalBounds()).isValid());
    }
    e->setSize(1536, 864);
    e.reset(p.createEditor());
    CHECK(e->getWidth() == 1536 && e->getHeight() == 864);

    // Keys follow the parameters both ways: host changes (message thread and other threads), presets, A/B, state loads.
    auto& ed = *e;
    auto keysMatch = [&] {
        bool ok = lit(ed, "dcShift") == (get(p, "dcShift") > 0.5f) && lit(ed, "approxOn") == (get(p, "approxOn") > 0.5f)
                  && lit(ed, "imagesOn") == (get(p, "imagesOn") > 0.5f) && lit(ed, "pre") == (get(p, "fOrder") < 0.5f) && lit(ed, "post") == (get(p, "fOrder") > 0.5f);
        int n = 0;
        for (auto* t : { "off", "lp", "bp", "hp", "br" })
            ok = ok && lit(ed, t) == (n++ == (int) get(p, "fType"));
        return ok;
    };
    CHECK(keysMatch());
    juce::Random r(5);
    for (int n = 0; n < 40; ++n)
    {
        for (auto* id : { "dcShift", "approxOn", "imagesOn", "fType", "fOrder" })
        {
            const float v = (float) r.nextInt(std::strcmp(id, "fType") == 0 ? 5 : 2);
            if (n % 2 == 0)
                set(p, id, v);
            else // an automation thread: the key updates asynchronously on the message thread
                std::thread([&] { set(p, id, v); }).join();
        }
        pumpUntil(keysMatch);
        CHECK(keysMatch());
    }
    for (int i = 0; i < 25; ++i)
        p.setCurrentProgram(i), CHECK(keysMatch());
    p.setCurrentProgram(20), p.selectAb(1), p.setCurrentProgram(18), CHECK(keysMatch());
    p.selectAb(0), CHECK(keysMatch() && get(p, "fType") == 3);
    // Clicking a lit radio key keeps it lit; clicks reach the parameter.
    auto click = [&](const char* id) { dynamic_cast<juce::Button*>(find(ed, id))->onClick(); };
    click("bp"), click("bp"), click("pre");
    CHECK(get(p, "fType") == 2 && get(p, "fOrder") == 0 && keysMatch());
    for (int k = 0; k < 30; ++k)
        click("plus");
    CHECK(get(p, "bits") == 24);
    click("minus");
    CHECK(get(p, "bits") == 23);
    auto* knob = dynamic_cast<juce::Slider*>(find(ed, "rate"));
    CHECK(knob != nullptr && knob->getTextFromValue(knob->getValue()) == p.apvts.getParameter("rate")->getCurrentValueAsText());
    knob->setValue(knob->getValueFromText("9.2k"), juce::sendNotificationSync);
    CHECK(get(p, "rate") == 9200);
    knob->setValue(knob->getDoubleClickReturnValue(), juce::sendNotificationSync);
    CHECK(get(p, "rate") == 26040);
    // Exercise JUCE's actual mouse shortcuts, including its implicit Alt/Option single-click default reset.
    const auto mouseClick = [&](int modifiers, int clicks) {
        knob->setValue(9200, juce::sendNotificationSync);
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
    // Restore on a host thread while dispatching real editor timers and asynchronous attachments.
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
    pumpUntil(keysMatch);
    CHECK(keysMatch() && p.abSlot() == 1);
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

// The screenshot fixture: preset 14 after a drum + bass loop, then one second of 30 fps editor frames (the VU settles).
static std::unique_ptr<juce::AudioProcessorEditor> fixture(Proc& p, bool preFilter)
{
    p.setCurrentProgram(SCREENSHOT_PRESET);
    if (preFilter)
        set(p, "fOrder", 0);
    prepare(p, 48000, 512, 2);
    p.scope.clock = testClock, testMs = 0;
    run(p, 512, 2, 8400, groove);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    for (int frame = 0; frame < 30; ++frame)
    {
        testMs += 1000.0 / 30;
        run(p, 512, 2, 1600, [frame](int i) { return groove(8400 + frame * 1600 + i); }); // ends 50 ms after a snare hit
        dynamic_cast<juce::Timer&>(*e).timerCallback();
    }
    return e;
}

static int screenshot(const juce::File& file, bool preFilter)
{
    Proc p;
    const auto e = fixture(p, preFilter);
    const auto image = e->createComponentSnapshot(e->getLocalBounds(), true, 2.0f);
    file.deleteFile();
    juce::FileOutputStream out(file);
    return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out) ? 0 : 1;
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
// factory preset, 2 s from a settled start. -1: the sweep over 4 s from init with preamp 6 dB, dither 20 %, images on,
// lowpass post. -2: the same, re-prepared at 96 kHz at block 200.
static juce::String render(int program, int channels)
{
    Proc p;
    if (program >= 0)
        p.setCurrentProgram(program);
    else
        set(p, { { "preamp", 6.0f }, { "dither", 20.0f }, { "imagesOn", 1.0f }, { "fType", 1.0f } });
    prepare(p, 48000, 512, channels);
    juce::AudioBuffer<float> buffer(channels, 512);
    juce::MidiBuffer midi;
    juce::MemoryOutputStream bytes;
    for (int b = 0; b < (program >= 0 ? 188 : 375); ++b)
    {
        if (program < 0)
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
static juce::Image snapshot(bool preFilter, int percent) // the fixture at an editor size, 2x pixels
{
    Proc p;
    auto e = fixture(p, preFilter);
    e->setSize(1024 * percent / 100, 576 * percent / 100);
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
static const Golden sounds[] = {
    { -2, 2, "6b4a7e3db119bd7953b329a5a71f07bf5c5e02f5050930ea45c399f2cc6a9e7f" },
    { -1, 2, "445d8085bf79a75d05fe72045b84b65e4c8aa564bd33a06d2f08e4b152a95311" },
    { -1, 1, "4119ae083e0464e315bb483ba720446203b872edbd4426f44fbb5bc78abb0597" },
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
struct Shot { bool preFilter; int percent; const char* hash; };
static const Shot shots[] = {
    { false, 100, "9e6312a8b8f03752026f0a487e7ac545ffe3a303c5cb7e938d6bdde3f6e1b0cf" },
    { false, 50, "bfaba0c0d89ba92b722d2a3d060a328450335f9da4425a4c1efaf9e58532806c" },
    { false, 200, "53f8e9e85d762fc2df2a31505e5185febaf843f968960837987600256b307e5f" },
    { true, 100, "9acc73af7f75e832461afbb0a2113ec4ac944b8cb6c13a7a5174b9a9b84b74ea" },
};
static void testGoldens()
{
    const auto audio = render(-1, 2), image = pixels(snapshot(false, 100));
    CHECK(audio == render(-1, 2));
    CHECK(image == pixels(snapshot(false, 100)));
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
    if (matchingMath)
        for (const auto& s : sounds)
            if (render(s.program, s.channels) != s.hash)
                CHECK(!"bit-exact render"), std::cerr << "  program " << s.program << ", " << s.channels << " ch\n";
    if (matchingMath && matchingRenderer) // snapshots also contain the audio-driven scopes and meters
        for (const auto& s : shots)
            if (pixels(snapshot(s.preFilter, s.percent)) != s.hash)
                CHECK(!"pixel-exact snapshot"), std::cerr << "  " << s.percent << " %" << (s.preFilter ? " pre" : "") << "\n";
    std::cout << "Environment fingerprints: " << sha(math) << " / " << pixels(probe) << "\nChecked "
              << (matchingMath ? std::size(sounds) : 0) << " audio hashes and "
              << (matchingMath && matchingRenderer ? std::size(shots) : 0) << " pixel hashes from 81593d3 (matching environments only)\n";
#endif
}
// CrumbTests --bench: processBlock ns per stereo sample frame (48 kHz, block 512, warmed; init = the defaults, the
// settled 26.04 kHz 64-tap case; the sweep) and the editor's 30 Hz work: timerCallback, then painting the two screens'
// region (24, 56, 976, 216) at 100 %; a full plate paint for comparison. Means over repeated runs.
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
    Proc p;
    auto e = fixture(p, false);
    auto& plate = *e->getChildComponent(0);
    juce::Image image(juce::Image::ARGB, 1024, 576, true);
    double timer = 0, screens = 0, full = 0;
    const int frames = 300;
    for (int frame = -30; frame < frames; ++frame) // 30 warm-up frames
    {
        testMs += 1000.0 / 30;
        run(p, 512, 2, 1600, [frame](int i) { return groove(60000 + frame * 1600 + i); });
        const auto t0 = Clock::now();
        dynamic_cast<juce::Timer&>(*e).timerCallback();
        const auto t1 = Clock::now();
        {
            juce::Graphics g(image);
            g.reduceClipRegion(24, 56, 976, 216);
            plate.paintEntireComponent(g, false);
        }
        const auto t2 = Clock::now();
        if (frame % 10 == 0)
        {
            juce::Graphics g(image);
            plate.paintEntireComponent(g, false);
        }
        const auto t3 = Clock::now();
        if (frame >= 0)
            timer += ms(t0, t1), screens += ms(t1, t2), full += frame % 10 == 0 ? ms(t2, t3) : 0;
    }
    std::cout << "editor timerCallback: " << timer / frames << " ms\n"
              << "editor 30 Hz screens paint (24, 56, 976, 216): " << screens / frames << " ms\n"
              << "editor timer + screens paint: " << (timer + screens) / frames << " ms\n"
              << "editor full plate paint: " << full / (frames / 10) << " ms\n";
    return 0;
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    if ((argc == 3 || argc == 4) && juce::String(argv[1]) == "--screenshot") // optional 3rd argument "pre": filter order pre
        return screenshot(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]), argc == 4 && juce::String(argv[3]) == "pre");
    if (argc == 2 && juce::String(argv[1]) == "--bench") return bench();
    if (argc == 2 && juce::String(argv[1]) == "--golden-tests") { testGoldens(); return failures == 0 ? 0 : 1; }
    testGoldens();
    testIdentity();
    testState();
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
    testScopeRepaints();
    testMeter();
    testEditorState();
    std::cout << (failures == 0 ? "All Crumb tests passed\n" : "Crumb tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
