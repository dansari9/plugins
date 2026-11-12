// CrumbTests: drives the real processor and editor without a host. Exit code 1 on failure.
//   CrumbTests --screenshot a.png   render the editor (100 %, 2x pixels) after feeding it a drum + bass loop
#include "Crumb.h"
#define SCREENSHOT_PRESET 14 // "drums · break dust": 12 bit, 26 kHz, images on, lp post
#include <cstdlib>
#include <cstring>
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
    CHECK(juce::String(JucePlugin_Name) == "Crumb" && juce::String(JucePlugin_Manufacturer) == "daveydidit");
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
    CHECK(found == ids && found.size() == 17);
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
    for (auto* bad : { "12abc", "1-2", "5.5.5", "--3", "1e", ".", "&#x", "1e2147483648", "-1e4294967296" }) // not wholly a (finite) number
    {
        load(v1, fixture);
        juce::String xml("<CRUMB>");
        for (auto& id : ids)
            xml << "<PARAM id=\"" << id << "\" value=\"" << bad << "\"/>";
        load(v1, blob(xml + "</CRUMB>"));
        CHECK(defaults(v1));
    }
    // Hostile or incomplete blobs (deep nesting, DTD entities, truncated references, foreign roots) leave the state alone.
    load(v1, fixture);
    load(v1, blob("<CRUMB>" + juce::String::repeatedString("<a>", 1000000)));
    load(v1, blob(R"(<!DOCTYPE CRUMB [<!ENTITY a "&a;&a;">]><CRUMB><PARAM id="preamp" value="&a;"/></CRUMB>)"));
    for (auto* bad : { "<CRUMB>&#x", "<CRUMB>&#", "<CRUMB><PARAM id=\"bits\" value=\"&#x", "</CRUMB>", "<CRUMB><a></CRUMB>",
                       R"(<SNAP><PARAM id="bits" value="3"/></SNAP>)" })
        load(v1, blob(bad));
    CHECK(fixtureLoaded(v1));

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
static std::vector<double> spectrum(const Settings& s, double freq, double fs = 44100, bool naive = false)
{
    constexpr int size = 1 << 15, skip = 8192;
    const double f0 = std::round(freq * size / fs) * fs / size, w = 2 * juce::MathConstants<double>::pi * f0 / fs;
    std::vector<float> y;
    if (naive) // reference: a plain sample & hold at the same rate, no bandlimited edges
        for (int i = 0; i < size + skip; ++i)
            y.push_back((float) (0.4 * std::sin(w * std::floor(i * s[0].second / fs) * fs / s[0].second)));
    else
    {
        Proc p;
        set(p, s);
        prepare(p, fs, 512, 2);
        y = run(p, 512, 2, size + skip, [w](int i) { return (float) (0.4 * std::sin(w * i)); });
    }
    std::vector<std::complex<double>> x(size);
    for (int i = 0; i < size; ++i)
    {
        const double a = 2 * juce::MathConstants<double>::pi * i / size;
        x[(size_t) i] = y[(size_t) (i + skip)] * (0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a));
    }
    crumb::fft(x, false);
    std::vector<double> db(size / 2);
    const double top = std::pow(0.4 * 0.35875 * size / 2, 2);
    for (int k = 0; k < size / 2; ++k)
        db[(size_t) k] = 10 * std::log10(std::norm(x[(size_t) k]) / top + 1e-30);
    return db;
}
// Strongest bin outside the listed tones (+-8 bins), and the level at one tone.
static double spur(const std::vector<double>& db, std::initializer_list<double> tones, double fs = 44100)
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
    const double naiveDb = spur(spectrum({ { "rate", fr } }, 1000, 44100, true), { 1000 });
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
    const auto home = juce::File::createTempFile("crumbhome");
    const juce::String realHome = std::getenv("HOME");
    setenv("HOME", home.getFullPathName().toRawUTF8(), 1);
    CHECK(Proc::presetFolder().isAChildOf(home));
    Proc u;
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
    home.deleteRecursively();
    setenv("HOME", realHome.toRawUTF8(), 1);
}

static void testScopeFifo()
{
    // Concurrent writer (flagged as audio) and reader: no allocation, and every pair the reader sees belongs together.
    auto scope = std::make_unique<crumb::Scope>();
    std::atomic<bool> stop { false };
    std::thread writer([&] {
        onAudioThread = true;
        for (int w = 0; !stop; w += 64)
        {
            for (int i = 0; i < 64; ++i)
                scope->push(w + i, (float) ((w + i) % 1000), (float) -((w + i) % 1000));
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
}

#if JUCE_LINUX || JUCE_WINDOWS
namespace juce::detail { bool dispatchNextMessageOnSystemQueue(bool returnIfNoPendingMessages); }
static bool pumpMessages() { while (juce::detail::dispatchNextMessageOnSystemQueue(true)) {} return true; }
#else
static bool pumpMessages() { return false; } // macOS: no public single-step dispatch without modal loops
#endif

static void testEditorState()
{
    Proc p;
    prepare(p, 48000, 128, 2);
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    CHECK(e->getWidth() == 960 && e->getHeight() == 540 && e->isResizable());
    for (int pct : { 50, 200, 100 }) // scaled 50-200 %, aspect kept; the size is stored for the next editor
    {
        e->setSize(960 * pct / 100, 540 * pct / 100);
        CHECK(e->getWidth() == 960 * pct / 100 && e->getHeight() == 540 * pct / 100 && p.editorWidth == e->getWidth());
        CHECK(e->createComponentSnapshot(e->getLocalBounds()).isValid());
    }
    e->setSize(1440, 810);
    e.reset(p.createEditor());
    CHECK(e->getWidth() == 1440 && e->getHeight() == 810);

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
            if (n % 2 == 0 || !pumpMessages())
                set(p, id, v);
            else // an automation thread: the key updates asynchronously on the message thread
                std::thread([&] { set(p, id, v); }).join();
        }
        pumpMessages();
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

static int screenshot(const juce::File& file)
{
    Proc p;
    p.setCurrentProgram(SCREENSHOT_PRESET);
    prepare(p, 48000, 512, 2);
    run(p, 512, 2, 48000 + 6000 + 2400, groove); // ends 50 ms after a snare hit
    std::unique_ptr<juce::AudioProcessorEditor> e(p.createEditor());
    const auto image = e->createComponentSnapshot(e->getLocalBounds(), true, 2.0f);
    file.deleteFile();
    juce::FileOutputStream out(file);
    return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out) ? 0 : 1;
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    if (argc == 3 && juce::String(argv[1]) == "--screenshot")
        return screenshot(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
    testIdentity();
    testState();
    testNull();
    testAlias();
    testSound();
    testBounded();
    testAllocations();
    testEditor();
    testPresets();
    testScopeFifo();
    testEditorState();
    std::cout << (failures == 0 ? "All Crumb tests passed\n" : "Crumb tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
