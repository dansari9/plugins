// Tests of the real plug-in: the JUCE processor, its parameters and state,
// and the editor, run without a host or an audio device.

#include "TestFramework.h"
#include "TestSignals.h"

#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <limits>

using namespace rootnote;
using namespace testsignals;

namespace
{
using Layout = juce::AudioProcessor::BusesLayout;

Layout layout(int inputs, int outputs)
{
    Layout l;
    l.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(inputs));
    l.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(outputs));
    return l;
}

void prepare(RootnoteProcessor& processor, double fs = 48000.0, int blockSize = 512, int inputs = 2, int outputs = 2)
{
    REQUIRE(processor.setBusesLayout(layout(inputs, outputs)));
    processor.setRateAndBufferSizeDetails(fs, blockSize);
    processor.prepareToPlay(fs, blockSize);
}

void set(RootnoteProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getApvts().getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

float get(RootnoteProcessor& processor, const char* id)
{
    return processor.getApvts().getRawParameterValue(id)->load();
}

/** Allow float round-off and 1% of a quantisation step, never a whole step. */
double parameterTolerance(const juce::RangedAudioParameter& parameter, double expected)
{
    const double step = static_cast<double>(parameter.getNormalisableRange().interval);
    return std::max(0.01 * step, 8.0 * static_cast<double>(std::numeric_limits<float>::epsilon())
                                      * std::max(1.0, std::abs(expected)));
}

bool holds(RootnoteProcessor& processor, const char* id, float expected)
{
    const auto* parameter = processor.getApvts().getParameter(id);
    return std::abs(static_cast<double>(get(processor, id)) - static_cast<double>(expected))
        <= parameterTolerance(*parameter, static_cast<double>(expected));
}

/** The same for values in the engine's units. */
bool close(double actual, double expected)
{
    return std::abs(actual - expected) <= 1.0e-5 * std::max(1.0, std::abs(expected));
}

juce::String textOf(RootnoteProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getApvts().getParameter(id);
    return parameter->getText(parameter->convertTo0to1(value), 64);
}

float parsed(RootnoteProcessor& processor, const char* id, const juce::String& text)
{
    auto* parameter = processor.getApvts().getParameter(id);
    return parameter->convertFrom0to1(parameter->getValueForText(text));
}

bool allFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (!std::isfinite(buffer.getSample(ch, i)))
                return false;
    return true;
}

/**
    Kicks at the given pitch, two per second, processed in blocks at about
    five times real time: the tuner thread is fed through a FIFO that holds
    about a second of audio, so it cannot take a whole song at once.
*/
void playKicks(RootnoteProcessor& processor, double hz, double fs, int hits = 6)
{
    Random random(7);
    std::vector<float> signal(static_cast<size_t>(fs * 0.5 * (hits + 1)), 0.0f);
    KickSpec kick;
    kick.hz = hz;
    for (int hit = 0; hit < hits; ++hit)
        addKick(signal, static_cast<size_t>(fs * (0.1 + 0.5 * hit)), kick, fs, random);

    juce::MidiBuffer midi;
    for (size_t position = 0; position + 512 <= signal.size(); position += 512)
    {
        juce::AudioBuffer<float> block(2, 512);
        block.copyFrom(0, 0, signal.data() + position, 512);
        block.copyFrom(1, 0, signal.data() + position, 512);
        processor.processBlock(block, midi);
        juce::Thread::sleep(2);
    }
}

/** The tuner runs on its own thread; give it up to five seconds. */
bool waitForReading(RootnoteProcessor& processor, uint32_t hits)
{
    for (int attempt = 0; attempt < 500; ++attempt)
    {
        if (processor.tunerReading().pitchedHits >= hits)
            return true;
        juce::Thread::sleep(10);
    }
    return false;
}
} // namespace

TEST_CASE("Plugin: identity")
{
    RootnoteProcessor processor;
    CHECK(processor.getName() == "Rootnote");
    CHECK(processor.acceptsMidi());
    CHECK(!processor.producesMidi());
    CHECK(!processor.isMidiEffect());
    CHECK(processor.hasEditor());
    CHECK(processor.getBypassParameter() == processor.getApvts().getParameter(ids::bypass));
}

