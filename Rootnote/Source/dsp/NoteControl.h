#pragma once

#include "NoteMath.h"
#include "Scales.h"

namespace rootnote
{
enum class MidiMode
{
    off = 0,
    channels12, // channel 1 plays the boost note, channel 2 the cut note
    omniBoost   // every channel plays the boost note
};

enum class NoteOffMode
{
    hold = 0,     // the last MIDI note stays
    returnToKnob  // the note goes back to the knob value when the last key is released
};

struct MidiEvent
{
    enum class Type { noteOn, noteOff, allNotesOff };
    Type type = Type::noteOn;
    int channel = 1; // 1..16
    int note = 60;   // 0..127
};

/**
    Decides which note each bank plays: the knob (host parameter) or MIDI.

    MIDI is monophonic with last-note priority per bank. The most recent
    source wins: a MIDI note takes over from the knob, and moving the knob (or
    automation) takes over from a held MIDI note. MIDI never writes the host
    parameter. No allocation; audio-thread safe.
*/
class NoteControl
{
public:
    enum Target { boost = 0, cut = 1 };
    enum : unsigned { boostChanged = 1u, cutChanged = 2u };

    struct Config
    {
        MidiMode mode = MidiMode::channels12;
        NoteOffMode noteOff = NoteOffMode::hold;
        bool keyLock = false;
        int key = 0;
        Scale scale = Scale::chromatic;
        double boostKnobNote = 36.0;
        double cutKnobNote = 36.0;
    };

    void reset() noexcept
    {
        voices[boost] = {};
        voices[cut] = {};
        configured = false;
    }

    /** Returns a mask of the targets whose note changed. */
    unsigned setConfig(const Config& newConfig) noexcept
    {
        const double before[2] = { note(boost), note(cut) };
        const bool first = !configured;

        if (configured)
        {
            if (newConfig.boostKnobNote != config.boostKnobNote)
                voices[boost].midiOwns = false;
            if (newConfig.cutKnobNote != config.cutKnobNote)
                voices[cut].midiOwns = false;
        }
        config = newConfig;
        configured = true;

        if (config.mode == MidiMode::off)
        {
            voices[boost] = {};
            voices[cut] = {};
        }
        else if (config.mode == MidiMode::omniBoost)
        {
            voices[cut] = {};
        }

        if (config.noteOff == NoteOffMode::returnToKnob)
            for (auto& voice : voices)
                if (voice.numHeld == 0)
                    voice.midiOwns = false;

        unsigned changed = 0;
        if (first || note(boost) != before[boost])
            changed |= boostChanged;
        if (first || note(cut) != before[cut])
            changed |= cutChanged;
        return changed;
    }

    /** Returns a mask of the targets whose note changed. */
    unsigned handleEvent(const MidiEvent& event) noexcept
    {
        const int target = targetForChannel(event.channel);
        if (target < 0)
            return 0;

        Voice& voice = voices[target];
        const double before = note(static_cast<Target>(target));
        const int midiNote = event.note < 0 ? 0 : (event.note > 127 ? 127 : event.note);

        switch (event.type)
        {
            case MidiEvent::Type::noteOn:
                voice.remove(midiNote);
                voice.push(midiNote);
                voice.midiNote = midiNote;
                voice.midiOwns = true;
                ++noteOnCounter;
                break;

            case MidiEvent::Type::noteOff:
                voice.remove(midiNote);
                released(voice);
                break;

            case MidiEvent::Type::allNotesOff:
                voice.numHeld = 0;
                released(voice);
                break;
        }

        return note(static_cast<Target>(target)) != before ? (target == boost ? boostChanged : cutChanged) : 0u;
    }

    /** The note the bank should play now: folded into range and, with key lock, snapped to the scale. */
    double note(Target target) const noexcept
    {
        const Voice& voice = voices[target];
        if (!voice.midiOwns)
            return target == boost ? config.boostKnobNote : config.cutKnobNote;

        int resolved = foldNoteIntoRange(voice.midiNote);
        if (config.keyLock)
            resolved = foldNoteIntoRange(snapToScale(static_cast<double>(resolved), config.key, config.scale));
        return static_cast<double>(resolved);
    }

    bool midiOwns(Target target) const noexcept { return voices[target].midiOwns; }
    int heldCount(Target target) const noexcept { return voices[target].numHeld; }
    unsigned noteOnCount() const noexcept { return noteOnCounter; }

private:
    struct Voice
    {
        unsigned char held[128] {};
        int numHeld = 0;
        int midiNote = 60;
        bool midiOwns = false;

        void remove(int n) noexcept
        {
            int write = 0;
            for (int read = 0; read < numHeld; ++read)
                if (held[read] != static_cast<unsigned char>(n))
                    held[write++] = held[read];
            numHeld = write;
        }

        void push(int n) noexcept
        {
            if (numHeld < 128)
                held[numHeld++] = static_cast<unsigned char>(n);
        }
    };

    int targetForChannel(int channel) const noexcept
    {
        switch (config.mode)
        {
            case MidiMode::off:        return -1;
            case MidiMode::channels12: return channel == 1 ? boost : (channel == 2 ? cut : -1);
            case MidiMode::omniBoost:  return boost;
        }
        return -1;
    }

    void released(Voice& voice) const noexcept
    {
        if (!voice.midiOwns)
            return;
        if (voice.numHeld > 0)
            voice.midiNote = voice.held[voice.numHeld - 1];
        else if (config.noteOff == NoteOffMode::returnToKnob)
            voice.midiOwns = false;
    }

    Voice voices[2];
    Config config;
    unsigned noteOnCounter = 0;
    bool configured = false;
};
} // namespace rootnote
