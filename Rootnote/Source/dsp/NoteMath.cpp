#include "NoteMath.h"

#include <cctype>

namespace rootnote
{
namespace
{
const char* const kNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

void skipSpaces(const std::string& s, size_t& i)
{
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0)
        ++i;
}

// Locale-independent on purpose: a host may have switched LC_NUMERIC to a
// comma decimal, which would make strtod stop at the dot.
bool readNumber(const std::string& s, size_t& i, double& value)
{
    size_t at = i;
    double sign = 1.0;
    if (at < s.size() && (s[at] == '+' || s[at] == '-'))
    {
        sign = s[at] == '-' ? -1.0 : 1.0;
        ++at;
    }

    double whole = 0.0;
    double fraction = 0.0;
    double scale = 1.0;
    int digits = 0;
    for (; at < s.size() && std::isdigit(static_cast<unsigned char>(s[at])) != 0; ++at, ++digits)
        whole = whole * 10.0 + static_cast<double>(s[at] - '0');

    if (at < s.size() && (s[at] == '.' || s[at] == ','))
    {
        ++at;
        for (; at < s.size() && std::isdigit(static_cast<unsigned char>(s[at])) != 0; ++at, ++digits)
        {
            scale *= 0.1;
            fraction += static_cast<double>(s[at] - '0') * scale;
        }
    }

    if (digits == 0 || digits > 18)
        return false;

    value = sign * (whole + fraction);
    i = at;
    return true;
}

std::string lowered(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    // UTF-8 for U+2212 (minus), U+266F (sharp) and U+266D (flat), which host
    // text fields and copied note names can contain.
    const struct { const char* from; const char* to; } replacements[] = {
        { "\xE2\x88\x92", "-" }, { "\xE2\x99\xAF", "#" }, { "\xE2\x99\xAD", "b" }
    };
    for (const auto& r : replacements)
    {
        const std::string from(r.from);
        for (size_t at = out.find(from); at != std::string::npos; at = out.find(from, at))
            out.replace(at, from.size(), r.to);
    }
    return out;
}
} // namespace

std::string noteName(int midiNote)
{
    const int pitchClass = ((midiNote % 12) + 12) % 12;
    const int octave = (midiNote - pitchClass) / 12 - 1;
    return std::string(kNames[pitchClass]) + std::to_string(octave);
}

std::string formatNote(double note)
{
    int nearest = static_cast<int>(std::lround(note));
    int cents = static_cast<int>(std::lround((note - nearest) * 100.0));
    if (cents == 50)
    {
        ++nearest;
        cents = -50;
    }

    std::string text = noteName(nearest);
    if (cents != 0)
        text += (cents > 0 ? " +" : " ") + std::to_string(cents) + " ct";
    return text;
}

bool parseNote(const std::string& rawText, double a4, double& noteOut)
{
    const std::string s = lowered(rawText);
    size_t i = 0;
    skipSpaces(s, i);
    if (i >= s.size())
        return false;

    const char first = s[i];
    if (first >= 'a' && first <= 'g')
    {
        static const int semitoneOf[7] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
        int pitchClass = semitoneOf[first - 'a'];
        ++i;

        while (i < s.size() && (s[i] == '#' || s[i] == 'b'))
        {
            pitchClass += s[i] == '#' ? 1 : -1;
            ++i;
        }

        skipSpaces(s, i);
        double octave = 0.0;
        if (!readNumber(s, i, octave) || octave != std::floor(octave) || octave < -2.0 || octave > 10.0)
            return false;

        double note = (octave + 1.0) * 12.0 + pitchClass;

        skipSpaces(s, i);
        if (i < s.size())
        {
            double cents = 0.0;
            if (!readNumber(s, i, cents))
                return false;
            skipSpaces(s, i);
            const std::string unit = s.substr(i);
            if (!(unit.empty() || unit == "ct" || unit == "c" || unit == "cent" || unit == "cents"))
                return false;
            note += cents / 100.0;
        }

        noteOut = note;
        return true;
    }

    double number = 0.0;
    if (!readNumber(s, i, number))
        return false;
    skipSpaces(s, i);
    const std::string unit = s.substr(i);

    if (unit == "hz" || unit == "khz")
    {
        const double hz = unit == "khz" ? number * 1000.0 : number;
        if (hz <= 0.0)
            return false;
        noteOut = hzToNote(hz, a4);
        return true;
    }

    if (!unit.empty())
        return false;

    noteOut = number;
    return true;
}
} // namespace rootnote