TEST_CASE("Plugin: every parameter of the plan exists with its range and default")
{
    struct Expected { const char* id; float low, high, value; };
    const Expected table[] = {
        { ids::boostNote, 12, 96, 36 }, { ids::boostFine, -50, 50, 0 }, { ids::boostGain, 0, 24, 9 },
        { ids::boostQ, 0.5f, 200, 12 }, { ids::series, 0, 2, 0 }, { ids::bands, 1, 32, 8 },
        { ids::blend, 0, 1, 0.5f }, { ids::focusLink, 0, 1, 1 }, { ids::focus, 20, 8000, 65 },
        { ids::range, 0.1f, 6, 1.5f }, { ids::tilt, -12, 6, -3 }, { ids::qMode, 0, 1, 0 },
        { ids::cutOn, 0, 1, 0 }, { ids::cutNote, 12, 96, 36 }, { ids::cutDepth, -30, 0, -12 },
        { ids::cutQ, 0.5f, 60, 8 }, { ids::cutOvertones, 0, 100, 50 }, { ids::cutBands, 1, 16, 6 },
        { ids::ring, 0, 100, 0 }, { ids::ringDecay, 50, 4000, 600 }, { ids::ringDrop, 0, 12, 0 },
        { ids::midiMode, 0, 2, 1 }, { ids::glide, 0, 500, 0 }, { ids::noteOff, 0, 1, 0 },
        { ids::key, 0, 11, 0 }, { ids::scale, 0, 8, 0 }, { ids::keyLock, 0, 1, 0 },
        { ids::a4, 415, 466, 440 }, { ids::mix, 0, 100, 100 }, { ids::output, -24, 12, 0 },
        { ids::limiter, 0, 1, 1 }, { ids::bypass, 0, 1, 0 }
    };

    RootnoteProcessor processor;
    CHECK(processor.getParameters().size() == static_cast<int>(std::size(table)));
    for (const auto& expected : table)
    {
        auto* parameter = processor.getApvts().getParameter(expected.id);
        REQUIRE(parameter != nullptr);
        const auto range = parameter->getNormalisableRange();
        CHECK_NEAR_MSG(range.start, expected.low, 1.0e-6, expected.id);
        CHECK_NEAR_MSG(range.end, expected.high, 1.0e-6, expected.id);
        const double tolerance = parameterTolerance(*parameter, static_cast<double>(expected.value));
        CHECK_NEAR_MSG(get(processor, expected.id), expected.value, tolerance, expected.id);
        CHECK_NEAR_MSG(parameter->convertFrom0to1(parameter->getDefaultValue()), expected.value, tolerance, expected.id);
    }

    // The defaults reach the engine in its own units.
    const EngineParams read = ParameterReader(processor.getApvts()).read();
    const EngineParams defaults;
    CHECK(close(read.boostNote, defaults.boostNote) && close(read.boostFineCents, defaults.boostFineCents));
    CHECK(close(read.boostGainDb, defaults.boostGainDb) && close(read.boostQ, defaults.boostQ));
    CHECK(read.series == defaults.series && read.bands == defaults.bands && close(read.blend, defaults.blend));
    CHECK(read.focusLink == defaults.focusLink && close(read.focusHz, defaults.focusHz));
    CHECK(close(read.rangeOct, defaults.rangeOct) && close(read.tiltDbPerOct, defaults.tiltDbPerOct));
    CHECK(read.qMode == defaults.qMode && read.cutOn == defaults.cutOn && close(read.cutNote, defaults.cutNote));
    CHECK(close(read.cutDepthDb, defaults.cutDepthDb) && close(read.cutQ, defaults.cutQ));
    CHECK(close(read.cutOvertones, defaults.cutOvertones) && read.cutBands == defaults.cutBands);
    CHECK(close(read.ring, defaults.ring) && close(read.ringDecaySeconds, defaults.ringDecaySeconds));
    CHECK(close(read.ringDropSemitones, defaults.ringDropSemitones) && read.midiMode == defaults.midiMode);
    CHECK(close(read.glideSeconds, defaults.glideSeconds) && read.noteOff == defaults.noteOff);
    CHECK(read.key == defaults.key && read.scale == defaults.scale && read.keyLock == defaults.keyLock);
    CHECK(close(read.a4, defaults.a4) && close(read.mix, defaults.mix) && close(read.outputDb, defaults.outputDb));
    CHECK(read.limiter == defaults.limiter && read.bypass == defaults.bypass);
}

TEST_CASE("Plugin: parameter round trips tolerate rounding but reject a changed value")
{
    RootnoteProcessor processor;
    for (auto* base : processor.getParameters())
    {
        auto* parameter = static_cast<juce::RangedAudioParameter*>(base);
        const auto& range = parameter->getNormalisableRange();
        for (float position : { 0.0f, 0.13f, 0.5f, 0.79f, 1.0f })
        {
            const float expected = range.snapToLegalValue(parameter->convertFrom0to1(position));
            set(processor, parameter->getParameterID().toRawUTF8(), expected);
            CHECK_MSG(holds(processor, parameter->getParameterID().toRawUTF8(), expected),
                      parameter->getParameterID().toStdString());
            const double tolerance = parameterTolerance(*parameter, static_cast<double>(expected));
            const float different = static_cast<float>(static_cast<double>(expected) + 4.0 * tolerance);
            CHECK(!holds(processor, parameter->getParameterID().toRawUTF8(), different));
        }
    }
    // Symmetric stepped ranges can denormalise zero to a few millionths.
    set(processor, ids::boostFine, 0.0f);
    CHECK(holds(processor, ids::boostFine, 0.0f));
}

