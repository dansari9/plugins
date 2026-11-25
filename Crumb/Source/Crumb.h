#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <complex>
#include <string_view>
#include <vector>
#include "Band.h"
#include "Codec.h"

// crumb: vintage-sampler bit crusher by daveydidit (clean-room: textbook DSP only).
// FROZEN: the parameter IDs listed in Crumb.cpp (ParameterID version 1, appended autoGain 2, 0.3.0's 3) and the state
// format (APVTS XML <CRUMB stateVersion="4">, plus additive root attributes). Never rename, remove or reorder; only append.
namespace crumb
{
void fft(std::vector<std::complex<double>>& x, bool inverse); // radix-2 in place, size a power of two
// The whole document is the flat state grammar: <CRUMB> with attributes, children at most one level deep, matching
// closing tags, complete character references, no DTD. JUCE's XmlDocument checks none of that, so it is run first.
bool isFlatCrumbXml(std::string_view xml);

// Butterworth damping 2cos((2i+1)pi/2N) of the cascaded sections: 8th-order approx filter, 4th-order images filter.
inline constexpr double approxK[] = { 1.9615705608, 1.6629392246, 1.1111404660, 0.3901806440 }, imagesK[] = { 1.8477590650, 0.7653668647 };

// ADC soft clip: linear up to the knee, then a tanh shoulder to +-1.
inline constexpr double knee = 0.5, soft = 1 - knee;
inline double clip(double x) { return std::abs(x) <= knee ? x : std::copysign(knee + soft * std::tanh((std::abs(x) - knee) / soft), x); }

// What that clip took off, in dB with the sample's sign: nothing until the sample is past the converter's full scale.
inline float clipped(double x) { return std::abs(x) > 1 ? (float) std::copysign(20 * std::log10(std::abs(x) / clip(std::abs(x))), x) : 0.0f; }

// Live display feed: the audio thread writes what really happened to each sample (the dry input lined up with the output,
// the output, what the codec took away, whether the codec lost the frame it came from, and where it clipped) with relaxed
// atomic stores; the editor takes everything written since it last looked. Lock-free and allocation-free on both sides; a
// frame torn by a concurrent write only shows as one odd sample. Also the output's peak since the editor last took it.
struct Scope
{
    static constexpr int size = 1 << 15; // 170 ms at 192 kHz, 5 editor frames
    std::array<std::atomic<float>, size> dry {}, wet {}, gone {};
    std::array<std::atomic<float>, size> cut {}, over {}; // clipping in dB, signed as the sample: what the drive's clip took off one past full scale, and how far the output is past 0 dBFS
    std::array<std::atomic<uint8_t>, size> mark {};
    std::atomic<uint32_t> write { 0 }, origin { 0 }; // origin: where in that count the codec's own began
    std::atomic<float> peak { 0 };
    double (*clock)() = [] { return juce::Time::getMillisecondCounterHiRes(); }; // ms; tests substitute their own
    void push(uint32_t at, float d, float w, float g, uint8_t m, float c = 0, float o = 0)
    {
        const auto i = (size_t) (at & (size - 1));
        dry[i].store(d, std::memory_order_relaxed), wet[i].store(w, std::memory_order_relaxed);
        gone[i].store(g, std::memory_order_relaxed), mark[i].store(m, std::memory_order_relaxed);
        cut[i].store(c, std::memory_order_relaxed), over[i].store(o, std::memory_order_relaxed);
    }
    void loudest(float p) // audio thread: raises the peak unless the editor just took it
    {
        if (p > peak.load(std::memory_order_relaxed)) peak.store(p, std::memory_order_relaxed);
    }
};
struct Preset
{
    const char* name;
    float v[16]; // every parameter before bypass, in ID order, plain units (choices and switches by index)
    float x[12] { 0, 100, 0, 0, 0, 0, -96, 0, 20, 20000, 3, 0 }; // 0.3.0's, loss .. invert (a preset turns auto gain, gone from the panel, off)
};
inline constexpr int numPresets = 41;
extern const Preset presets[numPresets]; // 0..24 as in 0.2, in their order
extern const int presetOrder[numPresets]; // how the arrows step: init, 0.3.0's, 0.2's
juce::Image plate(int k); // the editor's plates: ink, tint, paper
} // namespace crumb

class CrumbProcessor : public juce::AudioProcessor, private juce::AudioProcessorValueTreeState::Listener, private juce::AsyncUpdater
{
public:
    static constexpr int stateVersion = 4; // 4 (0.4.0): the same parameters, auto gain off by default and off the panel; 3 (0.3.0): 13 more parameters, missing = neutral; 2 (0.2.0): autoGain, missing (1) = off

    explicit CrumbProcessor(juce::File presetDirectory = {});
    ~CrumbProcessor() override;
    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    // Factory presets are the host's programs. User presets live in presetFolder() and are reached by prev/next.
    int getNumPrograms() override { return crumb::numPresets; }
    int getCurrentProgram() override { return juce::jmax(0, preset.load()); }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    crumb::Scope scope;
    std::atomic<double> sampleRate { 48000 };
    std::atomic<int> editorWidth { 0 }; // 0 = never resized; saved as an additive root attribute

    // Preset UI and host-state transactions. preset: 0..40 factory, -1 user file or unknown (old state).
    juce::File presetFolder() const;
    void stepPreset(int delta); // through factory presets, then the user files
    juce::File saveUserPreset();
    juce::String presetName() const;
    std::atomic<int> preset { 0 };
    void randomize(int64_t seed); // a new sound from the controls; mix, output and bypass stay
    // What the editor's filter controls show, and (before one is edited) the same sound moved onto them: a 0.2 session's
    // filter type and cutoff.
    struct BandView { float low, high; bool invert, legacy; };
    BandView band() const;
    void adoptBand();
    juce::StringArray userPresets() const; // names, in the arrows' order
    void loadUserPreset(const juce::String& name);
    bool codecOn() const; // loss or drop is up: the codec runs, with its latency
    int abSlot() const { return ab.load(); }
    void selectAb(int slot);

private:
    void parameterChanged(const juce::String&, float) override;
    void handleAsyncUpdate() override;
    void updateLatency();
    void setAll(const float* normalised); // every parameter but bypass, as one gesture each
    void applyParams(const juce::XmlElement& state, bool bypass); // all IDs (bypass optional); missing or unreadable -> default, missing autoGain -> off
    void setIdentity(int index, const juce::String& name);
    void serialize(juce::MemoryBlock&);
    struct Transaction;
    juce::CriticalSection txLock; // writers only: presets, A/B, restore. May be held across host callbacks and I/O.
    mutable juce::CriticalSection stateLock; // brief: `busy`, `committed`, userName. Never held across a callback.
    bool busy = false; // a writer is publishing: saves return `committed`, the state before it
    juce::MemoryBlock committed;
    const juce::File presetDirectory;
    std::atomic<bool> resetPending { false }; // set by prepareToPlay, taken by the first block after it
    std::vector<float> slots[2]; // written by writers only, read by serialize()
    int slotPreset[2] { 0, 0 };
    std::atomic<int> ab { 0 };
    juce::String userName, slotName[2];
    struct Dsp;
    std::unique_ptr<Dsp> dsp; // allocated once in the constructor, never on the audio thread
};
