// Tests of the real plug-in: the JUCE processor, its parameters and state,
// and the editor, run without a host or an audio device.

#include "TestFramework.h"
#include "TestSignals.h"

#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <limits>
#include <map>
#include <optional>
#include <thread>
#if JUCE_LINUX
 #include <unistd.h>
#endif

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
    processor.setPlayConfigDetails(inputs, outputs, fs, blockSize);
    REQUIRE(processor.getBusesLayout() == layout(inputs, outputs));
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

std::vector<float> drumSequence(double fs, juce::MidiBuffer& midi, int channel)
{
    std::vector<float> signal(static_cast<size_t>(fs), 0.0f);
    Random random(4102);
    for (int hit = 0; hit < 6; ++hit)
    {
        const int start = static_cast<int>(fs * (0.025 + 0.1 * hit));
        const int kind = hit % 3;
        const int note = kind == 0 ? 36 : (kind == 1 ? 38 : 42);
        if (kind == 0)
        {
            KickSpec kick;
            kick.decayTau = 0.04;
            addKick(signal, static_cast<size_t>(start), kick, fs, random);
        }
        else if (kind == 1)
        {
            MembraneSpec snare;
            snare.hz = 180.0;
            snare.decayTau = 0.025;
            snare.noise = 0.8;
            addMembrane(signal, static_cast<size_t>(start), snare, fs, random);
        }
        else
        {
            double previous = 0.0;
            for (int i = 0; i < static_cast<int>(fs * 0.08); ++i)
            {
                const double noise = random.bipolar();
                signal[static_cast<size_t>(start + i)] += static_cast<float>(
                    0.4 * (noise - previous) * std::exp(-static_cast<double>(i) / (fs * 0.01)));
                previous = noise;
            }
        }
        if (channel != 0)
        {
            midi.addEvent(juce::MidiMessage::noteOn(channel, note, 0.9f), start);
            midi.addEvent(juce::MidiMessage::noteOn(channel, note + 12, 0.7f), start); // simultaneous hits
            midi.addEvent(juce::MidiMessage::noteOn(channel, note, static_cast<juce::uint8>(0)), start + 7);
            midi.addEvent(juce::MidiMessage::noteOff(channel, note + 12), start + 31);
            midi.addEvent(juce::MidiMessage::controllerEvent(channel, 4, hit * 20), start + 9);
            midi.addEvent(juce::MidiMessage::pitchWheel(channel, 9000), start + 13);
        }
    }
    for (float& sample : signal)
        sample *= static_cast<float>(dbToGain(12.0));
    signal[static_cast<size_t>(fs * 0.8)] = 1.0f;
    signal[static_cast<size_t>(fs * 0.8) + 1] = -1.0f;
    std::fill(signal.begin() + static_cast<int>(fs * 0.9), signal.end(), static_cast<float>(dbToGain(12.0)));
    if (channel != 0)
    {
        midi.addEvent(juce::MidiMessage::allNotesOff(channel), static_cast<int>(fs * 0.7));
        midi.addEvent(juce::MidiMessage::allSoundOff(channel), static_cast<int>(fs * 0.71));
        midi.addEvent(juce::MidiMessage::createSysExMessage("ignored", 7), 3);
    }
    return signal;
}

void stressSettings(RootnoteProcessor& processor, int setting)
{
    const int presets = RootnoteProcessor::numPresets();
    processor.setCurrentProgram(setting < presets ? setting : 0);
    if (setting < presets)
        return;

    const int extreme = setting - presets;
    set(processor, ids::series, static_cast<float>(extreme % 3));
    set(processor, ids::qMode, static_cast<float>(extreme / 3));
    set(processor, ids::boostNote, extreme % 2 == 0 ? 12.0f : 96.0f);
    set(processor, ids::boostFine, extreme % 2 == 0 ? -50.0f : 50.0f);
    set(processor, ids::boostGain, 24.0f);
    set(processor, ids::boostQ, 200.0f);
    set(processor, ids::bands, 32.0f);
    set(processor, ids::range, 6.0f);
    set(processor, ids::tilt, extreme % 2 == 0 ? 6.0f : -12.0f);
    set(processor, ids::cutOn, 1.0f);
    set(processor, ids::cutNote, extreme % 2 == 0 ? 96.0f : 12.0f);
    set(processor, ids::cutDepth, -30.0f);
    set(processor, ids::cutQ, extreme % 2 == 0 ? 60.0f : 0.5f);
    set(processor, ids::cutBands, 16.0f);
    set(processor, ids::cutOvertones, 100.0f);
    set(processor, ids::ring, 100.0f);
    set(processor, ids::ringDecay, 4000.0f);
    set(processor, ids::ringDrop, 12.0f);
    set(processor, ids::output, 12.0f);
    set(processor, ids::midiMode, extreme % 2 == 0 ? 1.0f : 2.0f);
    set(processor, ids::noteOff, static_cast<float>(extreme % 2));
}

void checkDrumRender(RootnoteProcessor& processor, double fs, int inputs, int outputs,
                     const std::vector<float>& signal, const juce::MidiBuffer& events,
                     bool checkTail, const std::string& name)
{
    // The reported tail is a T60 bound. Two tails allow -120 dB of decay,
    // leaving headroom for +12 dBFS excitation before the -90 dBFS RMS check.
    const int measureFrom = static_cast<int>(signal.size())
        + (checkTail ? static_cast<int>(std::ceil(2.0 * processor.getTailLengthSeconds() * fs)) : 0);
    const int total = measureFrom + (checkTail ? static_cast<int>(fs * 0.5) : 0);
    const int sizes[] = { 1, 17, 64, 512, 1024, 4096, 8193 };
    juce::AudioBuffer<float> block(outputs, 8193);
    juce::MidiBuffer midi;
    bool finite = true, initialSilence = true;
    float peak = 0.0f;
    double energy = 0.0;
    int measured = 0;
    int cycle = 0;
    for (int position = 0; position < total;)
    {
        const int n = std::min(sizes[cycle++ % static_cast<int>(std::size(sizes))], total - position);
        block.setSize(outputs, n, false, false, true);
        block.clear();
        for (int ch = 0; ch < outputs; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const auto source = static_cast<size_t>(position + i);
                const float sample = source < signal.size() ? signal[source] : 0.0f;
                block.setSample(ch, i, ch >= inputs ? 0.0f : (ch == 0 ? sample : -0.7f * sample));
            }
        midi.clear();
        midi.addEvents(events, position, n, -position);
        processor.processBlock(block, midi);
        finite = finite && allFinite(block);
        peak = std::max(peak, block.getMagnitude(0, n));
        for (int ch = 0; ch < outputs; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const double sample = static_cast<double>(block.getSample(ch, i));
                if (position + i < static_cast<int>(fs * 0.025))
                    initialSilence = initialSilence && sample == 0.0;
                if (checkTail && position + i >= measureFrom)
                {
                    energy += sample * sample;
                    ++measured;
                }
            }
        position += n;
    }
    CHECK_MSG(finite, name);
    CHECK_MSG(peak <= static_cast<float>(Limiter::kCeiling) + 1.0e-6f, name);
    CHECK_MSG(initialSilence, name);
    if (checkTail)
        CHECK_MSG(measured > 0 && energy / static_cast<double>(measured) < 1.0e-9,
                  name + " tail RMS " + std::to_string(gainToDb(std::sqrt(energy / static_cast<double>(measured)))) + " dBFS");

    processor.reset();
    block.clear();
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, 1.0f), 0);
    midi.addEvent(juce::MidiMessage::noteOn(10, 42, 1.0f), 0);
    processor.processBlock(block, midi);
    CHECK_MSG(allFinite(block) && block.getMagnitude(0, block.getNumSamples()) == 0.0f, name + " reset silence");
}
} // namespace

TEST_CASE("Plugin: identity")
{
    RootnoteProcessor processor;
    CHECK(processor.getName() == "Rootnote");
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_AUMainType)) == "'aufx'");
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
        { ids::midiMode, 0, 2, 0 }, { ids::glide, 0, 500, 0 }, { ids::noteOff, 0, 1, 0 },
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

        float previous = range.start;
        for (int step = 0; step <= 100; ++step)
        {
            parameter->setValueNotifyingHost(static_cast<float>(step) / 100.0f);
            const float physical = get(processor, expected.id);
            CHECK_MSG(std::isfinite(physical) && physical >= range.start && physical <= range.end
                          && physical >= previous, expected.id);
            previous = physical;
        }
        parameter->setValueNotifyingHost(parameter->getDefaultValue());
        CHECK_NEAR_MSG(get(processor, expected.id), expected.value, tolerance, expected.id);
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

    // A host can leave a switch or a step between legal values; restoring settles it on the saved one.
    RootnoteProcessor between;
    for (const auto& [id, raw] : { std::pair { ids::limiter, 0.86f }, { ids::keyLock, 0.107f }, { ids::bands, 0.5f } })
        between.getApvts().getParameter(id)->setValueNotifyingHost(raw);
    between.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (int i = 0; i < source.getParameters().size(); ++i)
        CHECK_NEAR_MSG(between.getParameters()[i]->getValue(), source.getParameters()[i]->getValue(), 1.0e-6,
                       ("between steps: " + source.getParameters()[i]->getName(64)).toStdString());

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
        set(processor, ids::midiMode, 1.0f);
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

TEST_CASE("Plugin: drum inserts stay bounded and decay across presets and host configurations")
{
    const auto render = [](int setting, double fs, int inputs, int channel, bool checkTail) {
        juce::MidiBuffer events;
        const auto signal = drumSequence(fs, events, channel);
        RootnoteProcessor processor;
        prepare(processor, fs, 512, inputs, 2);
        stressSettings(processor, setting);
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        const std::string name = "setting " + std::to_string(setting) + ", MIDI " + std::to_string(channel)
            + ", " + std::to_string(inputs) + ">2, " + std::to_string(fs);
        checkDrumRender(processor, fs, inputs, 2, signal, events, checkTail, name);
    };

    const int presets = RootnoteProcessor::numPresets();
    for (int preset = 0; preset < presets; ++preset)
        render(preset, 48000.0, 2, 0, false);
    for (int extreme = 0; extreme < 6; ++extreme)
        render(presets + extreme, 48000.0, 1 + extreme % 2, 1, true);
    for (double fs : { 44100.0, 96000.0 })
        render(presets, fs, 2, 1, false);
}