TEST_CASE("Plugin: parameter text shows units and typed values are understood")
{
    RootnoteProcessor processor;
    CHECK(textOf(processor, ids::boostNote, 30.0f) == "F#1");
    CHECK(textOf(processor, ids::cutNote, 30.25f) == "F#1 +25 ct");
    CHECK(textOf(processor, ids::boostGain, 9.0f) == "9.0 dB");
    CHECK(textOf(processor, ids::focus, 65.0f) == "65.0 Hz");
    CHECK(textOf(processor, ids::focus, 1200.0f) == "1.20 kHz");
    CHECK(textOf(processor, ids::ringDecay, 600.0f) == "600 ms");
    CHECK(textOf(processor, ids::ringDecay, 2000.0f) == "2.00 s");
    CHECK(textOf(processor, ids::glide, 40.0f) == "40 ms");
    CHECK(textOf(processor, ids::blend, 0.25f) == "75 / 25");
    CHECK(textOf(processor, ids::tilt, -3.0f) == "-3.0 dB/oct");
    CHECK(textOf(processor, ids::a4, 432.0f) == "432.0 Hz");
    CHECK(textOf(processor, ids::series, 2.0f) == "Drum");
    CHECK(textOf(processor, ids::scale, 8.0f) == "Harmonic Minor");

    CHECK_NEAR(parsed(processor, ids::boostNote, "F#1"), 30.0, 1.0e-3);
    CHECK_NEAR(parsed(processor, ids::boostNote, "Gb1"), 30.0, 1.0e-3);
    CHECK_NEAR(parsed(processor, ids::boostNote, "46 Hz"), hzToNote(46.0), 0.006);
    CHECK_NEAR(parsed(processor, ids::boostNote, "C9"), 96.0, 1.0e-3); // clamped to the range
    CHECK_NEAR(parsed(processor, ids::output, "-6 dB"), -6.0, 1.0e-3);
    CHECK_NEAR(parsed(processor, ids::output, juce::String::fromUTF8("\xE2\x88\x92" "6")), -6.0, 1.0e-3);
    CHECK_NEAR(parsed(processor, ids::focus, "1.2 kHz"), 1200.0, 0.5);
    CHECK_NEAR(parsed(processor, ids::ringDecay, "1.5 s"), 1500.0, 0.5);
    CHECK_NEAR(parsed(processor, ids::ringDecay, "250 ms"), 250.0, 0.1);
    CHECK_NEAR(parsed(processor, ids::blend, "75 / 25"), 0.25, 1.0e-3);
    CHECK_NEAR(parsed(processor, ids::mix, "70 %"), 70.0, 1.0e-3);

    // A typed frequency follows the A4 setting.
    set(processor, ids::a4, 432.0f);
    CHECK_NEAR(parsed(processor, ids::cutNote, "54 Hz"), 33.0, 0.006);
}

TEST_CASE("Plugin: state round trip restores every parameter and the editor size")
{
    RootnoteProcessor source;
    juce::Random random(99);
    for (auto* parameter : source.getParameters())
    {
        // Snapped to a value the parameter can hold (a step, a choice, on or off).
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter);
        ranged->setValueNotifyingHost(ranged->convertTo0to1(ranged->convertFrom0to1(random.nextFloat())));
    }
    source.storeEditorSize(777, 555);

    juce::MemoryBlock state;
    source.getStateInformation(state);
    CHECK(state.getSize() > 0);

    RootnoteProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (int i = 0; i < source.getParameters().size(); ++i)
        CHECK_NEAR_MSG(restored.getParameters()[i]->getValue(), source.getParameters()[i]->getValue(), 1.0e-6,
                       source.getParameters()[i]->getName(64).toStdString());
    CHECK(restored.storedEditorSize() == juce::Point<int>(777, 555));

    // Saving the restored instance gives the same state again.
    juce::MemoryBlock second;
    restored.getStateInformation(second);
    CHECK(second == state);

    // Rubbish and foreign state are ignored.
    const char rubbish[] = "not a state";
    restored.setStateInformation(rubbish, sizeof(rubbish));
    restored.setStateInformation(nullptr, 0);
    CHECK_NEAR(restored.getParameters()[0]->getValue(), source.getParameters()[0]->getValue(), 1.0e-6);
}

TEST_CASE("Plugin: zero latency, and a tail that follows the Ring decay")
{
    RootnoteProcessor processor;
    CHECK(processor.getLatencySamples() == 0);
    CHECK(processor.getTailLengthSeconds() > 0.0);

    prepare(processor);
    CHECK(processor.getLatencySamples() == 0);

    set(processor, ids::boostGain, 0.0f);
    set(processor, ids::ring, 50.0f);
    set(processor, ids::ringDecay, 1000.0f);
    const double oneSecond = processor.getTailLengthSeconds();
    set(processor, ids::ringDecay, 4000.0f);
    CHECK_NEAR(oneSecond, 1.1, 1.0e-3);
    CHECK_NEAR(processor.getTailLengthSeconds(), 4.1, 1.0e-3);
    CHECK(processor.getLatencySamples() == 0);

    // An impulse comes out on the sample it went in on.
    juce::AudioBuffer<float> block(2, 512);
    block.clear();
    block.setSample(0, 200, 0.5f);
    block.setSample(1, 200, 0.5f);
    juce::MidiBuffer midi;
    processor.processBlock(block, midi);
    CHECK(block.getMagnitude(0, 0, 200) == 0.0f);
    CHECK(block.getSample(0, 200) != 0.0f);
}

TEST_CASE("Plugin: bus layouts")
{
    RootnoteProcessor processor;
    CHECK(processor.isBusesLayoutSupported(layout(1, 1)));
    CHECK(processor.isBusesLayoutSupported(layout(1, 2)));
    CHECK(processor.isBusesLayoutSupported(layout(2, 2)));
    CHECK(!processor.isBusesLayoutSupported(layout(2, 1)));
    CHECK(!processor.isBusesLayoutSupported(layout(2, 6)));
}

