#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>

// silt: a granular effect by daveydidit (clean room: original engine, curves and UI, textbook DSP).
// FROZEN: the parameter IDs in Silt.cpp (order, ranges, choice indices, ParameterID versions) and the state format
// (APVTS XML <SILT stateVersion="1"> plus additive root attributes). Never rename, remove or reorder; only append.
namespace silt
{
inline constexpr int numParams = 36, numPresets = 10, numWindows = 12, numNotes = 17, numRatios = 13, maxGrains = 128;
extern const char* const windowNames[numWindows];
extern const char* const noteNames[numNotes]; // 1/64 .. 2 bars, incl. triplets (T) and dotted (D)
// Note values in whole notes: noteNum / noteDen.
inline constexpr int noteNum[numNotes] { 1, 1, 1, 1, 1, 3, 1, 1, 3, 1, 1, 3, 1, 1, 3, 1, 2 };
inline constexpr int noteDen[numNotes] { 64, 48, 32, 24, 16, 32, 12, 8, 16, 6, 4, 8, 3, 2, 4, 1, 1 };
// grid "lock": size in grain spacings, position in clock periods.
inline constexpr double sizeRatio[numRatios] { 0.25, 1 / 3.0, 0.5, 2 / 3.0, 1, 1.5, 2, 3, 4, 6, 8, 12, 16 };
inline constexpr double backRatio[numRatios] { 0, 0.25, 0.5, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
inline constexpr double maxBackSeconds = 8; // the capture buffer grains read from

// A grain window at x in 0..1: a decay curve f (1 at its peak, 0 at its end) mirrored around the peak at `skew`
// (0..1, the attack's share). The 12 decay curves are gulp's duck curves normalised to end at 1.
double window(int shape, double x, double skew);

// Audio thread -> editor, relaxed atomics. wave: the peak of each 8 s / bins slice of each capture buffer (input,
// sidechain), a ring whose newest finished slice is at[...] - 1; it stops while frozen. Up to `dots` live grains: how
// far back they read (s), their pitch (st) and envelope (0 = no grain). centre: where new grains read (s, incl. scan).
struct Feed
{
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<int>::is_always_lock_free
                  && std::atomic<juce::int64>::is_always_lock_free && std::atomic<bool>::is_always_lock_free, "audio telemetry must be lock-free");
    static constexpr int bins = 224, dots = maxGrains;
    std::array<std::atomic<float>, bins> wave[2] {};
    std::atomic<int> at[2] {};
    std::array<std::atomic<float>, dots> back {}, pitch {}, env {};
    std::atomic<float> centre { 0 };
    std::atomic<bool> frozen { false };
    std::atomic<juce::int64> lastFire { -1 };
    std::atomic<float> lastPitch { 0 };
    std::atomic<int> lastLength { 0 };
    std::atomic<int> fired { 0 }; // grains started since prepare (wraps)
};

struct Preset
{
    const char* name;
    float v[numParams]; // plain values in ID order, choices and switches by index
};
extern const Preset presets[numPresets];
} // namespace silt

class SiltProcessor : public juce::AudioProcessor
{
public:
    static constexpr int stateVersion = 1;

    explicit SiltProcessor(juce::File presetDirectory = {});
    ~SiltProcessor() override;
    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }
    // Factory presets are the host's programs. User presets live in presetFolder() and are reached by prev/next.
    int getNumPrograms() override { return silt::numPresets; }
    int getCurrentProgram() override { return juce::jmax(0, preset.load()); }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    silt::Feed feed;
    std::atomic<int> editorWidth { 0 }; // 0 = never resized; saved as an additive root attribute
    std::atomic<bool> showMacros { false }; // the editor's macro panel; saved as an additive root attribute

    // Presets and A/B. preset: factory index, -1 = a user file or a custom sound.
    juce::File presetFolder() const;
    void stepPreset(int delta); // through the factory presets, then the user files
    juce::File saveUserPreset();
    juce::String presetName() const;
    std::atomic<int> preset { 0 };
    int abSlot() const { return ab.load(); }
    void selectAb(int slot);
    // A musical, loudness-safe random sound from `seed`; mix, output and the macros stay. Undo with A/B.
    void randomise(juce::int64 seed);

private:
    void setAll(const float* normalised); // one gesture per parameter that changes
    void applyParams(const juce::XmlElement& state); // missing or unreadable -> default
    void setIdentity(int index, const juce::String& name);
    mutable juce::CriticalSection lock; // identity and A/B slots; never held across a host callback
    const juce::File presetDirectory;
    std::vector<float> slots[2];
    int slotPreset[2] { 0, 0 };
    std::atomic<int> ab { 0 };
    juce::String userName, slotName[2];
    std::atomic<bool> resetPending { false }; // set by prepareToPlay, taken by the first block after it
    struct Engine;
    std::unique_ptr<Engine> engine; // allocated in the constructor and prepareToPlay, never on the audio thread
};