TEST_CASE("Plugin: silence stays finite and zero after preparing at each sample rate")
{
    RootnoteProcessor processor;
    for (double fs : { 44100.0, 48000.0, 96000.0, 44100.0 })
    {
        processor.setPlayConfigDetails(2, 2, fs, 64);
        juce::AudioBuffer<float> block(2, 1024);
        juce::MidiBuffer midi;
        processor.prepareToPlay(fs, 64);
        block.clear();
        processor.processBlock(block, midi);
        CHECK(allFinite(block) && block.getMagnitude(0, 1024) == 0.0f);
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

TEST_CASE("Plugin: unprepared mono input is copied to both outputs")
{
    RootnoteProcessor processor;
    REQUIRE(processor.setBusesLayout(layout(1, 2)));
    juce::AudioBuffer<float> block(2, 17);
    block.clear();
    for (int i = 0; i < 17; ++i)
        block.setSample(0, i, 0.01f * static_cast<float>(i));
    juce::MidiBuffer midi;
    processor.processBlock(block, midi);
    for (int i = 0; i < 17; ++i)
    {
        CHECK(block.getSample(0, i) == 0.01f * static_cast<float>(i));
        CHECK(block.getSample(1, i) == block.getSample(0, i));
    }
    REQUIRE(!processor.setBusesLayout(layout(0, 2)));
    REQUIRE(!processor.setBusesLayout(layout(2, 1)));
    REQUIRE(!processor.setBusesLayout(layout(2, 6)));
}

TEST_CASE("Plugin: non-finite upstream audio cannot poison the following silence")
{
    for (float invalid : { std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() })
        for (bool limit : { false, true })
        {
            RootnoteProcessor processor;
            prepare(processor, 48000.0, 64);
            set(processor, ids::limiter, limit ? 1.0f : 0.0f);
            set(processor, ids::ring, 100.0f);
            juce::AudioBuffer<float> block(2, 64);
            block.clear();
            block.setSample(0, 0, 0.25f);
            block.setSample(1, 17, invalid);
            juce::MidiBuffer midi;
            processor.processBlock(block, midi);
            CHECK(allFinite(block));
            CHECK(block.getSample(0, 0) == 0.25f); // recover with the available dry chunk
            CHECK(block.getSample(1, 17) == 0.0f);
            block.clear();
            processor.processBlock(block, midi);
            CHECK(allFinite(block));
            CHECK(block.getMagnitude(0, 64) == 0.0f);
        }
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
    CHECK_NEAR(state.boostHz, noteToHz(36.0), 1.0e-3);
    CHECK_NEAR(state.cutHz, noteToHz(36.0), 1.0e-3);
    CHECK(!state.boostFromMidi && !state.cutFromMidi && state.midiNoteOns == 0u);

    set(processor, ids::midiMode, 1.0f);
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
    set(processor, ids::midiMode, 1.0f);
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
    CHECK(holds(processor, ids::midiMode, 1.0f));
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
        set(processor, ids::midiMode, 1.0f);
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

        for (const auto size : { juce::Point<int>(720, 405),
                                 juce::Point<int>(RootnoteEditor::kDefaultWidth, RootnoteEditor::kDefaultHeight),
                                 juce::Point<int>(1920, 1080), juce::Point<int>(1200, 675) })
        {
            editor->setSize(size.x, size.y);
            const juce::Image snapshot = editor->createComponentSnapshot(editor->getLocalBounds());
            CHECK(snapshot.getWidth() == size.x && snapshot.getHeight() == size.y);
        }
    }
    CHECK(processor.storedEditorSize() == juce::Point<int>(1200, 675));

    // The size comes back with the state, and a second editor can follow the first.
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    RootnoteProcessor other;
    other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    std::unique_ptr<juce::AudioProcessorEditor> editor(other.createEditor());
    CHECK(editor->getWidth() == 1200 && editor->getHeight() == 675);
}

TEST_CASE("Plugin: the editor can reopen and paint before preparing audio")
{
    RootnoteProcessor processor;
    CHECK(processor.hasEditor());
    for (int opened = 0; opened < 2; ++opened)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        REQUIRE(editor != nullptr);
        CHECK(editor->getWidth() == 960 && editor->getHeight() == 540);
        const auto snapshot = editor->createComponentSnapshot(editor->getLocalBounds());
        CHECK(snapshot.isValid() && snapshot.getWidth() == 960 && snapshot.getHeight() == 540);
        bool uniform = true;
        const auto firstPixel = snapshot.getPixelAt(0, 0);
        for (int y = 0; y < snapshot.getHeight() && uniform; ++y)
            for (int x = 0; x < snapshot.getWidth() && uniform; ++x)
                uniform = snapshot.getPixelAt(x, y) == firstPixel;
        CHECK(!uniform);
    }
}

/** A control of the editor by its accessible title ("d", "Gain"), visible or not. */
template <typename Type>
Type* findControl(juce::Component& parent, const juce::String& title)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* match = dynamic_cast<Type*>(child); match != nullptr && child->getTitle() == title)
            return match;
        if (auto* found = findControl<Type>(*child, title))
            return found;
    }
    return nullptr;
}

TEST_CASE("Plugin: the faceplate keys and encoders write the parameters")
{
    using rootnote::ui::Encoder;
    using rootnote::ui::Key;
    RootnoteProcessor processor;
    prepare(processor);
    set(processor, ids::boostNote, 36.0f); // C2: the pitch keys start on octave 2
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);

    // Click a pitch key: the boost note; Alt-click: the cut note, switched on.
    auto* fSharp = findControl<Key>(faceplate, juce::String::fromUTF8("f\xe2\x99\xaf"));
    REQUIRE(fSharp != nullptr);
    fSharp->onPress(juce::ModifierKeys());
    CHECK(holds(processor, ids::boostNote, 42.0f));
    findControl<Key>(faceplate, "d")->onPress(juce::ModifierKeys(juce::ModifierKeys::altModifier));
    CHECK(holds(processor, ids::cutNote, 38.0f));
    CHECK(holds(processor, ids::cutOn, 1.0f));

    // The boost page's encoders are bound to their parameters.
    auto* gain = findControl<Encoder>(faceplate, "Gain");
    REQUIRE(gain != nullptr);
    CHECK(gain->isVisible());
    gain->setValue(15.0, juce::sendNotificationSync);
    CHECK(holds(processor, ids::boostGain, 15.0f));

    // The cut page rebinds the same encoders; Fine sets the cents of the cut note.
    faceplate.selectPage(1);
    auto* depth = findControl<Encoder>(faceplate, "Depth");
    REQUIRE(depth != nullptr);
    CHECK(depth == gain); // the first encoder, now Depth
    CHECK_NEAR(depth->getValue(), -12.0, 1.0e-4);
    depth->setValue(-20.0, juce::sendNotificationSync);
    CHECK(holds(processor, ids::cutDepth, -20.0f));
    CHECK(holds(processor, ids::boostGain, 15.0f));

    auto* fine = findControl<Encoder>(faceplate, "Fine");
    REQUIRE(fine != nullptr);
    fine->onDragStart();
    fine->setValue(25.0, juce::sendNotificationSync);
    fine->onDragEnd();
    CHECK(holds(processor, ids::cutNote, 38.25f));
    set(processor, ids::cutNote, 37.9f);
    CHECK_NEAR(fine->getValue(), -10.0, 0.05);

    // The cut note has no encoder: its value is typed on the display or set by the keys.
    CHECK(findControl<Encoder>(faceplate, "Note") == nullptr);
    int shown = 0;
    for (auto* child : editor->getChildComponent(0)->getChildren())
        shown += dynamic_cast<Encoder*>(child) != nullptr && child->isVisible() ? 1 : 0;
    CHECK(shown == 6);

    // Pages with fewer controls hide the rest.
    faceplate.selectPage(2);
    int visible = 0;
    for (auto* child : editor->getChildComponent(0)->getChildren())
        visible += dynamic_cast<Encoder*>(child) != nullptr && child->isVisible() ? 1 : 0;
    CHECK(visible == 3);
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

    // Both slots and the active one come back with the state, in a new instance too.
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    RootnoteProcessor other;
    other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(other.abSlot() == 1 && other.currentPreset() == 5 && holds(other, ids::mix, 40.0f));
    other.selectAbSlot(0);
    CHECK(other.currentPreset() == 2 && holds(other, ids::mix, 100.0f) && holds(other, ids::boostGain, 3.0f));
    other.selectAbSlot(1);
    CHECK(holds(other, ids::mix, 40.0f) && holds(other, ids::series, 1.0f));
}

TEST_CASE("Plugin: the display state has the live notes and bands, with or without audio")
{
    RootnoteProcessor processor;
    set(processor, ids::midiMode, 1.0f);
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
    set(processor, ids::midiMode, 1.0f);
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
    set(processor, ids::midiMode, 1.0f);
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

// ---- Real input: events through a window's peer, as the operating system delivers them ----

#if JUCE_LINUX
#include <dlfcn.h>

/**
    A bare X server (xvfb-run, no window manager) has never seen WM_PROTOCOLS
    and the like, and JUCE's window code fails on the missing atoms; nor does
    anything hand the keyboard focus to a new window. This defines the atoms
    before JUCE opens its display, on a connection that stays open so the
    server does not reset, and focuses windows for the input tests.
*/
namespace x11
{
struct Library
{
    using OpenDisplay = void* (*)(const char*);
    using InternAtom = unsigned long (*)(void*, const char*, int);
    using SetInputFocus = int (*)(void*, unsigned long, int, unsigned long);
    using Sync = int (*)(void*, int);
    using GetInputFocus = int (*)(void*, unsigned long*, int*);
    using ErrorHandler = int (*)(void*, void*);
    using SetErrorHandler = ErrorHandler (*)(ErrorHandler);

    Library()
    {
        if (void* lib = dlopen("libX11.so.6", RTLD_LAZY))
        {
            const auto open = reinterpret_cast<OpenDisplay>(dlsym(lib, "XOpenDisplay"));
            internAtom = reinterpret_cast<InternAtom>(dlsym(lib, "XInternAtom"));
            setInputFocus = reinterpret_cast<SetInputFocus>(dlsym(lib, "XSetInputFocus"));
            sync = reinterpret_cast<Sync>(dlsym(lib, "XSync"));
            getInputFocus = reinterpret_cast<GetInputFocus>(dlsym(lib, "XGetInputFocus"));
            setErrorHandler = reinterpret_cast<SetErrorHandler>(dlsym(lib, "XSetErrorHandler"));
            if (open != nullptr && internAtom != nullptr && setInputFocus != nullptr && sync != nullptr
                && getInputFocus != nullptr && setErrorHandler != nullptr)
                display = open(nullptr);
        }
    }

    void* display = nullptr;
    InternAtom internAtom = nullptr;
    SetInputFocus setInputFocus = nullptr;
    Sync sync = nullptr;
    GetInputFocus getInputFocus = nullptr;
    SetErrorHandler setErrorHandler = nullptr;
};

Library& library()
{
    static Library instance;
    return instance;
}

void defineWindowManagerAtoms()
{
    auto& x = library();
    if (x.display == nullptr)
        return;
    for (const char* name : { "WM_PROTOCOLS", "WM_TAKE_FOCUS", "WM_DELETE_WINDOW", "WM_STATE", "WM_CHANGE_STATE",
                              "_NET_WM_PING", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_FULLSCREEN",
                              "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_WINDOW_TYPE",
                              "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_COMBO", "_NET_WM_ALLOWED_ACTIONS",
                              "_NET_WM_MOVERESIZE", "_NET_FRAME_EXTENTS", "_NET_WORKAREA", "_MOTIF_WM_HINTS" })
        x.internAtom(x.display, name, 0);
    x.sync(x.display, 0);
}

/** True once the window has the focus; fails harmlessly while the window is not mapped yet. */
bool focus(void* window)
{
    auto& x = library();
    if (x.display == nullptr)
        return true;
    const auto handle = reinterpret_cast<unsigned long>(window);
    const auto previous = x.setErrorHandler([](void*, void*) { return 0; });
    x.setInputFocus(x.display, handle, 2 /* RevertToParent */, 0 /* CurrentTime */);
    x.sync(x.display, 0);
    x.setErrorHandler(previous);
    unsigned long focused = 0;
    int revert = 0;
    x.getInputFocus(x.display, &focused, &revert);
    return focused == handle;
}
} // namespace x11
#endif


#if JUCE_LINUX || JUCE_WINDOWS
namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue(bool returnIfNoPendingMessages);
}
#endif

namespace
{
/** Runs the message loop until nothing is pending: async callbacks, menu results. */
void dispatchMessages()
{
#if JUCE_LINUX || JUCE_WINDOWS
    for (int i = 0; i < 1000 && juce::detail::dispatchNextMessageOnSystemQueue(true); ++i) {}
#endif
}

/** Puts a component on the desktop and sends it mouse and key events through its peer. */
class Desk
{
public:
    explicit Desk(juce::Component& component) : root(component)
    {
        root.addToDesktop(0);
        root.setVisible(true);
        dispatchMessages();
#if JUCE_LINUX
        // Until JUCE itself has seen the FocusIn: before that it thinks another application is active,
        // and a popup menu dismisses itself on the very mouse-up that opens it.
        for (int attempt = 0; attempt < 200
                              && !(x11::focus(root.getPeer()->getNativeHandle()) && juce::Process::isForegroundProcess());
             ++attempt)
        {
            juce::Thread::sleep(5);
            dispatchMessages();
        }
#endif
        mouse(root, {}, {}); // no button down, whatever an earlier test left behind
    }