TEST_CASE("Plugin: processing never produces NaN, at any layout, rate or block size")
{
    struct Config { int inputs, outputs; double fs; int blockSize; };
    const Config configs[] = { { 1, 1, 44100.0, 64 }, { 1, 2, 48000.0, 1 }, { 2, 2, 96000.0, 512 }, { 2, 2, 192000.0, 37 } };

    for (const auto& config : configs)
    {
        RootnoteProcessor processor;
        prepare(processor, config.fs, config.blockSize, config.inputs, config.outputs);
        set(processor, ids::boostGain, 24.0f);
        set(processor, ids::boostQ, 200.0f);
        set(processor, ids::bands, 32.0f);
        set(processor, ids::cutOn, 1.0f);
        set(processor, ids::cutNote, 31.0f);
        set(processor, ids::ring, 100.0f);
        set(processor, ids::ringDrop, 12.0f);

        juce::Random random(5);
        juce::AudioBuffer<float> block(config.outputs, config.blockSize);
        bool finite = true;
        float loudest = 0.0f;
        const int blocks = juce::jmax(40, static_cast<int>(config.fs) / config.blockSize / 4);
        for (int n = 0; n < blocks; ++n)
        {
            for (int ch = 0; ch < config.outputs; ++ch)
                for (int i = 0; i < config.blockSize; ++i)
                    block.setSample(ch, i, random.nextFloat() * 4.0f - 2.0f);

            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1 + n % 3, 24 + (n * 7) % 60, 0.8f), config.blockSize / 2);
            midi.addEvent(juce::MidiMessage::pitchWheel(1, 9000), 0);
            midi.addEvent(juce::MidiMessage::createSysExMessage("0123456789abcdef", 16), 1);
            if (n % 5 == 4)
                midi.addEvent(juce::MidiMessage::allNotesOff(1), config.blockSize - 1);
            processor.processBlock(block, midi);

            finite = finite && allFinite(block);
            loudest = juce::jmax(loudest, block.getMagnitude(0, config.blockSize));
        }
        const std::string name = std::to_string(config.inputs) + ">" + std::to_string(config.outputs) + " at "
                                     + std::to_string(static_cast<int>(config.fs));
        CHECK_MSG(finite, name);
        CHECK_MSG(static_cast<double>(loudest) <= Limiter::kCeiling, name);

        // Empty blocks and a reset are harmless.
        juce::AudioBuffer<float> empty(config.outputs, 0);
        juce::MidiBuffer midi;
        processor.processBlock(empty, midi);
        processor.reset();
        block.clear();
        processor.processBlock(block, midi);
        CHECK_MSG(block.getMagnitude(0, config.blockSize) == 0.0f, name);
    }
}

TEST_CASE("Plugin: mono input is copied to both outputs")
{
    RootnoteProcessor processor;
    prepare(processor, 48000.0, 256, 1, 2);
    juce::AudioBuffer<float> block(2, 256);
    juce::Random random(3);
    for (int i = 0; i < 256; ++i)
    {
        block.setSample(0, i, random.nextFloat() - 0.5f);
        block.setSample(1, i, 123.0f); // stale data the host may leave in the second channel
    }
    juce::MidiBuffer midi;
    processor.processBlock(block, midi);
    bool same = true;
    for (int i = 0; i < 256; ++i)
        same = same && block.getSample(0, i) == block.getSample(1, i);
    CHECK(same);
    CHECK(block.getMagnitude(0, 256) < 2.0f);
}

TEST_CASE("Plugin: the bypass parameter fades to the untouched input")
{
    RootnoteProcessor processor;
    prepare(processor);
    set(processor, ids::boostGain, 18.0f);

    juce::Random random(8);
    juce::MidiBuffer midi;
    const auto runBlock = [&](bool expectDry) {
        juce::AudioBuffer<float> block(2, 512);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
                block.setSample(ch, i, random.nextFloat() * 0.4f - 0.2f);
        juce::AudioBuffer<float> input;
        input.makeCopyOf(block);
        processor.processBlock(block, midi);
        bool dry = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
                dry = dry && block.getSample(ch, i) == input.getSample(ch, i);
        CHECK(dry == expectDry);
    };

    runBlock(false);
    set(processor, ids::bypass, 1.0f);
    runBlock(false); // the 20 ms crossfade
    runBlock(false);
    runBlock(true);
    set(processor, ids::bypass, 0.0f);
    runBlock(false);
}

TEST_CASE("Plugin: MIDI channel 1 plays the boost note and channel 2 the cut note, without touching the parameters")
{
    RootnoteProcessor processor;
    prepare(processor);
    processor.setDisplayActive(true);
    juce::AudioBuffer<float> block(2, 512);
    block.clear();

    juce::MidiBuffer midi;
    processor.processBlock(block, midi);
    auto state = processor.displayState();
    CHECK_NEAR(state.boostHz, noteToHz(36.0), 1.0e-3);
    CHECK(!state.boostFromMidi && !state.cutFromMidi);
    CHECK(state.midiNoteOns == 0u);

    midi.addEvent(juce::MidiMessage::noteOn(1, 45, 1.0f), 0);
    midi.addEvent(juce::MidiMessage::noteOn(2, 50, 1.0f), 10);
    midi.addEvent(juce::MidiMessage::noteOn(3, 70, 1.0f), 20);
    processor.processBlock(block, midi);
    state = processor.displayState();
    CHECK_NEAR(state.boostHz, 110.0, 1.0e-3);
    CHECK_NEAR(state.cutHz, noteToHz(50.0), 1.0e-3);
    CHECK(state.boostFromMidi && state.cutFromMidi);
    CHECK(state.midiNoteOns == 2u);
    CHECK(holds(processor, ids::boostNote, 36.0f));
    CHECK(holds(processor, ids::cutNote, 36.0f));

    // Omni: every channel plays the boost note. A note-on with velocity 0 is a note-off.
    set(processor, ids::midiMode, 2.0f);
    set(processor, ids::noteOff, 1.0f);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOff(1, 45), 0);
    midi.addEvent(juce::MidiMessage::noteOn(9, 57, 1.0f), 0);
    processor.processBlock(block, midi);
    CHECK_NEAR(processor.displayState().boostHz, 220.0, 1.0e-3);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(9, 57, static_cast<juce::uint8>(0)), 0);
    processor.processBlock(block, midi);
    CHECK(!processor.displayState().boostFromMidi);
    CHECK_NEAR(processor.displayState().boostHz, noteToHz(36.0), 1.0e-3);

    // Key lock snaps played notes: C# in C major goes down to C.
    set(processor, ids::keyLock, 1.0f);
    set(processor, ids::scale, 1.0f);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 49, 1.0f), 0);
    processor.processBlock(block, midi);
    CHECK_NEAR(processor.displayState().boostHz, noteToHz(48.0), 1.0e-3);
}

