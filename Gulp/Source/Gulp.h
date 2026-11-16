#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>

// gulp: sidechain pumping / ducking by daveydidit (clean room: original curves and UI, textbook DSP).
// FROZEN: the 18 parameter IDs in Gulp.cpp (order, ranges, choice indices, ParameterID versions) and the state format
// (APVTS XML <GULP stateVersion="2"> plus additive root attributes). Never rename, remove or reorder; only append.
namespace gulp
{
inline constexpr int numCurves = 12, numRates = 16, numColors = 8, numPresets = 10, numParams = 18;
extern const char* const curveNames[numCurves];
extern const char* const rateNames[numRates];
extern const char* const colorNames[numColors];
// One cycle of each rate in whole notes: rateNum / rateDen.
inline constexpr int rateNum[numRates] { 1, 1, 1, 3, 1, 1, 3, 1, 1, 3, 1, 1, 3, 1, 2, 4 };
inline constexpr int rateDen[numRates] { 32, 24, 16, 32, 12, 8, 16, 6, 4, 8, 3, 2, 4, 1, 1, 1 };
// The 12 curves: duck amount at phase 0..1 (0 = full level, 1 = silent); s scales the release (0.5 .. 2).
double duck(int curve, double phase, double s);
// One cycle of the duck in 1024 steps (+ the end point): the curve with its slope % and invert, rounded by soothe 0..1
// (three circular box blurs of radius blur(soothe) steps, so it reaches 3 * blur steps either side). The DSP and the
// graph both read it. Triggered cycles additionally taper to idle at their boundaries.
using Table = std::array<double, 1025>;
inline int blur(double soothe) { return (int) std::lround(80 * soothe); }
void shape(Table&, int curve, double slope, bool invert, double soothe, bool triggered = false);

// Audio thread -> editor, relaxed atomics: where the cycle is (the beat in sync, the time since the trigger otherwise),
// the duck amount and its held peak (the editor may zero it), the peak (linear, per block) of the sidechain follower the
// threshold sees and of the output, and one cycle of the sidechain and of the post-trim output folded into `bins`
// columns (signed min and max): the kick ghost and the output scope. live: false while the host's transport is
// stopped; the cycle, the duck, the peak and the bins then hold still.
struct Feed
{
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free, "audio telemetry must be lock-free");
    static constexpr int bins = 140;
    std::atomic<float> phase { 0 }, duck { 0 }, held { 0 }, scLevel { 0 }, outLevel { 0 };
    std::atomic<bool> live { true };
    std::array<std::atomic<float>, bins> lo {}, hi {}, outLo {}, outHi {};
};

struct Preset
{
    const char* name;
    float v[numParams]; // plain values in ID order, choices and switches by index
};
extern const Preset presets[numPresets];
} // namespace gulp

class GulpProcessor : public juce::AudioProcessor
{
public:
    static constexpr int stateVersion = 2;

    explicit GulpProcessor(juce::File presetDirectory = {});
    ~GulpProcessor() override;
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
    double getTailLengthSeconds() const override { return 0.0; }
    // Factory presets are the host's programs. User presets live in presetFolder() and are reached by prev/next.
    int getNumPrograms() override { return gulp::numPresets; }
    int getCurrentProgram() override { return juce::jmax(0, preset.load()); }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    gulp::Feed feed;
    std::atomic<int> editorWidth { 0 }; // 0 = never resized; saved as an additive root attribute

    // Presets and A/B. preset: factory index, -1 = a user file or a custom sound.
    juce::File presetFolder() const;
    void stepPreset(int delta); // through the factory presets, then the user files
    juce::File saveUserPreset();
    juce::String presetName() const;
    std::atomic<int> preset { 0 };
    int abSlot() const { return ab.load(); }
    void selectAb(int slot);

private:
    void setAll(const float* normalised); // one gesture per parameter that changes
    void applyParams(const juce::XmlElement& state); // missing or unreadable -> default; migrates older versions
    void setIdentity(int index, const juce::String& name);
    mutable juce::CriticalSection lock; // identity and A/B slots; never held across a host callback
    const juce::File presetDirectory;
    std::vector<float> slots[2];
    int slotPreset[2] { 0, 0 };
    std::atomic<int> ab { 0 };
    juce::String userName, slotName[2];
    std::atomic<bool> resetPending { false }; // set by prepareToPlay, taken by the first block after it
    struct Dsp;
    std::unique_ptr<Dsp> dsp; // allocated once in the constructor, never on the audio thread
};
