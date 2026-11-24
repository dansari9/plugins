#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <optional>

// silt: a granular effect by daveydidit (clean room: original engine, curves and UI, textbook DSP).
// FROZEN: the parameter IDs in Silt.cpp (order, ranges, choice indices, ParameterID versions) and the state format
// (APVTS XML <SILT stateVersion="2"> plus additive root attributes). Never rename, remove or reorder; only append.
// The first numParamsV1 are 0.1.0's (version hint 1); 0.2 appended up to numParamsV2 (hint 2), each defaulting to 0.1.0's
// sound; 0.4.2 appended pitchSnap (hint 3: semi, or free for a saved pitch between semitones).
namespace silt
{
inline constexpr int numParamsV1 = 18, numParamsV2 = 38, numParams = 39, numPresets = 20, numWindows = 3, numNotes = 17, maxGrains = 128;
extern const char* const noteNames[numNotes]; // 1/64 .. 2 bars, incl. triplets (T) and dotted (D)
// Note values in whole notes: noteNum / noteDen.
inline constexpr int noteNum[numNotes] { 1, 1, 1, 1, 1, 3, 1, 1, 3, 1, 1, 3, 1, 1, 3, 1, 2 };
inline constexpr int noteDen[numNotes] { 64, 48, 32, 24, 16, 32, 12, 8, 16, 6, 4, 8, 3, 2, 4, 1, 1 };
inline constexpr double maxBackSeconds = 8; // the capture buffer grains read from

// A grain window at x in 0..1: a decay curve f (1 at its peak, 0 at its end) mirrored around the peak at `skew`
// (0..1, the attack's share). The hann, dome and lean curves are normalised to end at 1.
double window(int shape, double x, double skew);

// Audio thread -> editor, lock-free atomics. wave is the main capture's peak ring (8 s / bins per slice),
// with the newest completed slice at at - 1; it stops while frozen. env reports live voice envelopes for
// diagnostics; the editor draws actual grain footprints from history. centre is the new-grain read position (s).
struct Feed
{
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<int>::is_always_lock_free
                  && std::atomic<juce::int64>::is_always_lock_free && std::atomic<bool>::is_always_lock_free, "audio telemetry must be lock-free");
    static constexpr int bins = 224, dots = maxGrains;
    std::array<std::atomic<float>, bins> wave {};
    std::atomic<int> at {};
    std::array<std::atomic<float>, dots> env {};
    std::atomic<float> centre { 0 };
    std::atomic<bool> frozen { false };
    std::atomic<juce::int64> lastFire { -1 };
    std::atomic<float> lastPitch { 0 };
    std::atomic<int> lastLength { 0 };
    struct Event
    {
        std::atomic<unsigned> sequence { 0 }, grain { 0 };
        std::atomic<juce::int64> frame { -1 }, captureHead { 0 };
        std::atomic<float> back { 0 }, pitch { 0 }, size { 0 };
    };
    static constexpr size_t historySize = 2048;
    std::array<Event, historySize> history;
    std::atomic<juce::int64> time { 0 }, events { 0 }, captureHead { 0 };
    std::atomic<float> sampleRate { 48000 };
    std::atomic<int> note { 60 };
    std::atomic<bool> held { false }; // a MIDI note is transposing the grains
    std::array<std::atomic<float>, 3> meter {};
    std::atomic<int> fired { 0 }; // grains started since prepare (wraps)
    std::atomic<float> tempo { 120 }; // the host's, for typed note values
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
    static constexpr int stateVersion = 2;

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
    // After the input stops: grains read up to 8 s back and last up to 4 s (free), then reverb and feedback decay.
    double getTailLengthSeconds() const override { return 20.0; }
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
    std::atomic<int> editorWidth { 0 }; // in 0.1.0's 1024-px-wide units (0 = never resized); saved as an additive root attribute. An old showMacros attribute is ignored.

    // Presets and A/B. preset: factory index, -1 = a user file or a custom sound.
    juce::File presetFolder() const;
    void stepPreset(int delta); // through the factory presets, then the user files
    juce::File saveUserPreset();
    void loadUserPreset(const juce::File& file);
    juce::String presetName() const;
    std::atomic<int> preset { 4 };
    int abSlot() const { return ab.load(); }
    void selectAb(int slot, bool copy = false);
    // A musical, loudness-safe random sound from `seed`; mix, output, trigger, quality, onset and the macros stay. Undo with A/B.
    void randomise(juce::int64 seed);
    // Nudge continuous parameters by up to 15% of travel, preserving mix, output and macros and backing up to the other slot.
    void nudge(juce::int64 seed);
    // A typed value for a float parameter (PLAN 4.1 grammar), as a clamped plain value; nothing if the text is invalid.
    std::optional<float> parse(const juce::String& id, const juce::String& text) const;

private:
    void setAll(const float* normalised); // one gesture per parameter that changes
    void applyParams(const juce::XmlElement& state, bool gestures); // missing or unreadable -> default
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