TEST_CASE("Plugin: a restored state plays its own notes, whatever MIDI played before")
{
    RootnoteProcessor processor;
    prepare(processor);
    processor.setDisplayActive(true);
    juce::MemoryBlock state;
    processor.getStateInformation(state);

    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    block.clear();
    processor.processBlock(block, midi);
    midi.clear();
    for (int i = 0; i < 20; ++i)
        processor.processBlock(block, midi);
    CHECK(processor.displayState().boostFromMidi);
    CHECK_NEAR(hzToNote(processor.displayState().boostHz), 60.0, 0.01);

    // Same knob value as before, so only the restore itself can take the note back from MIDI.
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (int i = 0; i < 20; ++i)
        processor.processBlock(block, midi);
    CHECK(!processor.displayState().boostFromMidi);
    CHECK_NEAR(hzToNote(processor.displayState().boostHz), 36.0, 0.01);
}

TEST_CASE("Plugin: a MIDI note takes effect on the sample of its event")
{
    juce::Random random(4);
    juce::AudioBuffer<float> input(2, 512);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 512; ++i)
            input.setSample(ch, i, random.nextFloat() * 0.6f - 0.3f);

    const auto render = [&](int eventAt) {
        RootnoteProcessor processor;
        prepare(processor);
        set(processor, ids::boostGain, 12.0f);
        juce::AudioBuffer<float> block;
        block.makeCopyOf(input);
        juce::MidiBuffer midi;
        if (eventAt >= 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), eventAt);
        processor.processBlock(block, midi);
        return block;
    };

    const auto reference = render(-1);
    for (int eventAt : { 0, 100, 511 })
    {
        const auto played = render(eventAt);
        int firstDifference = 512;
        for (int i = 511; i >= 0; --i)
            if (played.getSample(0, i) != reference.getSample(0, i))
                firstDifference = i;
        CHECK(firstDifference >= eventAt);
        if (eventAt < 400)
            CHECK(firstDifference <= eventAt + 16);
    }
}

TEST_CASE("Plugin: tuner and spectrum only run while an editor is showing")
{
    RootnoteProcessor processor;
    prepare(processor);
    std::vector<float> samples(8192);

    playKicks(processor, 55.0, 48000.0);
    juce::Thread::sleep(150);
    CHECK(!processor.tunerReading().valid);
    CHECK(processor.readSpectrumSamples(samples.data(), samples.size()) == 0);
    CHECK(processor.takeOutputPeak(0) == 0.0f);
    CHECK(!processor.matchCutToHit());
    CHECK(!processor.tuneBoostToKey());

    processor.setDisplayActive(true);
    playKicks(processor, 55.0, 48000.0);
    CHECK(processor.readSpectrumSamples(samples.data(), samples.size()) == samples.size());
    CHECK(waitForReading(processor, 2));
    CHECK(processor.takeOutputPeak(0) > 0.1f);
    CHECK(processor.takeOutputPeak(1) > 0.1f);
    CHECK(processor.takeOutputPeak(0) == 0.0f);
    processor.setDisplayActive(false);
}

TEST_CASE("Plugin: Match cut and Tune to key act on the detected hit")
{
    RootnoteProcessor processor;
    prepare(processor);
    processor.setDisplayActive(true);

    // 20 cents above D1: the cut lands on the hit itself, the boost on the nearest note of the key.
    const double hitNote = 26.2;
    playKicks(processor, noteToHz(hitNote), 48000.0);
    REQUIRE(waitForReading(processor, 3));
    CHECK_NEAR(hzToNote(processor.tunerReading().hz), hitNote, 0.10);

    CHECK(holds(processor, ids::cutOn, 0.0f));
    CHECK(processor.matchCutToHit());
    CHECK(holds(processor, ids::cutOn, 1.0f));
    CHECK_NEAR(get(processor, ids::cutNote), hitNote, 0.10);

    set(processor, ids::boostFine, 30.0f);
    CHECK(processor.tuneBoostToKey());
    CHECK(holds(processor, ids::boostNote, 26.0f));
    CHECK(holds(processor, ids::boostFine, 0.0f));

    // In E minor pentatonic (E G A B D) D is in the scale; in C minor pentatonic (C Eb F G Bb) it is not.
    set(processor, ids::scale, static_cast<float>(Scale::pentatonicMinor));
    set(processor, ids::key, 4.0f);
    CHECK(processor.tuneBoostToKey());
    CHECK(holds(processor, ids::boostNote, 26.0f));
    set(processor, ids::key, 0.0f);
    CHECK(processor.tuneBoostToKey());
    CHECK(holds(processor, ids::boostNote, 27.0f));

    // A4 moves the note names: at A4 = 415 Hz the same hit reads almost a semitone higher.
    set(processor, ids::a4, 415.3f);
    CHECK(processor.matchCutToHit());
    CHECK_NEAR(get(processor, ids::cutNote), hitNote + 1.0, 0.10);
    processor.setDisplayActive(false);
}