    /** Left button, or the right one for popupMenuClickModifier. */
    void down(juce::Component& target, juce::Point<float> at, juce::ModifierKeys mods = {})
    {
        const int button = mods.isPopupMenu() ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier;
        mouse(target, at, mods.withoutMouseButtons().withFlags(button));
    }
    void drag(juce::Component& target, juce::Point<float> at, juce::ModifierKeys mods = {})
    {
        mouse(target, at, mods.withFlags(juce::ModifierKeys::leftButtonModifier));
    }
    void up(juce::Component& target, juce::Point<float> at, juce::ModifierKeys mods = {})
    {
        mouse(target, at, mods.withoutMouseButtons());
    }

    /** A click in the middle of the target, or at a point; far enough from the last one not to be a double-click. */
    void click(juce::Component& target, juce::ModifierKeys mods = {}) { click(target, target.getLocalBounds().getCentre().toFloat(), mods); }
    void click(juce::Component& target, juce::Point<float> at, juce::ModifierKeys mods = {})
    {
        time += 1000;
        down(target, at, mods);
        up(target, at, mods);
        juce::ModifierKeys::currentModifiers = {};
    }

    void key(const juce::KeyPress& key)
    {
        juce::ModifierKeys::currentModifiers = key.getModifiers();
        root.getPeer()->handleKeyPress(key);
        juce::ModifierKeys::currentModifiers = {};
        dispatchMessages(); // a text field answers Return and Escape asynchronously
    }

private:
    void mouse(juce::Component& target, juce::Point<float> at, juce::ModifierKeys mods)
    {
        time += 20;
        juce::ModifierKeys::currentModifiers = mods; // the mouse source reads Shift, Alt and Cmd from here
        root.getPeer()->handleMouseEvent(juce::MouseInputSource::InputSourceType::mouse, root.getLocalPoint(&target, at),
                                         mods, juce::MouseInputSource::defaultPressure,
                                         juce::MouseInputSource::defaultOrientation, time);
    }

    juce::Component& root;
    juce::int64 time = juce::Time::currentTimeMillis();
};

/** Counts the begin and end of each parameter's gestures. */
class GestureCounter : private juce::AudioProcessorParameter::Listener
{
public:
    explicit GestureCounter(RootnoteProcessor& p) : processor(p)
    {
        for (auto* parameter : processor.getParameters())
            parameter->addListener(this);
    }
    ~GestureCounter() override
    {
        for (auto* parameter : processor.getParameters())
            parameter->removeListener(this);
    }

    int begins(const char* id) const { return count(id, true); }
    int ends(const char* id) const { return count(id, false); }
    bool balanced() const { return begun == ended; }

private:
    int count(const char* id, bool start) const
    {
        const int index = processor.getApvts().getParameter(id)->getParameterIndex();
        const auto& counts = start ? begun : ended;
        const auto found = counts.find(index);
        return found == counts.end() ? 0 : found->second;
    }
    void parameterValueChanged(int, float) override {}
    void parameterGestureChanged(int index, bool starting) override { ++(starting ? begun : ended)[index]; }

    RootnoteProcessor& processor;
    std::map<int, int> begun, ended;
};

juce::Component* findNamed(juce::Component& parent, const juce::String& name)
{
    for (auto* child : parent.getChildren())
    {
        if (child->getName() == name)
            return child;
        if (auto* found = findNamed(*child, name))
            return found;
    }
    return nullptr;
}

template <typename Type>
Type* findFirst(juce::Component& parent)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* match = dynamic_cast<Type*>(child))
            return match;
        if (auto* found = findFirst<Type>(*child))
            return found;
    }
    return nullptr;
}

/** The middle of the n-th legend value on the screen. */
juce::Point<float> legendPoint(int slot)
{
    return { 16.0f + static_cast<float>(slot) * (528.0f - 62.0f) / 7.0f + 14.0f, 232.0f };
}
} // namespace

TEST_CASE("Plugin: a menu choice is applied, but not once the editor has closed")
{
#if JUCE_LINUX || JUCE_WINDOWS
    using rootnote::ui::PresetDisplay;
    RootnoteProcessor processor;
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    std::optional<Desk> desk(std::in_place, *editor);
    auto* display = findFirst<PresetDisplay>(*editor);
    auto* settings = findNamed(*editor, "Settings");
    REQUIRE(display != nullptr && settings != nullptr);

    const auto choose = [&](juce::Component& opener, juce::Point<float> at, int item, bool closeEditorFirst) {
        desk->click(opener, at);
        auto* menu = juce::ModalComponentManager::getInstance()->getModalComponent(0);
        REQUIRE(menu != nullptr);
        menu->exitModalState(item); // as a click on the item: the result is delivered asynchronously
        if (closeEditorFirst)
            editor.reset();
        dispatchMessages();
    };

    choose(*display, { 60.0f, 13.0f }, 5, false); // item 5: preset 4
    CHECK(processor.currentPreset() == 4);
    choose(*settings, { 13.0f, 13.0f }, 3, false); // UI size 125 %
    CHECK(editor->getWidth() == 1200 && editor->getHeight() == 675);
    choose(*settings, { 13.0f, 13.0f }, 101, false); // Constant width
    CHECK(holds(processor, ids::qMode, 1.0f));

    choose(*display, { 60.0f, 13.0f }, 2, true);
    CHECK(processor.currentPreset() == 4);
    editor.reset(processor.createEditor());
    desk.emplace(*editor);
    settings = findNamed(*editor, "Settings");
    choose(*settings, { 13.0f, 13.0f }, 100, true); // Constant Q, after the editor closed
    CHECK(holds(processor, ids::qMode, 1.0f));
#endif
}

TEST_CASE("Plugin: preset and editor size never touch the state tree the host restores")
{
    RootnoteProcessor processor;
    processor.loadPreset(3);
    processor.storeEditorSize(1200, 675);
    juce::MemoryBlock state;
    processor.getStateInformation(state);

    // The tree keeps parameters only; preset and size are added to the saved copy.
    CHECK(!processor.getApvts().state.hasProperty("preset"));
    CHECK(!processor.getApvts().state.hasProperty("editorWidth"));

    // A host restoring on another thread while the editor reads every frame.
    RootnoteProcessor other;
    std::atomic<bool> done { false };
    std::thread host([&] {
        for (int i = 0; i < 300; ++i)
            other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        done = true;
    });
    int reads = 0;
    while (!done)
    {
        const int preset = other.currentPreset();
        const auto size = other.storedEditorSize();
        CHECK(preset == 0 || preset == 3);
        CHECK(size.x == 0 || size.x == 1200);
        other.storeEditorSize(1200, 675);
        ++reads;
    }
    host.join();
    CHECK(reads > 0);
    CHECK(other.currentPreset() == 3 && other.storedEditorSize() == juce::Point<int>(1200, 675));
    CHECK(!other.getApvts().state.hasProperty("preset"));

    // States saved by 0.4.0 keep the metadata in the tree; they read the same.
    juce::ValueTree old = processor.getApvts().copyState();
    old.setProperty("preset", 6, nullptr);
    old.setProperty("editorWidth", 800, nullptr);
    old.setProperty("editorHeight", 450, nullptr);
    juce::MemoryBlock oldState;
    juce::AudioProcessor::copyXmlToBinary(*old.createXml(), oldState);
    other.setStateInformation(oldState.getData(), static_cast<int>(oldState.getSize()));
    CHECK(other.currentPreset() == 6 && other.storedEditorSize() == juce::Point<int>(800, 450));
}

TEST_CASE("Plugin: the factory presets are the host's programs")
{
    RootnoteProcessor processor;
    REQUIRE(processor.getNumPrograms() == RootnoteProcessor::numPresets());
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        CHECK(processor.getProgramName(i) == RootnoteProcessor::presetName(i));
    CHECK(processor.getCurrentProgram() == 0);

    set(processor, ids::boostNote, 30.0f);
    GestureCounter gestures(processor);
    processor.setCurrentProgram(3); // Snare Core
    CHECK(processor.getCurrentProgram() == 3 && processor.currentPreset() == 3);
    CHECK(holds(processor, ids::series, 2.0f) && holds(processor, ids::boostQ, 18.0f) && holds(processor, ids::cutDepth, -10.0f));
    CHECK(holds(processor, ids::boostNote, 30.0f)); // the song stays
    CHECK(gestures.begins(ids::boostQ) == 0);       // a host change, not a gesture

    // The same values as loadPreset(), which the editor uses.
    RootnoteProcessor other;
    other.loadPreset(3);
    for (int i = 0; i < processor.getParameters().size(); ++i)
        if (processor.getParameters()[i]->getName(64) != "Boost Note")
            CHECK_NEAR(processor.getParameters()[i]->getValue(), other.getParameters()[i]->getValue(), 1.0e-6);

    // From the audio thread, as some hosts do: no editor-only work, just the values.
    std::thread audio([&] { processor.setCurrentProgram(8); });
    audio.join();
    CHECK(processor.getCurrentProgram() == 8 && holds(processor, ids::bands, 24.0f));

    processor.setCurrentProgram(42);
    processor.setCurrentProgram(-1);
    CHECK(processor.getCurrentProgram() == 8);

    // Selecting the current program again reloads it, as a host's preset menu means.
    set(processor, ids::bands, 5.0f);
    processor.setCurrentProgram(8);
    CHECK(holds(processor, ids::bands, 24.0f));
}

TEST_CASE("Plugin: a stored size below the editor's limits opens at the smallest size")
{
    RootnoteProcessor processor;
    processor.storeEditorSize(400, 320); // valid for an earlier editor
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    CHECK(editor->getWidth() == 720 && editor->getHeight() == 405);
    editor.reset();

    processor.storeEditorSize(5000, 2000);
    editor.reset(processor.createEditor());
    CHECK(editor->getWidth() == 1920 && editor->getHeight() == 1080);
}

