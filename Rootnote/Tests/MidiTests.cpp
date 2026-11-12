#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/Engine.h"
#include "dsp/NoteControl.h"

#include <vector>

using namespace rootnote;
using namespace testsignals;

namespace
{
MidiEvent on(int channel, int note) { return { MidiEvent::Type::noteOn, channel, note }; }
MidiEvent off(int channel, int note) { return { MidiEvent::Type::noteOff, channel, note }; }
MidiEvent allOff(int channel) { return { MidiEvent::Type::allNotesOff, channel, 0 }; }

NoteControl::Config config(MidiMode mode = MidiMode::channels12, NoteOffMode noteOff = NoteOffMode::hold)
{
    NoteControl::Config c;
    c.mode = mode;
    c.noteOff = noteOff;
    c.boostKnobNote = 36.0;
    c.cutKnobNote = 38.0;
    return c;
}

/** Runs the engine on silence for a number of samples. */
void advance(Engine& engine, int samples)
{
    std::vector<float> silence(static_cast<size_t>(samples), 0.0f);
    float* channels[1] = { silence.data() };
    engine.process(channels, 1, samples);
}

double boostNote(const Engine& engine, double a4 = 440.0)
{
    return hzToNote(engine.boostHz(), a4);
}
} // namespace

TEST_CASE("MIDI: channel 1 plays the boost note, channel 2 the cut note")
{
    NoteControl control;
    control.setConfig(config());
    CHECK(control.note(NoteControl::boost) == 36.0);
    CHECK(control.note(NoteControl::cut) == 38.0);
    CHECK(!control.midiOwns(NoteControl::boost));

    CHECK(control.handleEvent(on(1, 43)) == NoteControl::boostChanged);
    CHECK(control.note(NoteControl::boost) == 43.0);
    CHECK(control.note(NoteControl::cut) == 38.0);
    CHECK(control.midiOwns(NoteControl::boost) && !control.midiOwns(NoteControl::cut));

    CHECK(control.handleEvent(on(2, 31)) == NoteControl::cutChanged);
    CHECK(control.note(NoteControl::cut) == 31.0);
    CHECK(control.note(NoteControl::boost) == 43.0);

    // Other channels do nothing.
    for (int channel = 3; channel <= 16; ++channel)
        CHECK(control.handleEvent(on(channel, 50)) == 0u);
    CHECK(control.note(NoteControl::boost) == 43.0);
    CHECK(control.note(NoteControl::cut) == 31.0);
    CHECK(control.noteOnCount() == 2u);

    // Playing the note that already sounds reports no change.
    CHECK(control.handleEvent(on(1, 43)) == 0u);
}

TEST_CASE("MIDI: omni sends every channel to boost, off ignores everything")
{
    NoteControl control;
    control.setConfig(config(MidiMode::omniBoost));
    for (int channel = 1; channel <= 16; ++channel)
    {
        CHECK(control.handleEvent(on(channel, 30 + channel)) == NoteControl::boostChanged);
        CHECK(control.note(NoteControl::boost) == 30.0 + channel);
    }
    CHECK(control.note(NoteControl::cut) == 38.0);
    CHECK(!control.midiOwns(NoteControl::cut));

    NoteControl silent;
    silent.setConfig(config(MidiMode::off));
    for (int channel = 1; channel <= 16; ++channel)
        CHECK(silent.handleEvent(on(channel, 50)) == 0u);
    CHECK(silent.note(NoteControl::boost) == 36.0);
    CHECK(silent.noteOnCount() == 0u);

    // Switching MIDI off hands both notes back to the knobs.
    NoteControl both;
    both.setConfig(config());
    both.handleEvent(on(1, 50));
    both.handleEvent(on(2, 52));
    both.setConfig(config(MidiMode::off));
    CHECK(both.note(NoteControl::boost) == 36.0);
    CHECK(both.note(NoteControl::cut) == 38.0);

    // Switching to omni releases the cut note only.
    NoteControl toOmni;
    toOmni.setConfig(config());
    toOmni.handleEvent(on(1, 50));
    toOmni.handleEvent(on(2, 52));
    toOmni.setConfig(config(MidiMode::omniBoost));
    CHECK(toOmni.note(NoteControl::boost) == 50.0);
    CHECK(toOmni.note(NoteControl::cut) == 38.0);
}