TEST_CASE("Plugin: the editor opens, shows the tuner, resizes and closes headlessly")
{
    RootnoteProcessor processor;
    prepare(processor);

    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        REQUIRE(editor != nullptr);
        CHECK(editor->getWidth() == RootnoteEditor::kDefaultWidth);
        CHECK(editor->getHeight() == RootnoteEditor::kDefaultHeight);
        CHECK(editor->isResizable());

        // The open editor switches the tuner on.
        playKicks(processor, 55.0, 48000.0);
        CHECK(waitForReading(processor, 1));

        for (const auto size : { juce::Point<int>(480, 420),
                                 juce::Point<int>(RootnoteEditor::kDefaultWidth, RootnoteEditor::kDefaultHeight),
                                 juce::Point<int>(1400, 1600), juce::Point<int>(700, 500) })
        {
            editor->setSize(size.x, size.y);
            const juce::Image snapshot = editor->createComponentSnapshot(editor->getLocalBounds());
            CHECK(snapshot.getWidth() == size.x && snapshot.getHeight() == size.y);
        }
    }
    CHECK(processor.storedEditorSize() == juce::Point<int>(700, 500));

    // The size comes back with the state, and a second editor can follow the first.
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    RootnoteProcessor other;
    other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    std::unique_ptr<juce::AudioProcessorEditor> editor(other.createEditor());
    CHECK(editor->getWidth() == 700 && editor->getHeight() == 500);
}

TEST_CASE("Plugin: a preset sets its exact values, everything else to default, and leaves the song alone")
{
    RootnoteProcessor processor;
    REQUIRE(RootnoteProcessor::numPresets() == 9);
    CHECK(RootnoteProcessor::presetName(0) == "Default");
    const char* names[] = { "Default", "Sub Afterglow", "Pocket Kick", "Snare Core", "Tom Shift",
                            "Octave Weight", "Resonance Eraser", "Played Percussion", "Noise Lattice" };
    for (int i = 0; i < RootnoteProcessor::numPresets(); ++i)
        CHECK(RootnoteProcessor::presetName(i) == names[i]);
    CHECK(RootnoteProcessor::presetName(9).isEmpty());
    CHECK(processor.currentPreset() == 0);

    // What belongs to the song, plus a few values no preset mentions.
    set(processor, ids::boostNote, 30.0f);
    set(processor, ids::boostFine, 12.0f);
    set(processor, ids::cutNote, 26.0f);
    set(processor, ids::key, 6.0f);
    set(processor, ids::scale, static_cast<float>(Scale::minor));
    set(processor, ids::keyLock, 1.0f);
    set(processor, ids::a4, 432.0f);
    set(processor, ids::bypass, 1.0f);
    set(processor, ids::output, -7.0f);
    set(processor, ids::focusLink, 0.0f);

    struct Value { const char* id; float value; };
    const auto expect = [&](int index, std::initializer_list<Value> values) {
        processor.loadPreset(index);
        CHECK(processor.currentPreset() == index);
        juce::MemoryBlock saved;
        processor.getStateInformation(saved);
        RootnoteProcessor restored;
        restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        CHECK(restored.currentPreset() == index);
        for (auto* parameter : processor.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter);
            const juce::String id = ranged->getParameterID();
            float expected = ranged->convertFrom0to1(ranged->getDefaultValue());
            for (const Value& kept : { Value { ids::boostNote, 30.0f }, { ids::boostFine, 12.0f }, { ids::cutNote, 26.0f },
                                       { ids::key, 6.0f }, { ids::scale, 2.0f }, { ids::keyLock, 1.0f },
                                       { ids::a4, 432.0f }, { ids::bypass, 1.0f } })
                if (id == kept.id)
                    expected = kept.value;
            for (const Value& value : values)
                if (id == value.id)
                    expected = value.value;
            CHECK_MSG(holds(processor, id.toRawUTF8(), expected),
                      (RootnoteProcessor::presetName(index) + ": " + id).toStdString());
            CHECK_MSG(holds(restored, id.toRawUTF8(), expected),
                      ("restored " + RootnoteProcessor::presetName(index) + ": " + id).toStdString());
        }
    };

    expect(1, { { ids::series, 0 }, { ids::bands, 4 }, { ids::boostQ, 80 }, { ids::boostGain, 12 }, { ids::ring, 40 },
                { ids::ringDecay, 900 }, { ids::ringDrop, 2 }, { ids::limiter, 1 } });
    expect(2, { { ids::bands, 6 }, { ids::boostQ, 10 }, { ids::boostGain, 6 }, { ids::range, 1 } });
    expect(3, { { ids::series, 2 }, { ids::bands, 6 }, { ids::boostQ, 18 }, { ids::boostGain, 7 }, { ids::cutOn, 1 },
                { ids::cutDepth, -10 } });
    expect(4, { { ids::series, 2 }, { ids::bands, 8 }, { ids::boostQ, 25 }, { ids::boostGain, 8 }, { ids::cutOn, 1 } });
    expect(5, { { ids::series, 1 }, { ids::boostQ, 14 }, { ids::boostGain, 8 }, { ids::tilt, -6 } });
    expect(6, { { ids::boostGain, 0 }, { ids::cutOn, 1 }, { ids::cutDepth, -18 }, { ids::cutBands, 12 },
                { ids::cutOvertones, 80 } });
    expect(7, { { ids::midiMode, 1 }, { ids::glide, 40 }, { ids::bands, 12 }, { ids::boostQ, 60 }, { ids::ring, 30 } });
    expect(8, { { ids::bands, 24 }, { ids::boostQ, 120 }, { ids::boostGain, 18 }, { ids::mix, 70 } });
    expect(0, {});

    // Out of range: nothing happens.
    processor.loadPreset(3);
    processor.loadPreset(99);
    processor.loadPreset(-1);
    CHECK(processor.currentPreset() == 3);

    // The preset is part of the state.
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    RootnoteProcessor other;
    other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(other.currentPreset() == 3);
    CHECK(holds(other, ids::boostQ, 18.0f)); // preset 3 is Snare Core
}