TEST_CASE("Plugin: clicking the knob note takes the bank back from MIDI, even unchanged")
{
    using rootnote::ui::Key;
    RootnoteProcessor processor;
    set(processor, ids::midiMode, 1.0f);
    prepare(processor);
    set(processor, ids::boostNote, 36.0f); // C2
    set(processor, ids::cutNote, 38.0f);
    set(processor, ids::cutOn, 1.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    Desk desk(*editor);

    juce::AudioBuffer<float> block(2, 512);
    block.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
    midi.addEvent(juce::MidiMessage::noteOn(2, 62, 1.0f), 0);
    processor.processBlock(block, midi);
    midi.clear();
    const auto run = [&] {
        for (int i = 0; i < 10; ++i)
            processor.processBlock(block, midi);
        return processor.displayState();
    };
    auto state = run();
    REQUIRE(state.boostFromMidi && state.cutFromMidi);

    // C on octave 2 is already the boost note: the click still takes it back.
    desk.click(*findControl<Key>(*editor, "c"));
    state = run();
    CHECK(holds(processor, ids::boostNote, 36.0f));
    CHECK(!state.boostFromMidi && state.cutFromMidi);
    CHECK_NEAR(hzToNote(state.boostHz), 36.0, 0.01);

    // Alt-click D: the cut note, also unchanged.
    desk.click(*findControl<Key>(*editor, "d"), juce::ModifierKeys(juce::ModifierKeys::altModifier));
    state = run();
    CHECK(!state.cutFromMidi);
    CHECK_NEAR(hzToNote(state.cutHz), 38.0, 0.01);

    // A new note takes over again.
    midi.addEvent(juce::MidiMessage::noteOn(1, 48, 1.0f), 0);
    processor.processBlock(block, midi);
    midi.clear();
    CHECK(run().boostFromMidi);
}

TEST_CASE("Plugin: remapping or closing the editor mid-drag leaves every gesture balanced")
{
    using rootnote::ui::Encoder;
    RootnoteProcessor processor;
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);
    Desk desk(*editor);
    GestureCounter gestures(processor);

    auto* gain = findControl<Encoder>(*editor, "Gain");
    REQUIRE(gain != nullptr);
    const float depthBefore = get(processor, ids::cutDepth);
    desk.down(*gain, { 40.0f, 30.0f });
    desk.drag(*gain, { 40.0f, 20.0f });
    CHECK(gestures.begins(ids::boostGain) == 1);
    CHECK(get(processor, ids::boostGain) > 9.0f);

    // Selecting the page already shown changes nothing: the drag goes on.
    faceplate.selectPage(0);
    const float dragged = get(processor, ids::boostGain);
    desk.drag(*gain, { 40.0f, 10.0f });
    CHECK(get(processor, ids::boostGain) > dragged);
    CHECK(gestures.ends(ids::boostGain) == 0);

    // Gain becomes Depth under the pointer; the rest of the drag moves nothing.
    faceplate.selectPage(1);
    CHECK(gestures.ends(ids::boostGain) == 1);
    desk.drag(*gain, { 40.0f, 0.0f });
    desk.up(*gain, { 40.0f, 0.0f });
    CHECK(gestures.begins(ids::cutDepth) == 0 && gestures.ends(ids::cutDepth) == 0);
    CHECK(get(processor, ids::cutDepth) == depthBefore);

    // The next drag works as usual.
    desk.down(*gain, { 40.0f, 30.0f });
    desk.drag(*gain, { 40.0f, 40.0f });
    desk.up(*gain, { 40.0f, 40.0f });
    CHECK(gestures.begins(ids::cutDepth) == 1 && gestures.ends(ids::cutDepth) == 1);
    CHECK(get(processor, ids::cutDepth) < depthBefore);

    // Closing the editor mid-drag ends the gesture.
    desk.down(*gain, { 40.0f, 30.0f });
    desk.drag(*gain, { 40.0f, 10.0f });
    CHECK(gestures.begins(ids::cutDepth) == 2);
    editor.reset();
    CHECK(gestures.ends(ids::cutDepth) == 2);
    CHECK(gestures.balanced());
}

TEST_CASE("Plugin: encoders take typed values and arrow keys, each change one gesture")
{
    using rootnote::ui::Encoder;
    using rootnote::ui::Screen;
    RootnoteProcessor processor;
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);
    Desk desk(*editor);
    GestureCounter gestures(processor);
    auto* screen = findFirst<Screen>(*editor);
    REQUIRE(screen != nullptr);
    const auto entry = [&] { return findFirst<juce::TextEditor>(*screen); };

    // The encoders are in the focus order.
    auto* focus = findControl<Encoder>(*editor, "Focus");
    REQUIRE(focus != nullptr);
    const auto order = editor->createKeyboardFocusTraverser()->getAllComponents(editor.get());
    CHECK(std::find(order.begin(), order.end(), focus) != order.end());

    // Alt-click opens the value's field on the display; Return applies the parameter's own parser.
    set(processor, ids::focusLink, 0.0f);
    desk.click(*focus, juce::ModifierKeys(juce::ModifierKeys::altModifier));
    CHECK(screen->isShowingEntry());
    CHECK(entry()->getText() == "65.0 Hz");
    CHECK(entry()->hasKeyboardFocus(false));
    CHECK(entry()->getBounds().contains(legendPoint(3).toInt()));
    entry()->setText("1.2 kHz");
    desk.key(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(!screen->isShowingEntry());
    CHECK_NEAR(get(processor, ids::focus), 1200.0, 0.5);
    CHECK(gestures.begins(ids::focus) == 1 && gestures.ends(ids::focus) == 1);

    // Escape keeps the value and gives the keyboard back to the encoder.
    desk.click(*focus, juce::ModifierKeys(juce::ModifierKeys::popupMenuClickModifier));
    REQUIRE(screen->isShowingEntry());
    entry()->setText("80 Hz");
    desk.key(juce::KeyPress(juce::KeyPress::escapeKey));
    CHECK(!screen->isShowingEntry());
    CHECK_NEAR(get(processor, ids::focus), 1200.0, 0.5);

    // Arrow keys on the focused encoder: a step each, Shift finer.
    CHECK(focus->hasKeyboardFocus(false));
    const float before = get(processor, ids::focus);
    desk.key(juce::KeyPress(juce::KeyPress::downKey));
    const float down = get(processor, ids::focus);
    CHECK(down < before);
    desk.key(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0));
    CHECK(get(processor, ids::focus) > down && get(processor, ids::focus) < before);
    CHECK(gestures.begins(ids::focus) == 3 && gestures.ends(ids::focus) == 3);

    // Return on the focused encoder opens its field too.
    desk.key(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(screen->isShowingEntry());
    desk.key(juce::KeyPress(juce::KeyPress::escapeKey));

    // A stepped parameter moves a whole step per key.
    auto* bands = findControl<Encoder>(*editor, "Bands");
    desk.click(*bands);
    desk.key(juce::KeyPress(juce::KeyPress::upKey));
    CHECK(holds(processor, ids::bands, 9.0f));

    // A click on a legend value opens its field: an exact frequency for the cut note, which has no encoder.
    faceplate.selectPage(1);
    desk.click(*screen, legendPoint(4));
    REQUIRE(screen->isShowingEntry());
    entry()->setText("55 Hz");
    desk.key(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK_NEAR(get(processor, ids::cutNote), 33.0, 0.006);

    // Cents of the cut note, from its encoder.
    auto* fine = findControl<Encoder>(*editor, "Fine");
    desk.click(*fine, juce::ModifierKeys(juce::ModifierKeys::commandModifier));
    REQUIRE(screen->isShowingEntry());
    CHECK(entry()->getBounds().contains(legendPoint(5).toInt()));
    entry()->setText(juce::String::fromUTF8("\xe2\x88\x92" "20 ct"));
    desk.key(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK_NEAR(get(processor, ids::cutNote), 32.8, 0.002);

    // Clicking the bare plate commits, as losing the focus does.
    desk.click(*screen, legendPoint(0));
    entry()->setText("-3.1db");
    desk.click(*editor->getChildComponent(0), { 300.0f, 440.0f });
    dispatchMessages();
    CHECK(!screen->isShowingEntry());
    CHECK(holds(processor, ids::cutDepth, -3.1f));
    CHECK(gestures.balanced());
}

TEST_CASE("Plugin: every value on every page can be typed; text that is no value changes nothing")
{
    using rootnote::ui::Screen;
    RootnoteProcessor processor;
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);
    Desk desk(*editor);
    auto* screen = findFirst<Screen>(*editor);
    REQUIRE(screen != nullptr);
    set(processor, ids::focusLink, 0.0f); // a typed Focus would take over from the link
    GestureCounter gestures(processor);

    int fields = 0;
    for (int page = 0; page < rootnote::ui::kNumPages; ++page)
    {
        faceplate.selectPage(page);
        const auto& slots = rootnote::ui::pageSlots(page);
        for (int slot = 0; slot < rootnote::ui::kNumEncoders; ++slot)
        {
            if (slots[static_cast<size_t>(slot)].id == nullptr)
                continue;
            std::vector<float> values;
            for (auto* p : processor.getParameters())
                values.push_back(p->getValue());
            const auto unchanged = [&] {
                for (int i = 0; i < processor.getParameters().size(); ++i)
                    if (std::abs(processor.getParameters()[i]->getValue() - values[static_cast<size_t>(i)]) > 1.0e-4f)
                        return false;
                return true;
            };

            // Garbage is ignored; the field's own text reads back as the same value.
            for (const char* text : { "abc", "5 parsecs", "", "1.2.3" })
            {
                desk.click(*screen, legendPoint(slot));
                REQUIRE(screen->isShowingEntry());
                findFirst<juce::TextEditor>(*screen)->setText(text);
                desk.key(juce::KeyPress(juce::KeyPress::returnKey));
                CHECK(unchanged());
            }
            desk.click(*screen, legendPoint(slot));
            CHECK(findFirst<juce::TextEditor>(*screen)->getText().isNotEmpty());
            desk.key(juce::KeyPress(juce::KeyPress::returnKey));
            CHECK(unchanged());
            ++fields;
        }
    }
    CHECK(fields == 8 + 7 + 3 + 5 + 4);
    CHECK(gestures.balanced());
}

TEST_CASE("Plugin: typed values in natural formats, for the display and the host alike")
{
    RootnoteProcessor processor;
    const auto reads = [](const char* id, const char* text, float expected) {
        const auto value = parseValue(id, juce::String::fromUTF8(text), 440.0);
        return value.has_value() && std::abs(*value - expected) < 1.0e-3f;
    };
    CHECK(reads(ids::cutDepth, "-3.1", -3.1f));
    CHECK(reads(ids::cutDepth, "-3.1db", -3.1f));
    CHECK(reads(ids::cutDepth, "-3.1 DB", -3.1f));
    CHECK(reads(ids::cutDepth, "\xe2\x88\x92" "3,1 dB", -3.1f));
    CHECK(reads(ids::cutQ, "8", 8.0f));
    CHECK(reads(ids::cutQ, "q8", 8.0f));
    CHECK(reads(ids::cutOvertones, "50", 50.0f));
    CHECK(reads(ids::cutOvertones, "50%", 50.0f));
    CHECK(reads(ids::cutBands, "6", 6.0f));
    CHECK(reads(ids::cutNote, "C#1", 25.0f));
    CHECK(reads(ids::cutNote, "c#1", 25.0f));
    CHECK(reads(ids::cutNote, "Db1", 25.0f));
    CHECK(reads(ids::cutNote, "C\xe2\x99\xaf" "1", 25.0f));
    CHECK(reads(ids::cutNote, "D\xe2\x99\xad" "1", 25.0f));
    CHECK(reads(ids::boostFine, "+5", 5.0f));
    CHECK(reads(ids::boostFine, "-12ct", -12.0f));
    CHECK(reads(ids::boostFine, "5 cents", 5.0f));
    CHECK(reads(ids::cutOn, "on", 1.0f) && reads(ids::cutOn, "OFF", 0.0f));
    CHECK(reads(ids::cutOn, "1", 1.0f) && reads(ids::cutOn, "0", 0.0f));
    CHECK(reads(ids::focus, "1.2k", 1200.0f) && reads(ids::focus, "65hz", 65.0f));
    CHECK(reads(ids::ringDecay, "1.5s", 1500.0f) && reads(ids::glide, "40", 40.0f));
    CHECK(reads(ids::blend, "30/70", 0.7f) && reads(ids::blend, "70", 0.7f));
    CHECK(reads(ids::range, "2 oct", 2.0f) && reads(ids::ringDrop, "2st", 2.0f) && reads(ids::tilt, "-3 db/oct", -3.0f));
    CHECK(reads(ids::scale, "MIN", 2.0f) && reads(ids::scale, "pent min", 7.0f) && reads(ids::scale, "Harmonic Minor", 8.0f));
    CHECK(reads(ids::midiMode, "ch1+2", 1.0f) && reads(ids::qMode, "const w", 1.0f) && reads(ids::noteOff, "Return", 1.0f));
    CHECK(reads(ids::key, "f#", 6.0f) && reads(ids::key, "Bb", 10.0f) && reads(ids::series, "OCT", 1.0f));
    for (const char* id : { ids::cutDepth, ids::cutNote, ids::cutOn, ids::scale, ids::key, ids::boostFine, ids::bands })
        for (const char* text : { "abc", "", "5 parsecs", "1.2.3", "h1" })
            CHECK(!parseValue(id, text, 440.0).has_value());

    // Blend: one share, or two that make 100; anything else is no value.
    CHECK(reads(ids::blend, "30 / 70%", 0.7f) && reads(ids::blend, "0/100", 1.0f));
    for (const char* text : { "garbage/70", "1.2.3/70", "30/20/70", "30/60", "/70", "70/", "-20/120" })
        CHECK_MSG(!parseValue(ids::blend, text, 440.0).has_value(), text);
    // Beyond a float is no value, not infinity.
    CHECK(!parseValue(ids::mix, juce::String::repeatedString("9", 400), 440.0).has_value());

    // The host's text entry reads the same; text that is no value gives the default.
    CHECK_NEAR(parsed(processor, ids::cutDepth, "-3.1db"), -3.1, 1.0e-3);
    CHECK_NEAR(parsed(processor, ids::scale, "min"), 2.0, 1.0e-6);
    CHECK_NEAR(parsed(processor, ids::cutOn, "on"), 1.0, 1.0e-6);
    CHECK_NEAR(parsed(processor, ids::cutBands, "6 bands"), 6.0, 1.0e-6);
    CHECK_NEAR(parsed(processor, ids::cutDepth, "garbage"), -12.0, 1.0e-3);
    CHECK_NEAR(parsed(processor, ids::blend, "garbage/70"), 0.5, 1.0e-6);

    // Integers beyond their range clamp, the same for the host as for the display, at any size.
    for (const char* text : { "33", "2147483648", "99999999999999999999", "1e30" })
    {
        const auto typed = parseValue(ids::bands, text, 440.0);
        const float display = typed ? processor.getApvts().getParameter(ids::bands)->convertFrom0to1(
                                          processor.getApvts().getParameter(ids::bands)->convertTo0to1(*typed))
                                    : 8.0f;
        CHECK_MSG(parsed(processor, ids::bands, text) == display, text);
    }
    CHECK(parsed(processor, ids::bands, "2147483648") == 32.0f && parsed(processor, ids::bands, "-2147483649") == 1.0f);
    CHECK(parsed(processor, ids::bands, juce::String::repeatedString("9", 400)) == 8.0f); // no value: the default
}

TEST_CASE("Plugin: after a remap an encoder formats and parses its own parameter")
{
    using rootnote::ui::Encoder;
    RootnoteProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);
    auto* range = findControl<Encoder>(*editor, "Range");
    REQUIRE(range != nullptr);
    CHECK(range->getTextFromValue(1.5) == "1.50 oct");

    faceplate.selectPage(1); // Range's encoder is now the cut note's Fine
    CHECK(range->getTitle() == "Fine");
    CHECK(range->getTextFromValue(0.0) == "+0 ct");
    CHECK(range->getTextFromValue(-12.5) == juce::String::fromUTF8("\xe2\x88\x92" "12.5 ct"));
    CHECK_NEAR(range->getValueFromText("+25 ct"), 25.0, 1.0e-9);

    faceplate.selectPage(3); // Q Mode is a choice
    auto* qMode = findControl<Encoder>(*editor, "Q Mode");
    CHECK(qMode->getTextFromValue(1.0) == processor.getApvts().getParameter(ids::qMode)->getText(1.0f, 0));
}

