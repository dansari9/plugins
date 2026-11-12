// SnapTests: drives the real processor and editor without a host.
//   SnapTests                      run every test, exit code 1 on failure
//   SnapTests --screenshot a.png   render the editor after feeding it drums
#include "Snap.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

// Real-time check: count heap allocations made on a thread flagged as "audio".
static thread_local bool onAudioThread = false;
static std::atomic<int> audioAllocations { 0 };
void* operator new(std::size_t n)
{
    if (onAudioThread)
        ++audioAllocations;
    if (void* p = std::malloc(n == 0 ? 1 : n))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static int failures = 0;
#define CHECK(c) ((c) ? (void) 0 : (void) (++failures, std::cerr << "FAIL " << __LINE__ << ": " #c "\n"))

using Proc = SnapProcessor;
static const juce::StringArray ids { "attack", "sustain", "mix", "output" };

static void set(Proc& p, const char* id, float v)
{
    auto* param = p.apvts.getParameter(id);
    param->setValueNotifyingHost(param->convertTo0to1(v));
}
static float get(Proc& p, const juce::String& id) { return p.apvts.getRawParameterValue(id)->load(); }
static bool defaults(Proc& p) { return get(p, "attack") == 0 && get(p, "sustain") == 0 && get(p, "mix") == 100 && get(p, "output") == 0; }

static void prepare(Proc& p, double fs, int block, int ch)
{
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(ch));
    l.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(ch));
    CHECK(p.setBusesLayout(l));
    p.setPlayConfigDetails(ch, ch, fs, block);
    p.prepareToPlay(fs, block);
}

// Kick (decaying 60 Hz sine) and snare (decaying noise) alternating every 250 ms.
static float drum(int i, double fs)
{
    static juce::Random noise(7);
    const double t = std::fmod(i / fs, 0.5), u = t < 0.25 ? t : t - 0.25;
    return t < 0.25 ? (float) (0.9 * std::sin(2 * juce::MathConstants<double>::pi * 60 * u) * std::exp(-u / 0.08))
                    : (float) (0.6 * (noise.nextFloat() * 2 - 1) * std::exp(-u / 0.05));
}

// Processes `seconds` of drums; returns channel 0 out and (via `in`) channel 0 in.
static std::vector<float> run(Proc& p, double fs, int block, int ch, double seconds, std::vector<float>* in = nullptr, bool poison = false)
{
    juce::AudioBuffer<float> buffer(ch, block);
    juce::MidiBuffer midi;
    std::vector<float> out, input;
    for (int start = 0; start < (int) (seconds * fs); start += block)
    {
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i)
                buffer.setSample(c, i, drum(start + i, fs) * (c == 0 ? 1.0f : -0.7f));
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
static juce::MemoryBlock blob(const char* xml)
{
    juce::MemoryBlock m;
    juce::AudioProcessor::copyXmlToBinary(*juce::parseXML(xml), m);
    return m;
}

static void testState()
{
    juce::Random r(42);
    for (int n = 0; n < 50; ++n) // (1) random round trip, (2) into a brand-new instance
    {
        Proc a, b;
        for (auto* param : a.getParameters())
            param->setValueNotifyingHost(r.nextFloat());
        juce::MemoryBlock m;
        a.getStateInformation(m);
        b.setStateInformation(m.getData(), (int) m.getSize());
        a.setStateInformation(m.getData(), (int) m.getSize());
        juce::MemoryBlock m2;
        b.getStateInformation(m2);
        CHECK(m == m2);
        for (auto& id : ids)
            CHECK(get(a, id) == get(b, id));
        CHECK(juce::AudioProcessor::getXmlFromBinary(m.getData(), (int) m.getSize())->getIntAttribute("stateVersion") == 1);
    }
    // (2) Frozen v1 fixture, exactly as Snap 0.1.0 writes it. Must load in every future version.
    Proc v1;
    const auto fixture = blob(R"(<SNAP stateVersion="1"><PARAM id="attack" value="60.0"/><PARAM id="sustain" value="-40.0"/>)"
                              R"(<PARAM id="mix" value="75.0"/><PARAM id="output" value="-3.5"/></SNAP>)");
    v1.setStateInformation(fixture.getData(), (int) fixture.getSize());
    CHECK(get(v1, "attack") == 60.0f && get(v1, "sustain") == -40.0f && get(v1, "mix") == 75.0f && get(v1, "output") == -3.5f);
    // Unknown entries ignored, out-of-range clamped, missing and unparsable back to default.
    const auto odd = blob(R"(<SNAP stateVersion="7" future="x"><PARAM id="attack" value="1e9"/><PARAM id="mix" value="abc"/>)"
                          R"(<PARAM id="output" value="nan"/><PARAM id="wobble" value="3"/><EXTRA/></SNAP>)");
    v1.setStateInformation(odd.getData(), (int) odd.getSize());
    CHECK(get(v1, "attack") == 100.0f && get(v1, "sustain") == 0.0f && get(v1, "mix") == 100.0f && get(v1, "output") == 0.0f);

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
    const auto foreign = blob(R"(<ROOTNOTE><PARAM id="attack" value="50"/></ROOTNOTE>)");
    g.setStateInformation(foreign.getData(), (int) foreign.getSize());
    CHECK(defaults(g));
}

static void testIdentity() // (4)
{
    CHECK(JucePlugin_PluginCode == 0x536e6170);       // 'Snap'
    CHECK(JucePlugin_ManufacturerCode == 0x44776b73); // 'Dwks'
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_AUMainType)) == "'aufx'");
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_CFBundleIdentifier)) == "com.dripworks.snap");
    CHECK(juce::String(JucePlugin_Name) == "Snap" && juce::String(JucePlugin_Manufacturer) == "Dripworks");
    CHECK(!JucePlugin_IsSynth && !JucePlugin_WantsMidiInput);
    Proc p;
    juce::StringArray found;
    for (auto* param : p.getParameters())
    {
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(param);
        CHECK(withId != nullptr && withId->getVersionHint() == 1);
        found.add(withId->paramID);
    }
    CHECK(found == ids);
    CHECK(defaults(p) && p.getLatencySamples() == 0 && p.getTailLengthSeconds() == 0 && !p.acceptsMidi() && !p.producesMidi());
    CHECK(p.getNumPrograms() == 1);
}