TEST_CASE("Plugin: A/B keeps two settings apart")
{
    RootnoteProcessor processor;
    CHECK(processor.abSlot() == 0);
    processor.loadPreset(2);
    set(processor, ids::boostGain, 3.0f);

    // B starts as a copy of A.
    processor.selectAbSlot(1);
    CHECK(processor.abSlot() == 1);
    CHECK(holds(processor, ids::boostGain, 3.0f) && processor.currentPreset() == 2);

    processor.loadPreset(5);
    set(processor, ids::mix, 40.0f);
    set(processor, ids::bypass, 1.0f);

    processor.selectAbSlot(0);
    CHECK(processor.currentPreset() == 2);
    CHECK(holds(processor, ids::boostGain, 3.0f) && holds(processor, ids::mix, 100.0f) && holds(processor, ids::series, 0.0f));
    CHECK(holds(processor, ids::bypass, 1.0f)); // bypass is not part of a slot

    processor.selectAbSlot(1);
    CHECK(processor.currentPreset() == 5);
    CHECK(holds(processor, ids::boostGain, 8.0f) && holds(processor, ids::mix, 40.0f) && holds(processor, ids::series, 1.0f));

    processor.selectAbSlot(1);
    processor.selectAbSlot(7);
    CHECK(processor.abSlot() == 1);

    // A restored state starts again from A.
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(processor.abSlot() == 0);
    processor.selectAbSlot(1);
    CHECK(holds(processor, ids::mix, 40.0f));
}

TEST_CASE("Plugin: the display state has the live notes and bands, with or without audio")
{
    RootnoteProcessor processor;
    set(processor, ids::boostNote, 30.0f);
    set(processor, ids::cutNote, 26.0f);

    // No audio yet: the knob notes.
    auto state = processor.displayState();
    CHECK_NEAR(state.boostHz, noteToHz(30.0), 1.0e-3);
    CHECK_NEAR(state.cutHz, noteToHz(26.0), 1.0e-3);
    CHECK(state.numBoostBands == 8 && state.numCutBands == 0);
    CHECK_NEAR(state.boostBands[0].gainDb, 9.0, 1.0e-3);
    CHECK(state.limiterGain == 1.0f);

    set(processor, ids::cutOn, 1.0f);
    CHECK(processor.displayState().numCutBands == 6);

    // The bands follow a MIDI note.
    prepare(processor);
    processor.setDisplayActive(true);
    juce::AudioBuffer<float> block(2, 512);
    block.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 45, 1.0f), 0);
    processor.processBlock(block, midi);
    midi.clear();
    for (int i = 0; i < 10; ++i)
        processor.processBlock(block, midi);
    state = processor.displayState();
    CHECK(state.boostFromMidi);
    CHECK_NEAR(state.boostBands[0].hz, 110.0, 1.0e-3);
    CHECK_NEAR(state.boostBands[1].hz, 220.0, 1.0e-3);
    CHECK_NEAR(state.cutBands[0].hz, noteToHz(26.0), 1.0e-3);
    CHECK_NEAR(state.cutBands[0].gainDb, -12.0, 1.0e-3);
}

TEST_CASE("Plugin: display frames contain smoothed gains and stop collecting when closed")
{
    RootnoteProcessor processor;
    prepare(processor, 48000.0, 128);
    processor.setDisplayActive(true);
    juce::AudioBuffer<float> block(2, 128);
    juce::MidiBuffer midi;
    block.clear();
    processor.processBlock(block, midi);
    CHECK_NEAR(processor.displayState().boostBands[0].gainDb, 9.0, 1.0e-3);
    set(processor, ids::boostGain, 24.0f);
    processor.processBlock(block, midi);
    const auto moving = processor.displayState();
    CHECK(moving.boostBands[0].gainDb > 9.0 && moving.boostBands[0].gainDb < 24.0);

    processor.setDisplayActive(false);
    // Samples from the previous editor are discarded before the next opens.
    processor.setDisplayActive(true);
    float samples[512];
    CHECK(processor.readSpectrumSamples(samples, 512) == 0);
    CHECK(processor.takeOutputPeak(0) == 0.0f);
    processor.setDisplayActive(false);
    block.clear();
    block.setSample(0, 0, 0.5f);
    processor.processBlock(block, midi);
    CHECK(processor.readSpectrumSamples(samples, 512) == 0);
    CHECK(processor.takeOutputPeak(0) == 0.0f);
}