TEST_CASE("Plugin: holding a page key peeks at its page, a click selects it")
{
    using rootnote::ui::Encoder;
    using rootnote::ui::Key;
    using rootnote::ui::Screen;
    RootnoteProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    Desk desk(*editor);
    auto* ring = findControl<Key>(*editor, "ring");
    auto* screen = findFirst<Screen>(*editor);
    REQUIRE(ring != nullptr && screen != nullptr);
    const auto legendShowsRing = [&] { return screen->hitTest(juce::roundToInt(legendPoint(2).x), 232)
                                              && !screen->hitTest(juce::roundToInt(legendPoint(3).x), 232); };
    const auto encodersOnBoost = [&] { return findControl<Encoder>(*editor, "Gain") != nullptr; };

    // Hold: the display shows the ring page while the key is down, then the boost page again.
    const juce::Point<float> middle = ring->getLocalBounds().getCentre().toFloat();
    desk.down(*ring, middle);
    CHECK(legendShowsRing());
    CHECK(encodersOnBoost());
    juce::Thread::sleep(static_cast<int>(RootnoteEditor::kPagePeekMs) + 50);
    desk.up(*ring, middle);
    CHECK(!legendShowsRing());
    CHECK(encodersOnBoost());

    // A short click selects it.
    desk.click(*ring);
    CHECK(legendShowsRing());
    CHECK(!encodersOnBoost() && findControl<Encoder>(*editor, "Amount") != nullptr);
}

TEST_CASE("Plugin: a page key held and released off the key selects nothing, and the next click works")
{
    using rootnote::ui::Encoder;
    using rootnote::ui::Key;
    RootnoteProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    Desk desk(*editor);
    auto* ring = findControl<Key>(*editor, "ring");
    const juce::Point<float> middle = ring->getLocalBounds().getCentre().toFloat();
    desk.down(*ring, middle);
    juce::Thread::sleep(static_cast<int>(RootnoteEditor::kPagePeekMs) + 50);
    desk.drag(*ring, { middle.x, -200.0f });
    desk.up(*ring, { middle.x, -200.0f });
    dispatchMessages();
    CHECK(findControl<Encoder>(*editor, "Gain") != nullptr);

    desk.click(*findControl<Key>(*editor, "cut"));
    CHECK(findControl<Encoder>(*editor, "Depth") != nullptr);
}

TEST_CASE("Plugin: the octave keys move the sounding note an octave, and the keys follow the note")
{
    using rootnote::ui::Key;
    RootnoteProcessor processor;
    prepare(processor);
    set(processor, ids::boostNote, 36.0f);
    set(processor, ids::cutNote, 40.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);
    Desk desk(*editor);
    GestureCounter gestures(processor);
    auto* up = findControl<Key>(*editor, "oct +");
    auto* down = findControl<Key>(*editor, juce::String::fromUTF8("oct \xe2\x88\x92"));
    REQUIRE(up != nullptr && down != nullptr);

    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    const auto run = [&] {
        for (int i = 0; i < 20; ++i)
        {
            block.clear();
            processor.processBlock(block, midi);
            midi.clear();
        }
        const auto state = processor.displayState();
        faceplate.refresh();
        return state;
    };

    // One gesture, heard at once: the engine's band doubles.
    const double before = run().boostHz;
    desk.click(*up);
    CHECK(holds(processor, ids::boostNote, 48.0f));
    CHECK(gestures.begins(ids::boostNote) == 1 && gestures.ends(ids::boostNote) == 1);
    CHECK_NEAR(run().boostHz, 2.0 * before, 0.001 * before);

    // The pitch keys are on the note's octave, however it got there.
    desk.click(*findControl<Key>(*editor, "d"));
    CHECK(holds(processor, ids::boostNote, 50.0f));
    set(processor, ids::boostNote, 62.0f); // automation
    run();
    desk.click(*findControl<Key>(*editor, "c"));
    CHECK(holds(processor, ids::boostNote, 60.0f));

    // Alt moves the cut note; the ends of the range clamp.
    desk.click(*down, juce::ModifierKeys(juce::ModifierKeys::altModifier));
    CHECK(holds(processor, ids::cutNote, 28.0f) && holds(processor, ids::boostNote, 60.0f));
    set(processor, ids::boostNote, 90.0f);
    desk.click(*up);
    CHECK(holds(processor, ids::boostNote, 96.0f));
    set(processor, ids::boostNote, 15.0f);
    desk.click(*down);
    CHECK(holds(processor, ids::boostNote, 12.0f));

    // With a MIDI note playing, the octave is of that note, and the knob takes the bank back.
    set(processor, ids::boostNote, 30.0f);
    set(processor, ids::midiMode, 1.0f);
    midi.addEvent(juce::MidiMessage::noteOn(1, 40, 1.0f), 0);
    REQUIRE(run().boostFromMidi);
    desk.click(*up);
    desk.click(*up); // twice before the next frame
    CHECK(holds(processor, ids::boostNote, 64.0f));
    const auto state = run();
    CHECK(!state.boostFromMidi);
    CHECK_NEAR(state.boostNote, 64.0, 1.0e-6);

    // A display refresh between the presses, before any audio block: the stale frame still says MIDI 40.
    midi.addEvent(juce::MidiMessage::noteOn(1, 40, 1.0f), 0);
    REQUIRE(run().boostFromMidi);
    desk.click(*up);
    CHECK(holds(processor, ids::boostNote, 52.0f));
    faceplate.refresh();
    desk.click(*up);
    CHECK(holds(processor, ids::boostNote, 64.0f));
    // Newer MIDI after the press wins again.
    midi.addEvent(juce::MidiMessage::noteOn(1, 41, 1.0f), 0);
    REQUIRE(run().boostFromMidi);
    desk.click(*up);
    CHECK(holds(processor, ids::boostNote, 53.0f));
    CHECK(gestures.balanced());
}

TEST_CASE("Plugin: match cut toggles the cut and blinks without a hit; its LED follows the cut")
{
    using rootnote::ui::Key;
    RootnoteProcessor processor;
    prepare(processor);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);
    Desk desk(*editor);
    auto* match = findControl<Key>(*editor, "match cut");
    auto* tune = findControl<Key>(*editor, "tune to key");
    REQUIRE(match != nullptr && tune != nullptr);
    const auto frames = [&](int count) {
        bool on = false, off = false;
        for (int i = 0; i < count; ++i)
        {
            faceplate.refresh();
            on = on || match->getLed() == Key::Led::on || tune->getLed() == Key::Led::on;
            off = off || match->getLed() == Key::Led::off;
        }
        return on && off;
    };

    // No hit yet: nothing changes, and the keys blink, then settle.
    desk.click(*match);
    desk.click(*tune);
    CHECK(holds(processor, ids::cutOn, 0.0f));
    CHECK(frames(12));
    frames(30);
    CHECK(match->getLed() == Key::Led::off && tune->getLed() == Key::Led::none);

    // With a hit: on at the hit, then off; the LED shows the cut.
    playKicks(processor, noteToHz(26.2), 48000.0);
    REQUIRE(waitForReading(processor, 3));
    desk.click(*match);
    CHECK(holds(processor, ids::cutOn, 1.0f));
    CHECK_NEAR(get(processor, ids::cutNote), 26.2, 0.1);
    faceplate.refresh();
    CHECK(match->getLed() == Key::Led::on);
    desk.click(*match);
    CHECK(holds(processor, ids::cutOn, 0.0f));
    faceplate.refresh();
    CHECK(match->getLed() == Key::Led::off);

    // Automation and the Cut encoder show the same.
    set(processor, ids::cutOn, 1.0f);
    faceplate.refresh();
    CHECK(match->getLed() == Key::Led::on);
}