TEST_CASE("MIDI: a held key is a channel and a note, so omni keeps channels apart")
{
    // The same note on two channels: releasing one leaves the other held.
    NoteControl control;
    control.setConfig(config(MidiMode::omniBoost, NoteOffMode::returnToKnob));
    control.handleEvent(on(1, 60));
    control.handleEvent(on(2, 60));
    CHECK(control.heldCount(NoteControl::boost) == 2);
    CHECK(control.handleEvent(off(1, 60)) == 0u);
    CHECK(control.midiOwns(NoteControl::boost));
    CHECK(control.note(NoteControl::boost) == 60.0);
    CHECK(control.handleEvent(off(2, 60)) == NoteControl::boostChanged);
    CHECK(control.note(NoteControl::boost) == 36.0);

    // All-notes-off on one channel releases that channel's keys only.
    control.handleEvent(on(3, 50));
    control.handleEvent(on(4, 55));
    control.handleEvent(allOff(4));
    CHECK(control.heldCount(NoteControl::boost) == 1);
    CHECK(control.note(NoteControl::boost) == 50.0);
    control.handleEvent(allOff(3));
    CHECK(!control.midiOwns(NoteControl::boost));

    // Back to Ch 1+2: keys of the other channels could never be released again, so they go.
    control.handleEvent(on(1, 40));
    control.handleEvent(on(5, 45));
    control.setConfig(config(MidiMode::channels12, NoteOffMode::returnToKnob));
    CHECK(control.heldCount(NoteControl::boost) == 1);
    CHECK(control.note(NoteControl::boost) == 40.0);
    control.handleEvent(off(1, 40));
    CHECK(control.note(NoteControl::boost) == 36.0);

    // Hold cannot keep a note from a channel the new routing ignores.
    NoteControl held;
    held.setConfig(config(MidiMode::omniBoost, NoteOffMode::hold));
    held.handleEvent(on(5, 60));
    held.handleEvent(off(5, 60));
    held.setConfig(config(MidiMode::channels12, NoteOffMode::hold));
    CHECK(!held.midiOwns(NoteControl::boost));

    // Every key of every channel fits.
    NoteControl full;
    full.setConfig(config(MidiMode::omniBoost, NoteOffMode::returnToKnob));
    for (int channel = 1; channel <= 16; ++channel)
        for (int note = 0; note < 128; ++note)
            full.handleEvent(on(channel, note));
    CHECK(full.heldCount(NoteControl::boost) == 16 * 128);
    for (int channel = 1; channel <= 16; ++channel)
        full.handleEvent(allOff(channel));
    CHECK(full.heldCount(NoteControl::boost) == 0);
    CHECK(!full.midiOwns(NoteControl::boost));
}

TEST_CASE("MIDI: mono, last-note priority")
{
    NoteControl control;
    control.setConfig(config());
    control.handleEvent(on(1, 40));
    control.handleEvent(on(1, 43));
    control.handleEvent(on(1, 47));
    CHECK(control.note(NoteControl::boost) == 47.0);
    CHECK(control.heldCount(NoteControl::boost) == 3);

    // Releasing a note that is not sounding changes nothing.
    CHECK(control.handleEvent(off(1, 43)) == 0u);
    CHECK(control.note(NoteControl::boost) == 47.0);

    // Releasing the sounding note falls back to the most recent key still down.
    CHECK(control.handleEvent(off(1, 47)) == NoteControl::boostChanged);
    CHECK(control.note(NoteControl::boost) == 40.0);

    // Pressing a held key again moves it to the top.
    control.handleEvent(on(1, 45));
    control.handleEvent(on(1, 40));
    CHECK(control.note(NoteControl::boost) == 40.0);
    control.handleEvent(off(1, 40));
    CHECK(control.note(NoteControl::boost) == 45.0);
}

TEST_CASE("MIDI: note-off holds the note by default, or returns to the knob")
{
    NoteControl hold;
    hold.setConfig(config(MidiMode::channels12, NoteOffMode::hold));
    hold.handleEvent(on(1, 45));
    CHECK(hold.handleEvent(off(1, 45)) == 0u);
    CHECK(hold.note(NoteControl::boost) == 45.0);
    CHECK(hold.midiOwns(NoteControl::boost));

    NoteControl back;
    back.setConfig(config(MidiMode::channels12, NoteOffMode::returnToKnob));
    back.handleEvent(on(1, 45));
    back.handleEvent(on(1, 48));
    CHECK(back.handleEvent(off(1, 48)) == NoteControl::boostChanged);
    CHECK(back.note(NoteControl::boost) == 45.0); // another key is still down
    CHECK(back.handleEvent(off(1, 45)) == NoteControl::boostChanged);
    CHECK(back.note(NoteControl::boost) == 36.0);
    CHECK(!back.midiOwns(NoteControl::boost));

    // Changing the mode to Return while a released note is held lets it go.
    hold.setConfig(config(MidiMode::channels12, NoteOffMode::returnToKnob));
    CHECK(hold.note(NoteControl::boost) == 36.0);

    // All-notes-off behaves like releasing every key.
    NoteControl panic;
    panic.setConfig(config(MidiMode::channels12, NoteOffMode::returnToKnob));
    panic.handleEvent(on(1, 45));
    panic.handleEvent(on(1, 50));
    CHECK(panic.handleEvent(allOff(1)) == NoteControl::boostChanged);
    CHECK(panic.note(NoteControl::boost) == 36.0);
    CHECK(panic.heldCount(NoteControl::boost) == 0);
}

