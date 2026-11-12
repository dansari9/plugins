#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <complex>
#include <string_view>
#include <vector>

// crumb: vintage-sampler bit crusher by daveydidit (clean-room: textbook DSP only).
// FROZEN: the parameter IDs listed in Crumb.cpp (ParameterID version 1) and the state format
// (APVTS XML <CRUMB stateVersion="1">, plus additive root attributes). Never rename, remove or reorder; only append.
namespace crumb
{
void fft(std::vector<std::complex<double>>& x, bool inverse); // radix-2 in place, size a power of two
// The whole document is the flat state grammar: <CRUMB> with attributes, children at most one level deep, matching
// closing tags, complete character references, no DTD. JUCE's XmlDocument checks none of that, so it is run first.
bool isFlatCrumbXml(std::string_view xml);

// Live display feed: the audio thread writes (in, out) pairs with relaxed atomic stores, the editor copies the newest n.
// Lock-free and allocation-free on both sides; a frame torn by a concurrent write only shows as one odd sample.
struct Scope
{
    static constexpr int size = 1 << 14; // 85 ms at 192 kHz: a 20 ms window plus room to find a trigger
    std::array<std::atomic<float>, size> in {}, out {};
    std::atomic<uint32_t> write { 0 };
    std::atomic<float> peak[2] {}; // output meter, taken (exchanged with 0) by the editor
    void push(uint32_t at, float i, float o) { in[(size_t) (at & (size - 1))].store(i, std::memory_order_relaxed), out[(size_t) (at & (size - 1))].store(o, std::memory_order_relaxed); }
    void read(float* i, float* o, int n) const // the newest n <= size pairs, oldest first
    {
        const auto w = write.load(std::memory_order_acquire);
        for (uint32_t k = 0; k < (uint32_t) n; ++k)
            i[k] = in[(size_t) ((w - (uint32_t) n + k) & (size - 1))].load(std::memory_order_relaxed), o[k] = out[(size_t) ((w - (uint32_t) n + k) & (size - 1))].load(std::memory_order_relaxed);
    }
};
struct Preset
{
    const char* name;
    float v[16]; // every parameter but bypass, in ID order, plain units (choices and switches by index)
};
extern const Preset presets[25];
} // namespace crumb

class CrumbProcessor : public juce::AudioProcessor
{
public:
    static constexpr int stateVersion = 1;

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
    const juce::String getName() const override { return "Crumb"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    // Factory presets are the host's programs. User presets live in presetFolder() and are reached by prev/next.
    int getNumPrograms() override { return 25; }
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

    // Preset UI and host-state transactions. preset: 0..24 factory, -1 user file or unknown (old state).
    juce::File presetFolder() const;
    void stepPreset(int delta); // through factory presets, then the user files
    juce::File saveUserPreset();
    juce::String presetName() const;
    std::atomic<int> preset { 0 };
    int abSlot() const { return ab.load(); }
    void selectAb(int slot);

private:
    void setAll(const float* normalised); // every parameter but bypass, as one gesture each
    void applyParams(const juce::XmlElement& state, int count); // the first `count` IDs; missing or unreadable -> default
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