TEST_CASE("Plugin: the display draws the Focus window where the weighting puts it")
{
    using rootnote::ui::Screen;
    RootnoteProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);
    auto* screen = findFirst<Screen>(*editor);
    REQUIRE(screen != nullptr);
    const double lowHz = 16.3516 * std::exp2(-1.0 / 24.0);
    const auto xAt = [lowHz](double hz) { return 16.0 + 528.0 * std::log2(hz / lowHz) / 9.0; };
    const double octave = 528.0 / 9.0;

    set(processor, ids::focusLink, 0.0f);
    set(processor, ids::focus, 200.0f);
    set(processor, ids::range, 1.0f);
    faceplate.refresh();
    auto marks = screen->focusMarks();
    CHECK(marks.visible && !marks.linked);
    CHECK_NEAR(marks.centre, xAt(200.0), 0.05);
    CHECK_NEAR(marks.high - marks.low, 2.0 * octave, 0.05);

    set(processor, ids::range, 2.5f);
    faceplate.refresh();
    CHECK_NEAR(screen->focusMarks().high - screen->focusMarks().low, 5.0 * octave, 0.05);

    // Linked, it follows the boost note.
    set(processor, ids::focusLink, 1.0f);
    set(processor, ids::boostNote, 48.0f);
    faceplate.refresh();
    marks = screen->focusMarks();
    CHECK(marks.linked);
    CHECK_NEAR(marks.centre, xAt(noteToHz(48.0)), 0.05);

    set(processor, ids::boostGain, 0.0f);
    faceplate.refresh();
    CHECK(!screen->focusMarks().visible);
}

TEST_CASE("Plugin: the legend says when a value has no effect, and Focus takes over from the link")
{
    using rootnote::ui::Encoder;
    using rootnote::ui::legendValue;
    using rootnote::ui::pageSlots;
    RootnoteProcessor processor;
    auto& apvts = processor.getApvts();
    const auto& boost = pageSlots(0);
    CHECK(legendValue(boost[3], apvts) == "LINK");
    CHECK(legendValue(boost[2], apvts) == "8" && legendValue(boost[5], apvts) == "50/50");
    set(processor, ids::series, 1.0f); // octaves: every octave sounds, no odd/even
    CHECK(legendValue(boost[2], apvts) == "ALL" && legendValue(boost[5], apvts) == "N/A");
    CHECK(legendValue(pageSlots(1)[3], apvts) == "ALL");
    set(processor, ids::series, 2.0f);
    CHECK(legendValue(boost[2], apvts) == "8" && legendValue(boost[5], apvts) == "N/A");

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    Desk desk(*editor);
    GestureCounter gestures(processor);
    auto* focus = findControl<Encoder>(*editor, "Focus");
    desk.down(*focus, { 40.0f, 30.0f });
    desk.drag(*focus, { 40.0f, 10.0f });
    desk.up(*focus, { 40.0f, 10.0f });
    CHECK(holds(processor, ids::focusLink, 0.0f));
    CHECK(get(processor, ids::focus) > 65.0f);
    CHECK(legendValue(boost[3], apvts) != "LINK");
    CHECK(gestures.balanced());
}


// ---- State and load reliability ---------------------------------------------------------

namespace
{
/** Every parameter at a random value it can hold, different from its default. */
void randomise(RootnoteProcessor& processor, juce::Random& random)
{
    for (auto* parameter : processor.getParameters())
    {
        auto* ranged = static_cast<juce::RangedAudioParameter*>(parameter);
        float value = ranged->getDefaultValue();
        while (value == ranged->getDefaultValue())
            value = ranged->convertTo0to1(ranged->convertFrom0to1(random.nextFloat()));
        ranged->setValueNotifyingHost(value);
    }
}

juce::MemoryBlock stateOf(RootnoteProcessor& processor)
{
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    return state;
}

void restore(RootnoteProcessor& processor, const juce::MemoryBlock& state)
{
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
}

/** Bit for bit: every normalised value and the plain value behind it. */
bool sameParameters(RootnoteProcessor& a, RootnoteProcessor& b)
{
    for (int i = 0; i < a.getParameters().size(); ++i)
    {
        auto* pa = static_cast<juce::RangedAudioParameter*>(a.getParameters()[i]);
        const auto id = pa->getParameterID();
        if (pa->getValue() != b.getParameters()[i]->getValue()
            || a.getApvts().getRawParameterValue(id)->load() != b.getApvts().getRawParameterValue(id)->load())
        {
            std::printf("    differs: %s\n", id.toRawUTF8());
            return false;
        }
    }
    return true;
}

/** Every value is a legal one: finite and inside its range. */
bool validParameters(RootnoteProcessor& processor)
{
    for (auto* parameter : processor.getParameters())
    {
        auto* ranged = static_cast<juce::RangedAudioParameter*>(parameter);
        const float plain = processor.getApvts().getRawParameterValue(ranged->getParameterID())->load();
        const auto& range = ranged->getNormalisableRange();
        if (!std::isfinite(plain) || plain < range.start || plain > range.end || !(ranged->getValue() >= 0.0f && ranged->getValue() <= 1.0f))
        {
            std::printf("    invalid: %s plain %g normalised %g\n", ranged->getParameterID().toRawUTF8(), (double) plain, (double) ranged->getValue());
            return false;
        }
    }
    return true;
}

/** Two seconds of kicks and noise through the processor, as one buffer per block. */
std::vector<float> render(RootnoteProcessor& processor, double fs = 48000.0, int blockSize = 512)
{
    Random random(3);
    std::vector<float> input(static_cast<size_t>(fs * 2.0), 0.0f);
    KickSpec kick;
    kick.hz = 55.0;
    for (int hit = 0; hit < 4; ++hit)
        addKick(input, static_cast<size_t>(fs * (0.05 + 0.45 * hit)), kick, fs, random);
    for (auto& x : input)
        x += 0.01f * (static_cast<float>(random.uniform()) - 0.5f);

    std::vector<float> output;
    juce::MidiBuffer midi;
    for (size_t position = 0; position < input.size(); position += static_cast<size_t>(blockSize))
    {
        const int n = static_cast<int>(std::min(static_cast<size_t>(blockSize), input.size() - position));
        juce::AudioBuffer<float> block(2, n);
        block.copyFrom(0, 0, input.data() + position, n);
        block.copyFrom(1, 0, input.data() + position, n);
        processor.processBlock(block, midi);
        for (int i = 0; i < n; ++i)
            output.push_back(block.getSample(0, i) + 0.5f * block.getSample(1, i));
    }
    return output;
}

juce::MemoryBlock fixture(const char* name)
{
    juce::MemoryBlock data;
    juce::File(ROOTNOTE_FIXTURES).getChildFile(name).loadFileAsData(data);
    return data;
}

/** JUCE's binary state header around any text, valid XML or not. */
juce::MemoryBlock blobOf(const juce::String& text)
{
    juce::MemoryOutputStream out;
    out.writeInt(0x21324356);
    out.writeInt(static_cast<int>(text.getNumBytesAsUTF8()) + 1);
    out << text;
    out.writeByte(0);
    return out.getMemoryBlock();
}
} // namespace

TEST_CASE("Plugin: identifiers are frozen: codes, parameter IDs, order, ranges and defaults")
{
    // Changing any of these breaks saved sessions and automation in every host. Never edit this table:
    // add new parameters at the end with new IDs instead.
    CHECK(JucePlugin_PluginCode == 0x52744e74);       // 'RtNt'
    CHECK(JucePlugin_ManufacturerCode == 0x44776b73); // 'Dwks'
    CHECK(juce::String(JucePlugin_AUMainType) == "aufx" || JucePlugin_AUMainType == 'aufx');
    CHECK(juce::String(JUCE_STRINGIFY(JucePlugin_CFBundleIdentifier)) == "com.dripworks.rootnote");
    CHECK(juce::String(JucePlugin_Name) == "Rootnote");
    CHECK(juce::String(JucePlugin_Manufacturer) == "daveydidit"); // visible brand only; codes above stay
    CHECK(JucePlugin_VSTUniqueID == JucePlugin_PluginCode);
    CHECK(JUCE_VST3_CAN_REPLACE_VST2 == 0); // part of the VST3 class ID
    CHECK(RootnoteProcessor::kStateSchema == 2);

    struct Frozen { const char* id; double start, end, interval, middle, defaultValue; };
    const Frozen frozen[] = {
        { "boost_note", 12, 96, 0.00999999978, 54, 36 },
        { "boost_fine", -50, 50, 0.100000001, 0, 0 },
        { "boost_gain", 0, 24, 0.100000001, 12, 9 },
        { "boost_q", 0.5, 200, 0, 10, 12 },
        { "series", 0, 2, 1, 1, 0 },
        { "bands", 1, 32, 1, 16, 8 },
        { "blend", 0, 1, 0.00999999978, 0.5, 0.5 },
        { "focus_link", 0, 1, 1, 1, 1 },
        { "focus", 20, 8000, 0, 400, 65 },
        { "range", 0.100000001, 6, 0.00999999978, 3.04999995, 1.5 },
        { "tilt", -12, 6, 0.100000001, -3, -3 },
        { "q_mode", 0, 1, 1, 0, 0 },
        { "cut_on", 0, 1, 1, 1, 0 },
        { "cut_note", 12, 96, 0.00999999978, 54, 36 },
        { "cut_depth", -30, 0, 0.100000001, -15, -12 },
        { "cut_q", 0.5, 60, 0, 5.47722578, 8 },
        { "cut_overtones", 0, 100, 0.100000001, 50, 50 },
        { "cut_bands", 1, 16, 1, 8, 6 },
        { "ring", 0, 100, 0.100000001, 50, 0 },
        { "ring_decay", 50, 4000, 0, 447.213593, 600 },
        { "ring_drop", 0, 12, 0.00999999978, 6, 0 },
        { "midi_mode", 0, 2, 1, 1, 0 },
        { "glide", 0, 500, 1, 250, 0 },
        { "note_off", 0, 1, 1, 0, 0 },
        { "key", 0, 11, 1, 6, 0 },
        { "scale", 0, 8, 1, 4, 0 },
        { "key_lock", 0, 1, 1, 1, 0 },
        { "a4", 415, 466, 0.100000001, 440.5, 440 },
        { "mix", 0, 100, 0.100000001, 50, 100 },
        { "output", -24, 12, 0.100000001, -6, 0 },
        { "limiter", 0, 1, 1, 1, 1 },
        { "bypass", 0, 1, 1, 1, 0 },
    };
    RootnoteProcessor processor;
    const auto& all = processor.getParameters();
    REQUIRE(all.size() == static_cast<int>(std::size(frozen)));
    for (int i = 0; i < all.size(); ++i)
    {
        auto* p = static_cast<juce::RangedAudioParameter*>(all[i]);
        const Frozen& f = frozen[i];
        const auto& range = p->getNormalisableRange();
        const auto near = [](double a, double b) { return std::abs(a - b) <= 1.0e-6 * std::max(1.0, std::abs(b)); };
        CHECK_MSG(p->getParameterID() == f.id && p->getParameterIndex() == i && p->getVersionHint() == 1, f.id);
        CHECK_MSG(near(range.start, f.start) && near(range.end, f.end) && near(range.interval, f.interval), f.id);
        CHECK_MSG(near(p->convertFrom0to1(0.5f), f.middle), f.id);
        CHECK_MSG(near(p->convertFrom0to1(p->getDefaultValue()), f.defaultValue), f.id);
    }
    CHECK(processor.getBypassParameter() == processor.getApvts().getParameter("bypass"));
}