TEST_CASE("MIDI: the most recent source wins between knob and keyboard")
{
    NoteControl control;
    NoteControl::Config c = config();
    control.setConfig(c);
    control.handleEvent(on(1, 45));
    CHECK(control.note(NoteControl::boost) == 45.0);

    // Same knob value again: MIDI keeps the note.
    control.setConfig(c);
    CHECK(control.note(NoteControl::boost) == 45.0);

    // The knob (or automation) moves: it takes over, even with the key still down.
    c.boostKnobNote = 41.5;
    control.setConfig(c);
    CHECK(control.note(NoteControl::boost) == 41.5);
    CHECK(control.handleEvent(off(1, 45)) == 0u);
    CHECK(control.note(NoteControl::boost) == 41.5);

    // The next key takes it back.
    control.handleEvent(on(1, 50));
    CHECK(control.note(NoteControl::boost) == 50.0);
}

TEST_CASE("MIDI: key lock snaps played notes to the scale, notes are folded into range")
{
    NoteControl control;
    NoteControl::Config c = config();
    c.keyLock = true;
    c.key = 0;
    c.scale = Scale::major;
    control.setConfig(c);

    control.handleEvent(on(1, 61)); // C#4 -> C4
    CHECK(control.note(NoteControl::boost) == 60.0);
    control.handleEvent(on(1, 66)); // F#4 -> F4
    CHECK(control.note(NoteControl::boost) == 65.0);
    control.handleEvent(on(1, 64)); // E4 is in the scale
    CHECK(control.note(NoteControl::boost) == 64.0);

    // Knob notes are not snapped: automation may glide between semitones.
    CHECK(control.note(NoteControl::cut) == 38.0);
    c.cutKnobNote = 37.37;
    control.setConfig(c);
    CHECK(control.note(NoteControl::cut) == 37.37);

    // Unlocking shows the played note again; changing the key re-snaps.
    control.handleEvent(on(1, 61));
    c.keyLock = false;
    control.setConfig(c);
    CHECK(control.note(NoteControl::boost) == 61.0);
    c.keyLock = true;
    c.key = 1; // C# major contains C#
    control.setConfig(c);
    CHECK(control.note(NoteControl::boost) == 61.0);

    // Outside C0..C7 the note moves by octaves.
    NoteControl range;
    range.setConfig(config());
    range.handleEvent(on(1, 100));
    CHECK(range.note(NoteControl::boost) == 88.0);
    range.handleEvent(on(1, 5));
    CHECK(range.note(NoteControl::boost) == 17.0);
    range.handleEvent(on(1, 127));
    CHECK(range.note(NoteControl::boost) == 91.0);
}

TEST_CASE("MIDI in the engine: the change starts on the exact sample of the event")
{
    const double fs = 48000.0;
    EngineParams params;
    params.boostGainDb = 12.0;
    params.boostQ = 20.0;
    params.limiter = false;

    Random random(21);
    std::vector<float> input(4800);
    for (auto& x : input)
        x = static_cast<float>(0.3 * random.bipolar());

    for (int eventAt : { 0, 1, 37, 255, 256, 1000 })
    {
        std::vector<float> reference = input;
        std::vector<float> played = input;

        Engine quiet, moved;
        quiet.prepare(fs, 512);
        moved.prepare(fs, 512);
        quiet.setParams(params);
        moved.setParams(params);

        float* a[1] = { reference.data() };
        quiet.process(a, 1, static_cast<int>(reference.size()));

        float* b[1] = { played.data() };
        moved.process(b, 1, eventAt);
        moved.handleMidi(on(1, 60));
        float* rest[1] = { played.data() + eventAt };
        moved.process(rest, 1, static_cast<int>(played.size()) - eventAt);

        int firstDifference = -1;
        for (size_t i = 0; i < played.size() && firstDifference < 0; ++i)
            if (played[i] != reference[i])
                firstDifference = static_cast<int>(i);

        // Identical up to the event, different from the event on. (The very
        // first samples of a ramp differ by less than float resolution, so
        // the first visible difference may come a few samples later.)
        CHECK(firstDifference >= eventAt);
        CHECK(firstDifference <= eventAt + 16);
        CHECK(moved.boostTargetNote() == 60.0);
        CHECK(moved.boostFollowsMidi());
    }
}