static void testBounded() // (5) and (10)
{
    const float settings[][4] = { { 100, 100, 100, 12 }, { -100, -100, 100, 12 }, { 100, -100, 50, -24 }, { -100, 100, 0, 12 } };
    for (double fs : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int ch : { 1, 2 })
            for (int block : { 1, 7, 64, 511, 1023, 4096 })
                for (auto& s : settings)
                {
                    Proc p;
                    for (int k = 0; k < 4; ++k)
                        set(p, ids[k].toRawUTF8(), s[k]);
                    prepare(p, fs, block, ch);
                    bool ok = true;
                    for (float y : run(p, fs, block, ch, block == 1 ? 0.1 : 0.4, nullptr, true))
                        ok = ok && std::isfinite(y) && std::abs(y) <= 64.0f; // input peak <= 1, max gain 63x
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
    auto render = [&](float attack, float sustain, std::vector<float>* in = nullptr) {
        Proc p;
        set(p, "attack", attack), set(p, "sustain", sustain);
        prepare(p, fs, 256, 2);
        return run(p, fs, 256, 2, 2.0, in);
    };
    std::vector<float> in;
    const auto unity = render(0, 0, &in);
    float diff = 0;
    for (size_t i = 0; i < in.size(); ++i)
        diff = std::max(diff, std::abs(unity[i] - in[i]));
    CHECK(diff < 1.0e-6f);

    // Kick at 1 s: transient = first 10 ms, tail = 60..200 ms after the hit.
    const size_t hit = (size_t) fs, head = hit + (size_t) (0.01 * fs), t0 = hit + (size_t) (0.06 * fs), t1 = hit + (size_t) (0.2 * fs);
    CHECK(peak(render(100, 0), hit, head) > 1.5f * peak(unity, hit, head));
    CHECK(peak(render(-100, 0), hit, head) < 0.7f * peak(unity, hit, head));
    CHECK(energy(render(0, 100), t0, t1) > 2.0 * energy(unity, t0, t1));
    CHECK(energy(render(0, -100), t0, t1) < 0.5 * energy(unity, t0, t1));

    // (8) No allocation inside processBlock, including while parameters move.
    onAudioThread = true;
    delete new int(1);
    onAudioThread = false;
    CHECK(audioAllocations.exchange(0) == 1); // the counter itself works
    Proc p;
    prepare(p, fs, 512, 2);
    for (int n = 0; n < 20; ++n)
    {
        set(p, "attack", (float) (n * 10 - 100)), set(p, "sustain", (float) (100 - n * 10)), set(p, "output", (float) n - 12);
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

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    if (argc == 3 && juce::String(argv[1]) == "--screenshot")
        return screenshot(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
    testIdentity();
    testState();
    testBounded();
    testSound();
    testEditor();
    std::cout << (failures == 0 ? "All Snap tests passed\n" : "Snap tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
