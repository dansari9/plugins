#pragma once

#include "NoteMath.h"
#include "Scales.h"

#include <cstdint>

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

    MIDI is monophonic with last-note priority per bank. A held key is known
    by its channel and its note, so the same note on two channels is two keys. The most recent
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
        MidiMode mode = MidiMode::off;
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

    /** Takes the knob notes and the MIDI settings. A knob that moved takes its bank back from MIDI. */
    void setConfig(const Config& newConfig) noexcept
    {
        const bool modeChanged = !configured || newConfig.mode != config.mode;
        if (configured)
        {
            if (newConfig.boostKnobNote != config.boostKnobNote)
                voices[boost].midiOwns = false;
            if (newConfig.cutKnobNote != config.cutKnobNote)
                voices[cut].midiOwns = false;
        }
        config = newConfig;
        configured = true;

        if (modeChanged)
        {
            // Keys the new routing would no longer release must not stay held.
            if (config.mode != MidiMode::channels12)
                voices[cut] = {};
            if (config.mode == MidiMode::off)
                voices[boost] = {};
            else if (config.mode == MidiMode::channels12)
            {
                voices[boost].removeIf([](int id) { return id / 128 != 0; });
                if (voices[boost].numHeld == 0 && voices[boost].midiChannel != 1)
                    voices[boost] = {};
            }
        }

        for (auto& voice : voices)
            released(voice);
    }

    /** The knob takes the targets (boostChanged / cutChanged) back from MIDI, moved or not. */
    void takeBack(unsigned targets) noexcept
    {
        if ((targets & boostChanged) != 0u)
            voices[boost].midiOwns = false;
        if ((targets & cutChanged) != 0u)
            voices[cut].midiOwns = false;
    }

    /** Returns a mask of the targets whose note changed. */
    unsigned handleEvent(const MidiEvent& event) noexcept
    {
        const int target = targetForChannel(event.channel);
        if (target < 0)
            return 0;

        Voice& voice = voices[target];
        const double before = note(static_cast<Target>(target));
        const int channel = (event.channel < 1 ? 1 : (event.channel > 16 ? 16 : event.channel)) - 1;
        const int midiNote = event.note < 0 ? 0 : (event.note > 127 ? 127 : event.note);
        const int id = channel * 128 + midiNote;

        switch (event.type)
        {
            case MidiEvent::Type::noteOn:
                voice.removeIf([id](int held) { return held == id; });
                voice.held[voice.numHeld++] = static_cast<uint16_t>(id);
                voice.midiNote = midiNote;
                voice.midiChannel = channel + 1;
                voice.midiOwns = true;
                ++noteOnCounter;
                break;

            case MidiEvent::Type::noteOff:
                voice.removeIf([id](int held) { return held == id; });
                released(voice);
                break;

            case MidiEvent::Type::allNotesOff:
                voice.removeIf([channel](int held) { return held / 128 == channel; });
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
    /** Every key of every channel can be held at once, so nothing is ever dropped. */
    static constexpr int kMaxHeld = 16 * 128;

    struct Voice
    {
        uint16_t held[kMaxHeld] {}; // (channel - 1) * 128 + note, oldest first
        int numHeld = 0;
        int midiNote = 60;
        int midiChannel = 1;
        bool midiOwns = false;

        template <typename Predicate>
        void removeIf(Predicate&& matches) noexcept
        {
            int write = 0;
            for (int read = 0; read < numHeld; ++read)
                if (!matches(static_cast<int>(held[read])))
                    held[write++] = held[read];
            numHeld = write;
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

    /** After keys went up: the newest key still held plays, or the note stays or returns to the knob. */
    void released(Voice& voice) const noexcept
    {
        if (!voice.midiOwns)
            return;
        if (voice.numHeld > 0)
        {
            voice.midiNote = voice.held[voice.numHeld - 1] % 128;
            voice.midiChannel = voice.held[voice.numHeld - 1] / 128 + 1;
        }
        else if (config.noteOff == NoteOffMode::returnToKnob)
            voice.midiOwns = false;
    }

    Voice voices[2];
    Config config;
    unsigned noteOnCounter = 0;
    bool configured = false;
};
} // namespace rootnote
