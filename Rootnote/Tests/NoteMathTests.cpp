#include "TestFramework.h"

#include "dsp/NoteMath.h"
#include "dsp/Scales.h"

using namespace rootnote;

TEST_CASE("NoteMath: reference pitches")
{
    CHECK_NEAR(noteToHz(69.0), 440.0, 1.0e-9);
    CHECK_NEAR(noteToHz(57.0), 220.0, 1.0e-9);
    CHECK_NEAR(noteToHz(36.0), 65.4064, 1.0e-4);   // C2, the default boost note
    CHECK_NEAR(noteToHz(30.0), 46.2493, 1.0e-4);   // F#1
    CHECK_NEAR(noteToHz(12.0), 16.3516, 1.0e-4);   // C0, bottom of the range
    CHECK_NEAR(noteToHz(96.0), 2093.0045, 1.0e-3); // C7, top of the range
    CHECK_NEAR(noteToHz(69.0, 432.0), 432.0, 1.0e-9);
    CHECK_NEAR(noteToHz(69.5), 440.0 * std::pow(2.0, 0.5 / 12.0), 1.0e-9);
}

TEST_CASE("NoteMath: note <-> Hz round trip for every A4")
{
    for (double a4 : { 415.0, 432.0, 440.0, 442.0, 466.0 })
        for (double note = kMinNote; note <= kMaxNote; note += 0.37)
            CHECK_NEAR(hzToNote(noteToHz(note, a4), a4), note, 1.0e-9);
}

TEST_CASE("NoteMath: names")
{
    CHECK(noteName(12) == "C0");
    CHECK(noteName(30) == "F#1");
    CHECK(noteName(36) == "C2");
    CHECK(noteName(60) == "C4");
    CHECK(noteName(69) == "A4");
    CHECK(noteName(96) == "C7");
    CHECK(noteName(11) == "B-1");

    CHECK(formatNote(30.0) == "F#1");
    CHECK(formatNote(30.25) == "F#1 +25 ct");
    CHECK(formatNote(29.88) == "F#1 -12 ct");
    CHECK(formatNote(30.5) == "G1 -50 ct");
    CHECK(formatNote(30.004) == "F#1");
}

TEST_CASE("NoteMath: parsing typed values")
{
    double note = 0.0;

    CHECK(parseNote("F#1", 440.0, note));
    CHECK_NEAR(note, 30.0, 1.0e-12);
    CHECK(parseNote("Gb1", 440.0, note));
    CHECK_NEAR(note, 30.0, 1.0e-12);
    CHECK(parseNote("  f#1 ", 440.0, note));
    CHECK_NEAR(note, 30.0, 1.0e-12);
    CHECK(parseNote("C2", 440.0, note));
    CHECK_NEAR(note, 36.0, 1.0e-12);
    CHECK(parseNote("c 2", 440.0, note));
    CHECK_NEAR(note, 36.0, 1.0e-12);
    CHECK(parseNote("Bb0", 440.0, note));
    CHECK_NEAR(note, 22.0, 1.0e-12);
    CHECK(parseNote("B1", 440.0, note));
    CHECK_NEAR(note, 35.0, 1.0e-12);
    CHECK(parseNote("Cb2", 440.0, note));
    CHECK_NEAR(note, 35.0, 1.0e-12);
    CHECK(parseNote("B#1", 440.0, note));
    CHECK_NEAR(note, 36.0, 1.0e-12);
    CHECK(parseNote("A-1", 440.0, note));
    CHECK_NEAR(note, 9.0, 1.0e-12);
    CHECK(parseNote("F\xE2\x99\xAF" "1", 440.0, note)); // F♯1 as typed from the readout
    CHECK_NEAR(note, 30.0, 1.0e-12);

    CHECK(parseNote("F#1 +25 ct", 440.0, note));
    CHECK_NEAR(note, 30.25, 1.0e-12);
    CHECK(parseNote("F#1 -12ct", 440.0, note));
    CHECK_NEAR(note, 29.88, 1.0e-12);
    CHECK(parseNote("D1 \xE2\x88\x92" "12 ct", 440.0, note)); // typographic minus
    CHECK_NEAR(note, 25.88, 1.0e-12);

    CHECK(parseNote("46 Hz", 440.0, note));
    CHECK_NEAR(note, hzToNote(46.0), 1.0e-12);
    CHECK(parseNote("46.25hz", 440.0, note));
    CHECK_NEAR(note, 30.0, 1.0e-3);
    CHECK(parseNote("0.44 kHz", 440.0, note));
    CHECK_NEAR(note, 69.0, 1.0e-9);
    CHECK(parseNote("432 Hz", 432.0, note));
    CHECK_NEAR(note, 69.0, 1.0e-9);
    CHECK(parseNote("46,25 Hz", 440.0, note)); // comma decimal
    CHECK_NEAR(note, 30.0, 1.0e-3);

    CHECK(parseNote("36", 440.0, note));
    CHECK_NEAR(note, 36.0, 1.0e-12);
    CHECK(parseNote("36.5", 440.0, note));
    CHECK_NEAR(note, 36.5, 1.0e-12);

    CHECK(!parseNote("", 440.0, note));
    CHECK(!parseNote("   ", 440.0, note));
    CHECK(!parseNote("H2", 440.0, note));
    CHECK(!parseNote("F#", 440.0, note));
    CHECK(!parseNote("F#1 loud", 440.0, note));
    CHECK(!parseNote("46 dB", 440.0, note));
    CHECK(!parseNote("0 Hz", 440.0, note));
    CHECK(!parseNote("-5 Hz", 440.0, note));
}