TEST_CASE("Plugin: a full state round trip is bit-exact, keeps A/B, preset and editor, and sounds the same")
{
    juce::Random random(2024);
    RootnoteProcessor source;
    source.loadPreset(4);
    randomise(source, random);
    source.selectAbSlot(1); // A keeps these; B goes on with others
    randomise(source, random);
    source.storeEditorSize(1080, 607);
    source.storeEditorPage(3);
    const auto state = stateOf(source);

    RootnoteProcessor restored;
    restore(restored, state);
    CHECK(sameParameters(source, restored));
    CHECK(restored.abSlot() == 1 && restored.currentPreset() == source.currentPreset());
    CHECK(restored.storedEditorSize() == juce::Point<int>(1080, 607) && restored.storedEditorPage() == 3);
    CHECK(stateOf(restored) == state);
    CHECK(stateOf(source) == state); // saving changes nothing

    // The program-state paths of the hosts are the same state.
    juce::MemoryBlock program;
    source.getCurrentProgramStateInformation(program);
    RootnoteProcessor viaProgram;
    viaProgram.setCurrentProgramStateInformation(program.getData(), static_cast<int>(program.getSize()));
    CHECK(sameParameters(source, viaProgram) && viaProgram.abSlot() == 1);

    // Slot A comes back the same in both.
    source.selectAbSlot(0);
    restored.selectAbSlot(0);
    CHECK(sameParameters(source, restored));
    CHECK(restored.currentPreset() == 4);

    // The same input gives the same output, sample for sample.
    for (auto* p : { &source, &restored })
    {
        set(*p, ids::bypass, 0.0f);
        set(*p, ids::mix, 100.0f);
        prepare(*p);
    }
    CHECK(render(source) == render(restored));
}

TEST_CASE("Plugin: a state loads the same before or after preparing, with an editor open, at any rate and block size")
{
    juce::Random random(77);
    RootnoteProcessor source;
    randomise(source, random);
    set(source, ids::bypass, 0.0f);
    source.storeEditorPage(2);
    const auto state = stateOf(source);

    struct Setup { double fs; int block; };
    for (const Setup setup : { Setup { 48000.0, 512 }, Setup { 44100.0, 64 }, Setup { 96000.0, 1024 }, Setup { 192000.0, 7 } })
    {
        RootnoteProcessor before, after, withEditor;
        restore(before, state);
        prepare(before, setup.fs, setup.block);

        prepare(after, setup.fs, setup.block);
        render(after, setup.fs, setup.block); // already playing
        restore(after, state);
        after.reset();

        prepare(withEditor, setup.fs, setup.block);
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor(withEditor.createEditor());
            restore(withEditor, state);
            dynamic_cast<RootnoteEditor&>(*editor).refresh();
            CHECK(sameParameters(source, withEditor));
        }
        std::unique_ptr<juce::AudioProcessorEditor> reopened(withEditor.createEditor());
        CHECK(withEditor.storedEditorPage() == 2);
        reopened.reset();

        CHECK(sameParameters(source, before) && sameParameters(source, after));
        const auto reference = render(before, setup.fs, setup.block);
        bool finite = true;
        for (float x : reference)
            finite = finite && std::isfinite(x);
        CHECK(finite);
        CHECK(render(after, setup.fs, setup.block) == reference);
        withEditor.reset();
        CHECK(render(withEditor, setup.fs, setup.block) == reference);
    }

    // A host that never prepares, or prepares with no usable rate, gets its audio back untouched.
    RootnoteProcessor unprepared;
    restore(unprepared, state);
    unprepared.prepareToPlay(0.0, 512);
    juce::AudioBuffer<float> block(2, 64);
    block.clear();
    block.setSample(0, 3, 0.5f);
    juce::MidiBuffer midi;
    unprepared.processBlock(block, midi);
    CHECK(block.getSample(0, 3) == 0.5f);
}

TEST_CASE("Plugin: a restore while the editor is open shows the restored page and size and drops a pending entry")
{
    using rootnote::ui::Screen;
    RootnoteProcessor source;
    set(source, ids::cutDepth, -20.0f);
    source.storeEditorPage(3);
    source.storeEditorSize(1200, 675);
    const auto state = stateOf(source);

    RootnoteProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);
    Desk desk(*editor);
    auto* screen = findFirst<Screen>(*editor);
    faceplate.selectPage(1);
    desk.click(*screen, legendPoint(0)); // typing the cut depth
    REQUIRE(screen->isShowingEntry());
    findFirst<juce::TextEditor>(*screen)->setText("-3");

    restore(processor, state); // the host, with the editor still open
    faceplate.refresh();
    CHECK(faceplate.currentPage() == 3);
    CHECK(editor->getWidth() == 1200 && editor->getHeight() == 675);
    CHECK(!screen->isShowingEntry());
    editor->unfocusAllComponents();
    dispatchMessages();
    CHECK(holds(processor, ids::cutDepth, -20.0f)); // the old field wrote nothing
    CHECK(processor.storedEditorPage() == 3);
}

TEST_CASE("Plugin: saving, restoring and switching A/B are whole transactions, whatever thread they come from")
{
    // A listener on the gain runs in the middle of each operation and starts the other one
    // on a second thread: it must wait for the first to finish, never see half of it.
    struct MidOperation : juce::AudioProcessorParameter::Listener
    {
        std::function<void()> once;
        void parameterValueChanged(int, float) override
        {
            if (auto run = std::exchange(once, nullptr))
                run();
        }
        void parameterGestureChanged(int, bool) override {}
    };

    const auto savedActive = [](const juce::MemoryBlock& blob) {
        const auto xml = juce::AudioProcessor::getXmlFromBinary(blob.getData(), static_cast<int>(blob.getSize()));
        return xml->getChildByName("AB")->getIntAttribute("active");
    };
    const auto savedGain = [](const juce::MemoryBlock& blob) {
        const auto xml = juce::AudioProcessor::getXmlFromBinary(blob.getData(), static_cast<int>(blob.getSize()));
        for (auto* param : xml->getChildWithTagNameIterator("PARAM"))
            if (param->getStringAttribute("id") == ids::boostGain)
                return param->getDoubleAttribute("value");
        return -1.0;
    };

    // Save during a switch to B: the saved state is all A or all B, and here, after the switch, all B.
    {
        RootnoteProcessor processor;
        set(processor, ids::boostGain, 4.0f);
        processor.selectAbSlot(1);
        set(processor, ids::boostGain, 20.0f);
        processor.selectAbSlot(0); // A live (4 dB), B stored (20 dB)

        MidOperation mid;
        juce::MemoryBlock saved;
        std::atomic<bool> finishedEarly { false };
        std::thread saver;
        mid.once = [&] {
            saver = std::thread([&] { processor.getStateInformation(saved); });
            juce::Thread::sleep(100);
            finishedEarly = saved.getSize() > 0;
        };
        auto* gain = processor.getApvts().getParameter(ids::boostGain);
        gain->addListener(&mid);
        processor.selectAbSlot(1);
        gain->removeListener(&mid);
        saver.join();
        CHECK(!finishedEarly);
        CHECK(savedActive(saved) == 1);
        CHECK_NEAR(savedGain(saved), 20.0, 1.0e-4);
        RootnoteProcessor reloaded;
        restore(reloaded, saved);
        reloaded.selectAbSlot(0);
        CHECK(holds(reloaded, ids::boostGain, 4.0f));
    }

    // Switch during a restore: it comes after the whole restore, and switches from the restored slot.
    {
        RootnoteProcessor source;
        set(source, ids::boostGain, 6.0f);
        source.selectAbSlot(1);
        set(source, ids::boostGain, 18.0f); // saved: B active at 18 dB, A stored at 6 dB
        const auto state = stateOf(source);

        RootnoteProcessor processor;
        set(processor, ids::boostGain, 1.0f);
        MidOperation mid;
        std::thread switcher;
        mid.once = [&] {
            switcher = std::thread([&] { processor.selectAbSlot(0); });
            juce::Thread::sleep(100);
        };
        auto* gain = processor.getApvts().getParameter(ids::boostGain);
        gain->addListener(&mid);
        restore(processor, state);
        gain->removeListener(&mid);
        switcher.join();
        CHECK(processor.abSlot() == 0 && holds(processor, ids::boostGain, 6.0f));
        processor.selectAbSlot(1);
        CHECK(holds(processor, ids::boostGain, 18.0f));
    }

    // A listener that saves on the same thread, mid-switch, does not deadlock.
    {
        RootnoteProcessor processor;
        processor.selectAbSlot(1);
        set(processor, ids::boostGain, 3.0f);
        MidOperation mid;
        juce::MemoryBlock saved;
        mid.once = [&] { processor.getStateInformation(saved); };
        processor.getApvts().getParameter(ids::boostGain)->addListener(&mid);
        processor.selectAbSlot(0);
        processor.getApvts().getParameter(ids::boostGain)->removeListener(&mid);
        CHECK(saved.getSize() > 0);
    }
}