TEST_CASE("MIDI in the engine: glide is linear in pitch and takes the Glide time")
{
    const double fs = 48000.0;
    Engine engine;
    engine.prepare(fs, 512);
    EngineParams params;
    params.glideSeconds = 0.100;
    engine.setParams(params);
    advance(engine, 512);
    CHECK_NEAR(boostNote(engine), 36.0, 1.0e-9);

    engine.handleMidi(on(1, 60)); // two octaves up
    advance(engine, 1200);        // 25 ms
    CHECK_NEAR(boostNote(engine), 42.0, 0.1);
    advance(engine, 1200);        // 50 ms
    CHECK_NEAR(boostNote(engine), 48.0, 0.1);
    advance(engine, 2400 - 32);   // just before 100 ms
    CHECK(boostNote(engine) < 60.0);
    CHECK(boostNote(engine) > 59.7);
    advance(engine, 32 + 16);
    CHECK_NEAR(boostNote(engine), 60.0, 1.0e-9);

    // A new note during a glide starts from where the pitch is.
    engine.handleMidi(on(1, 36));
    advance(engine, 2400);
    CHECK_NEAR(boostNote(engine), 48.0, 0.1);
    engine.handleMidi(on(1, 48));
    advance(engine, 4800 + 16);
    CHECK_NEAR(boostNote(engine), 48.0, 1.0e-9);

    // Channel 2 glides the cut note the same way.
    engine.handleMidi(on(2, 48));
    advance(engine, 2400);
    CHECK_NEAR(hzToNote(engine.cutHz()), 42.0, 0.1);
    advance(engine, 2400 + 16);
    CHECK_NEAR(hzToNote(engine.cutHz()), 48.0, 1.0e-9);
}

TEST_CASE("MIDI in the engine: Glide 0 still moves over 5 ms, never as a jump")
{
    const double fs = 48000.0;
    Engine engine;
    engine.prepare(fs, 512);
    EngineParams params;
    params.glideSeconds = 0.0;
    engine.setParams(params);
    advance(engine, 512);

    engine.handleMidi(on(1, 48));
    advance(engine, 120); // 2.5 ms
    CHECK_NEAR(boostNote(engine), 42.0, 1.0);
    advance(engine, 120 + 16);
    CHECK_NEAR(boostNote(engine), 48.0, 1.0e-9);
}

TEST_CASE("MIDI in the engine: note-off Return glides back to the knob, fine tune and A4 apply")
{
    const double fs = 48000.0;
    Engine engine;
    engine.prepare(fs, 512);
    EngineParams params;
    params.noteOff = NoteOffMode::returnToKnob;
    params.glideSeconds = 0.050;
    params.boostFineCents = 25.0;
    params.a4 = 432.0;
    engine.setParams(params);
    advance(engine, 512);
    CHECK_NEAR(engine.boostHz(), noteToHz(36.25, 432.0), 1.0e-9);

    engine.handleMidi(on(1, 43));
    advance(engine, 2400 + 16);
    CHECK_NEAR(engine.boostHz(), noteToHz(43.25, 432.0), 1.0e-9);

    engine.handleMidi(off(1, 43));
    CHECK(!engine.boostFollowsMidi());
    advance(engine, 1200);
    CHECK_NEAR(hzToNote(engine.boostHz(), 432.0), 39.75, 0.1);
    advance(engine, 1200 + 16);
    CHECK_NEAR(engine.boostHz(), noteToHz(36.25, 432.0), 1.0e-9);

    // Knob moves use the 30 ms parameter ramp, not the glide time.
    params.boostNote = 48.0;
    engine.setParams(params);
    advance(engine, 720); // 15 ms: half way
    CHECK_NEAR(hzToNote(engine.boostHz(), 432.0), 42.25, 0.15);
    advance(engine, 720 + 16);
    CHECK_NEAR(engine.boostHz(), noteToHz(48.25, 432.0), 1.0e-9);
}

TEST_CASE("MIDI in the engine: key lock and MIDI mode from the parameters")
{
    const double fs = 48000.0;
    Engine engine;
    engine.prepare(fs, 512);
    EngineParams params;
    params.keyLock = true;
    params.key = 6;              // F#
    params.scale = Scale::minor; // F# G# A B C# D E
    engine.setParams(params);

    engine.handleMidi(on(1, 60)); // C -> B (C# is equally close: ties go down)
    CHECK(engine.boostTargetNote() == 59.0);
    engine.handleMidi(on(1, 63)); // D# -> D
    CHECK(engine.boostTargetNote() == 62.0);

    params.midiMode = MidiMode::omniBoost;
    engine.setParams(params);
    engine.handleMidi(on(9, 54)); // F# on any channel
    CHECK(engine.boostTargetNote() == 54.0);
    CHECK(engine.midiNoteOnCount() == 3u);

    params.midiMode = MidiMode::off;
    engine.setParams(params);
    CHECK(engine.boostTargetNote() == 36.0);
    engine.handleMidi(on(1, 54));
    CHECK(engine.boostTargetNote() == 36.0);
}