TEST_CASE("Plugin: the display shows the newest frame, and the knobs once audio stops")
{
    RootnoteProcessor processor;
    prepare(processor, 48000.0, 128);
    processor.setDisplayActive(true);
    juce::AudioBuffer<float> block(2, 128);
    juce::MidiBuffer midi;
    // Many blocks without a read: the editor must see the last one, not an old one.
    for (int note = 30; note <= 50; ++note)
    {
        block.clear();
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, note, 1.0f), 0);
        processor.processBlock(block, midi);
    }
    midi.clear();
    for (int i = 0; i < 40; ++i) // the note settles after a short ramp
        processor.processBlock(block, midi);
    CHECK_NEAR(processor.displayState().boostHz, noteToHz(50.0), 1.0e-6);

    // The host stops calling processBlock: a knob move still shows.
    set(processor, ids::boostNote, 40.0f);
    juce::Thread::sleep(300);
    CHECK_NEAR(processor.displayState().boostHz, noteToHz(40.0), 1.0e-6);
    processor.setDisplayActive(false);
}

TEST_CASE("Plugin: UI snapshots stay whole while audio publishes and the consumer falls behind")
{
    RootnoteProcessor processor;
    prepare(processor, 48000.0, 128);
    processor.setDisplayActive(true);
    std::atomic<bool> done { false };
    std::thread audio([&] {
        juce::AudioBuffer<float> block(2, 128);
        juce::MidiBuffer midi;
        midi.ensureSize(32);
        for (int i = 0; i < 2000; ++i)
        {
            block.clear();
            midi.clear();
            midi.addEvent(juce::MidiMessage::noteOn(1, 36 + i % 24, 1.0f), 0);
            processor.processBlock(block, midi);
        }
        done.store(true, std::memory_order_release);
    });
    bool whole = true;
    do
    {
        const auto state = processor.displayState();
        whole = whole && state.numBoostBands == 8 && state.numCutBands == 0
            && std::abs(state.boostBands[0].hz - state.boostHz) < 1.0e-9
            && std::abs(state.boostBands[1].hz - 2.0 * state.boostHz) < 1.0e-9;
        juce::Thread::sleep(1);
    } while (!done.load(std::memory_order_acquire));
    audio.join();
    CHECK(whole);
    CHECK(processor.displayState().midiNoteOns > 0u);
    processor.setDisplayActive(false);
}

/**
    Renders the editor with a demo state to a PNG: a kick ringing at D1, the
    boost on F#1 played from MIDI, the cut matched to the hit.
*/
int writeSnapshot(const juce::File& file)
{
    RootnoteProcessor processor;
    prepare(processor);
    processor.loadPreset(2);
    set(processor, ids::boostNote, 30.0f);
    set(processor, ids::boostGain, 9.0f);
    set(processor, ids::bands, 8.0f);
    set(processor, ids::key, 6.0f);
    set(processor, ids::scale, static_cast<float>(Scale::minor));

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto* rootnoteEditor = dynamic_cast<RootnoteEditor*>(editor.get());
    if (rootnoteEditor == nullptr)
        return 1;
    editor->setSize(RootnoteEditor::kDefaultWidth, RootnoteEditor::kDefaultHeight);

    playKicks(processor, noteToHz(26.0 - 0.12), 48000.0);
    if (!waitForReading(processor, 3) || !processor.matchCutToHit())
        return 1;

    // One more hit with a MIDI note, drawn frame by frame as a host would.
    Random random(7);
    std::vector<float> signal(48000 / 4, 0.0f);
    KickSpec kick;
    kick.hz = noteToHz(26.0 - 0.12);
    addKick(signal, 0, kick, 48000.0, random);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 30, 1.0f), 0);
    for (size_t position = 0; position + 512 <= signal.size(); position += 512)
    {
        juce::AudioBuffer<float> block(2, 512);
        block.copyFrom(0, 0, signal.data() + position, 512);
        block.copyFrom(1, 0, signal.data() + position, 512);
        processor.processBlock(block, midi);
        midi.clear();
        if (position % 2048 == 0)
            rootnoteEditor->refresh();
    }

    rootnoteEditor->refresh();
    const juce::Image image = editor->createComponentSnapshot(editor->getLocalBounds());
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream stream(file);
    if (!stream.openedOk() || !juce::PNGImageFormat().writeImageToStream(image, stream))
        return 1;
    stream.flush();
    if (stream.getStatus().failed())
        return 1;
    std::printf("wrote %s (%d x %d)\n", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    return 0;
}

/** "--snapshot <file.png> [copy.png]" renders once and optionally copies the PNG. */
int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    if ((argc == 3 || argc == 4) && juce::String(argv[1]) == "--snapshot")
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[2]));
        const int result = writeSnapshot(file);
        if (result == 0 && argc == 4)
        {
            const auto copy = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[3]));
            if (copy.getParentDirectory().createDirectory().failed() || !file.copyFileTo(copy))
                return 1;
            std::printf("copied to %s\n", copy.getFullPathName().toRawUTF8());
        }
        return result;
    }
    return testing::runAll(argc, argv);
}