TEST_CASE("Plugin: opening and closing a value's field without editing it changes nothing")
{
    using rootnote::ui::Screen;
    RootnoteProcessor processor;
    set(processor, ids::boostQ, 8.123f);   // shown as 8.1
    set(processor, ids::focus, 123.456f); // shown while linked
    set(processor, ids::range, 1.234f);
    REQUIRE(holds(processor, ids::focusLink, 1.0f));
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto& faceplate = dynamic_cast<RootnoteEditor&>(*editor);
    Desk desk(*editor);
    auto* screen = findFirst<Screen>(*editor);
    faceplate.selectPage(0);
    GestureCounter gestures(processor);

    std::vector<float> before;
    for (auto* p : processor.getParameters())
        before.push_back(p->getValue());
    const auto& slots = rootnote::ui::pageSlots(0);
    for (int slot = 0; slot < rootnote::ui::kNumEncoders; ++slot)
    {
        if (slots[static_cast<size_t>(slot)].id == nullptr)
            continue;
        desk.click(*screen, legendPoint(slot)); // Return
        REQUIRE(screen->isShowingEntry());
        desk.key(juce::KeyPress(juce::KeyPress::returnKey));
        desk.click(*screen, legendPoint(slot)); // focus loss
        REQUIRE(screen->isShowingEntry());
        editor->unfocusAllComponents();
        dispatchMessages();
        CHECK(!screen->isShowingEntry());
    }
    for (int i = 0; i < processor.getParameters().size(); ++i)
        CHECK(processor.getParameters()[i]->getValue() == before[static_cast<size_t>(i)]);
    CHECK(gestures.begins(ids::focusLink) == 0 && gestures.begins(ids::boostQ) == 0);

    // A real edit of Focus still takes over from the link.
    int focusSlot = 0;
    while (slots[static_cast<size_t>(focusSlot)].id == nullptr || std::strcmp(slots[static_cast<size_t>(focusSlot)].id, ids::focus) != 0)
        ++focusSlot;
    desk.click(*screen, legendPoint(focusSlot));
    findFirst<juce::TextEditor>(*screen)->setText("200 hz");
    desk.key(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK(holds(processor, ids::focusLink, 0.0f) && std::abs(get(processor, ids::focus) - 200.0f) < 0.5f);
    CHECK(gestures.balanced());
}

TEST_CASE("Plugin: states saved by 0.4.0, 0.4.1 and 0.4.2 load with their values")
{
    // Written by each release's own code (Tests/fixtures/README.md); preset 3 then these edits.
    const std::pair<const char*, float> edited[] = {
        { "boost_note", 31.25f }, { "boost_fine", -12.5f }, { "boost_gain", 14.2f }, { "boost_q", 33.0f }, { "bands", 11.0f },
        { "blend", 0.3f }, { "focus_link", 0.0f }, { "focus", 140.0f }, { "range", 2.25f }, { "tilt", -4.5f },
        { "q_mode", 1.0f }, { "cut_on", 1.0f }, { "cut_note", 40.5f }, { "cut_depth", -21.0f }, { "cut_q", 15.0f },
        { "cut_overtones", 35.0f }, { "cut_bands", 9.0f }, { "ring", 45.0f }, { "ring_decay", 1200.0f }, { "ring_drop", 3.5f },
        { "midi_mode", 2.0f }, { "glide", 80.0f }, { "note_off", 1.0f }, { "key", 9.0f }, { "scale", 7.0f },
        { "key_lock", 1.0f }, { "a4", 432.0f }, { "mix", 85.0f }, { "output", -4.5f }, { "limiter", 0.0f }, { "bypass", 0.0f }
    };
    for (const char* version : { "0.4.0", "0.4.1", "0.4.2" })
    {
        const auto data = fixture((juce::String("state-") + version + ".bin").toRawUTF8());
        REQUIRE(data.getSize() > 0);
        RootnoteProcessor processor;
        restore(processor, data);
        for (const auto& [id, value] : edited)
            CHECK_MSG(holds(processor, id, value), std::string(version) + " " + id);
        CHECK(processor.currentPreset() == 3);
        CHECK(processor.storedEditorSize() == juce::Point<int>(1200, 675));
        CHECK(processor.abSlot() == 0 && processor.storedEditorPage() == 0);

        // Saved again, it is today's schema with the same values.
        RootnoteProcessor again;
        restore(again, stateOf(processor));
        CHECK(sameParameters(processor, again));
        CHECK(juce::AudioProcessor::getXmlFromBinary(stateOf(again).getData(), static_cast<int>(stateOf(again).getSize()))
                  ->getIntAttribute("schema") == RootnoteProcessor::kStateSchema);
    }

    // Each release's defaults: 0.4.0 and 0.4.1 sessions keep their MIDI Ch 1+2, 0.4.2 its Off.
    for (const auto& [name, midiMode] : { std::pair { "default-0.4.0.bin", 1.0f }, { "default-0.4.1.bin", 1.0f }, { "default-0.4.2.bin", 0.0f } })
    {
        RootnoteProcessor processor;
        juce::Random random(5);
        randomise(processor, random);
        restore(processor, fixture(name));
        CHECK_MSG(holds(processor, ids::midiMode, midiMode), name);
        CHECK_MSG(holds(processor, ids::boostNote, 36.0f) && holds(processor, ids::limiter, 1.0f) && processor.currentPreset() == 0, name);
    }
}

TEST_CASE("Plugin: damaged, foreign or hostile states never crash and always leave a valid state")
{
    juce::Random random(4242);
    RootnoteProcessor processor;
    prepare(processor);
    randomise(processor, random);
    set(processor, ids::bypass, 0.0f);
    const auto good = stateOf(processor);
    const auto goodText = juce::String::fromUTF8(static_cast<const char*>(good.getData()) + 8, static_cast<int>(good.getSize()) - 9);
    RootnoteProcessor reference;
    restore(reference, good);

    const auto unchanged = [&] { return sameParameters(processor, reference); };
    const auto playable = [&] {
        juce::AudioBuffer<float> block(2, 256);
        block.clear();
        block.setSample(0, 0, 1.0f);
        juce::MidiBuffer midi;
        processor.processBlock(block, midi);
        if (!allFinite(block))
            std::printf("    not finite output\n");
        return allFinite(block) && validParameters(processor);
    };

    // Not a state at all: nothing changes.
    juce::MemoryBlock empty;
    restore(processor, empty);
    processor.setStateInformation(nullptr, 100);
    processor.setStateInformation(good.getData(), -5);
    CHECK(unchanged());
    for (int i = 0; i < 300; ++i)
    {
        juce::MemoryBlock noise(static_cast<size_t>(random.nextInt(4096)));
        random.fillBitsRandomly(noise.getData(), noise.getSize());
        if (i % 2 == 0 && noise.getSize() >= 4) // half of them with a valid header
        {
            const auto magic = juce::ByteOrder::swapIfBigEndian(static_cast<juce::uint32>(0x21324356));
            std::memcpy(noise.getData(), &magic, sizeof(magic));
        }
        restore(processor, noise);
    }
    CHECK(unchanged());
    for (size_t length = 0; length < good.getSize(); length += 7) // truncated
        processor.setStateInformation(good.getData(), static_cast<int>(length));
    CHECK(unchanged() && playable());
    for (int i = 0; i < 300; ++i) // flipped bits
    {
        juce::MemoryBlock damaged(good);
        static_cast<juce::uint8*>(damaged.getData())[random.nextInt(static_cast<int>(damaged.getSize()))] ^= static_cast<juce::uint8>(1 << random.nextInt(8));
        restore(processor, damaged);
        REQUIRE(validParameters(processor));
    }
    CHECK(playable());
    for (const char* text : { "", "<", "<RootnoteState", "<Other><PARAM id=\"mix\" value=\"5\"/></Other>", "\xff\xfe garbage" })
    {
        restore(processor, good);
        restore(processor, blobOf(text));
        CHECK_MSG(unchanged(), text);
    }

    // Readable, but wrong: unreadable values take their default, out-of-range ones are clamped,
    // unknown fields are ignored, and a bad A/B slot is dropped.
    const juce::String hostile = R"(<RootnoteState schema="99" preset="abc" editorWidth="1e999" editorPage="-7" future="x">
        <PARAM id="mix" value="nan"/><PARAM id="output" value="1e30"/><PARAM id="boost_note" value="-1e30"/>
        <PARAM id="bands" value="seven"/><PARAM id="cut_on" value="0.86"/><PARAM id="focus" value="inf"/>
        <PARAM id="limiter"/><PARAM id="unknown_param" value="3"/><FUTURE><X/></FUTURE>
        <AB active="5"><SLOT index="0" preset="2" mix="0.5"/><SLOT index="1" preset="1"/></AB></RootnoteState>)";
    restore(processor, blobOf(hostile));
    CHECK(validParameters(processor) && playable());
    CHECK(holds(processor, ids::mix, 100.0f) && holds(processor, ids::output, 12.0f) && holds(processor, ids::boostNote, 12.0f));
    CHECK(holds(processor, ids::bands, 8.0f) && holds(processor, ids::cutOn, 1.0f) && holds(processor, ids::focus, 65.0f));
    CHECK(holds(processor, ids::limiter, 1.0f));
    CHECK(processor.currentPreset() == 0 && processor.storedEditorPage() == 0 && processor.abSlot() == 1);
    processor.selectAbSlot(0); // slot A was incomplete: B's settings carry over
    CHECK(holds(processor, ids::mix, 100.0f));

    // A number is exactly one number: malformed or fractional ones are invalid, never a numeric prefix.
    restore(processor, good);
    const juce::String malformed = R"(<RootnoteState schema="2" preset="2-1" editorPage="1.5" editorWidth="1200x">
        <PARAM id="mix" value="1.2.3"/><PARAM id="output" value="-3dB"/><PARAM id="bands" value="0x10"/>
        <PARAM id="ring" value=" 50 "/><PARAM id="glide" value="1e+2"/>
        <AB active="0.5"><SLOT index="1" preset="1" )" + [&] {
            juce::String values;
            for (auto* parameter : processor.getParameters())
                values << static_cast<juce::RangedAudioParameter*>(parameter)->getParameterID() << "=\"0.4.5\" ";
            return values;
        }() + R"(/><SLOT index="0.5" preset="1"/></AB></RootnoteState>)";
    restore(processor, blobOf(malformed));
    CHECK(validParameters(processor));
    CHECK(holds(processor, ids::mix, 100.0f) && holds(processor, ids::output, 0.0f) && holds(processor, ids::bands, 8.0f));
    CHECK(holds(processor, ids::ring, 50.0f) && holds(processor, ids::glide, 100.0f));
    CHECK(processor.currentPreset() == 0 && processor.storedEditorPage() == 0 && processor.storedEditorSize().x == 0);
    CHECK(processor.abSlot() == 0);
    set(processor, ids::ring, 10.0f);
    processor.selectAbSlot(1); // slot B was invalid: it starts as a copy
    CHECK(holds(processor, ids::ring, 10.0f));

    // Huge: a large state with a large unknown field still loads.
    restore(processor, blobOf(goodText.replace("<AB", "<FUTURE data=\"" + juce::String::repeatedString("x", 8 << 20) + "\"/><AB")));
    CHECK(unchanged());

    // Any normalised value of any parameter, from a host: always legal afterwards.
    for (int i = 0; i < 2000; ++i)
    {
        auto* parameter = processor.getParameters()[random.nextInt(processor.getParameters().size())];
        parameter->setValueNotifyingHost(random.nextFloat());
    }
    CHECK(validParameters(processor) && playable());
}

TEST_CASE("Plugin: repeated create, play, save, restore and destroy cycles leave no threads or memory behind")
{
#if JUCE_LINUX
    const auto threads = [] { return juce::File("/proc/self/task").getNumberOfChildFiles(juce::File::findDirectories); };
    const auto residentPages = [] {
        long size = 0, resident = 0;
        if (FILE* f = std::fopen("/proc/self/statm", "r"))
        {
            if (std::fscanf(f, "%ld %ld", &size, &resident) != 2)
                resident = 0;
            std::fclose(f);
        }
        return resident;
    };
#endif
    juce::Random random(11);
    juce::MemoryBlock state;
    const auto cycle = [&] {
        auto processor = std::make_unique<RootnoteProcessor>();
        if (state.getSize() > 0)
            restore(*processor, state);
        prepare(*processor, 48000.0, 256);
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
        randomise(*processor, random);
        juce::AudioBuffer<float> block(2, 256);
        block.clear();
        juce::MidiBuffer midi;
        for (int i = 0; i < 8; ++i)
            processor->processBlock(block, midi);
        dynamic_cast<RootnoteEditor&>(*editor).refresh();
        processor->selectAbSlot(random.nextInt(2));
        state = stateOf(*processor);
        editor.reset();
        processor->releaseResources();
    };
    for (int i = 0; i < 5; ++i)
        cycle();
    dispatchMessages();
#if JUCE_LINUX
    const int threadsBefore = threads();
    const long pagesBefore = residentPages();
#endif
    for (int i = 0; i < 60; ++i)
        cycle();
    dispatchMessages();
#if JUCE_LINUX
    CHECK(threads() == threadsBefore);
    // Only a diagnostic: allocator and font caches keep memory without leaking. Leaks are
    // JUCE's leak detector's (debug) and the sanitizers' to find.
    std::printf("    resident growth over 60 cycles: %.1f MB\n",
                static_cast<double>(residentPages() - pagesBefore) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0));
#endif
}

/**
    Renders the editor with a demo state to a PNG: a kick ringing at D1, the
    boost on F#1 played from MIDI, the cut matched to the hit, the Focus
    window around it. With entry, the cut page with its Depth being typed.
*/
int writeSnapshot(const juce::File& file, bool entry)
{
    RootnoteProcessor processor;
    prepare(processor);
    processor.loadPreset(2);
    set(processor, ids::midiMode, 1.0f);
    set(processor, ids::boostNote, 30.0f);
    set(processor, ids::boostGain, 9.0f);
    set(processor, ids::boostQ, 12.0f);
    set(processor, ids::bands, 8.0f);
    set(processor, ids::range, 2.0f);
    set(processor, ids::ring, 25.0f);
    set(processor, ids::glide, 40.0f);
    set(processor, ids::key, 6.0f);
    set(processor, ids::scale, static_cast<float>(Scale::minor));
    set(processor, ids::keyLock, 1.0f);

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
    if (entry)
    {
        rootnoteEditor->selectPage(1);
        auto* screen = findFirst<rootnote::ui::Screen>(*editor);
        screen->showEntry(0);
        findFirst<juce::TextEditor>(*screen)->setText("-3.1db");
    }
    // At twice the size, as the mockup is rendered.
    const juce::Image image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 2.0f);
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


/** "--snapshot[-entry] <file.png> [copy.png]" renders once and optionally copies the PNG. */
int main(int argc, char** argv)
{
#if JUCE_LINUX
    x11::defineWindowManagerAtoms();
#endif
    const juce::ScopedJuceInitialiser_GUI gui;
    if ((argc == 3 || argc == 4) && juce::String(argv[1]).startsWith("--snapshot"))
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[2]));
        const int result = writeSnapshot(file, juce::String(argv[1]) == "--snapshot-entry");
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