TEST_CASE("NoteMath: format and parse agree")
{
    for (double note = kMinNote; note <= kMaxNote; note += 0.01)
    {
        const double rounded = std::round(note * 100.0) / 100.0;
        double parsed = 0.0;
        REQUIRE(parseNote(formatNote(rounded), 440.0, parsed));
        CHECK_NEAR(parsed, rounded, 1.0e-9);
    }
}

TEST_CASE("NoteMath: folding MIDI notes into the parameter range")
{
    CHECK(foldNoteIntoRange(36) == 36);
    CHECK(foldNoteIntoRange(12) == 12);
    CHECK(foldNoteIntoRange(96) == 96);
    CHECK(foldNoteIntoRange(0) == 12);
    CHECK(foldNoteIntoRange(11) == 23);
    CHECK(foldNoteIntoRange(97) == 85);
    CHECK(foldNoteIntoRange(127) == 91);
    for (int note = 0; note < 128; ++note)
    {
        const int folded = foldNoteIntoRange(note);
        CHECK(folded >= 12 && folded <= 96);
        CHECK((folded - note) % 12 == 0);
    }
}

TEST_CASE("Scales: membership")
{
    const int cMajor[] = { 0, 2, 4, 5, 7, 9, 11 };
    for (int pc = 0; pc < 12; ++pc)
    {
        bool expected = false;
        for (int member : cMajor)
            expected = expected || member == pc;
        CHECK(isInScale(60 + pc, 0, Scale::major) == expected);
    }

    // F# minor: F# G# A B C# D E.
    const int fSharpMinor[] = { 6, 8, 9, 11, 1, 2, 4 };
    for (int pc = 0; pc < 12; ++pc)
    {
        bool expected = false;
        for (int member : fSharpMinor)
            expected = expected || member == pc;
        CHECK(isInScale(48 + pc, 6, Scale::minor) == expected);
    }

    for (int note = 0; note < 128; ++note)
        CHECK(isInScale(note, 3, Scale::chromatic));

    const int sizes[kNumScales] = { 12, 7, 7, 7, 7, 7, 5, 5, 7 };
    for (int s = 0; s < kNumScales; ++s)
    {
        int count = 0;
        for (int pc = 0; pc < 12; ++pc)
            count += isInScale(pc, 0, static_cast<Scale>(s)) ? 1 : 0;
        CHECK(count == sizes[s]);
        CHECK(isInScale(0, 0, static_cast<Scale>(s))); // the root is in every scale
    }

    CHECK(isInScale(60 + 3, 0, Scale::dorian) && isInScale(60 + 9, 0, Scale::dorian));
    CHECK(isInScale(60 + 1, 0, Scale::phrygian));
    CHECK(isInScale(60 + 10, 0, Scale::mixolydian) && isInScale(60 + 4, 0, Scale::mixolydian));
    CHECK(isInScale(60 + 11, 0, Scale::harmonicMinor) && isInScale(60 + 8, 0, Scale::harmonicMinor));
    CHECK(!isInScale(60 + 5, 0, Scale::pentatonicMajor));
    CHECK(isInScale(60 + 10, 0, Scale::pentatonicMinor));
}

TEST_CASE("Scales: snapping to the nearest scale note")
{
    // C major.
    CHECK(snapToScale(60.0, 0, Scale::major) == 60);
    CHECK(snapToScale(61.0, 0, Scale::major) == 60); // C# is a tie between C and D: down
    CHECK(snapToScale(63.0, 0, Scale::major) == 62); // D# -> D (tie)
    CHECK(snapToScale(66.0, 0, Scale::major) == 65); // F# -> F (tie)
    CHECK(snapToScale(61.2, 0, Scale::major) == 62); // closer to D
    CHECK(snapToScale(60.8, 0, Scale::major) == 60);
    CHECK(snapToScale(64.4, 0, Scale::major) == 64);
    CHECK(snapToScale(64.6, 0, Scale::major) == 65);

    // A minor pentatonic: A C D E G.
    CHECK(snapToScale(58.0, 9, Scale::pentatonicMinor) == 57); // A# -> A
    CHECK(snapToScale(59.0, 9, Scale::pentatonicMinor) == 60); // B -> C
    CHECK(snapToScale(66.0, 9, Scale::pentatonicMinor) == 67); // F# -> G

    // Chromatic rounds to the nearest semitone.
    CHECK(snapToScale(37.4, 0, Scale::chromatic) == 37);
    CHECK(snapToScale(37.6, 0, Scale::chromatic) == 38);

    // Every result is in the scale and never more than a tritone away.
    for (int s = 0; s < kNumScales; ++s)
        for (int key = 0; key < 12; ++key)
            for (double note = 12.0; note <= 96.0; note += 0.25)
            {
                const int snapped = snapToScale(note, key, static_cast<Scale>(s));
                CHECK(isInScale(snapped, key, static_cast<Scale>(s)));
                CHECK(std::abs(snapped - note) <= 2.0);
            }
}
