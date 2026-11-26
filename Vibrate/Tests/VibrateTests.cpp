#include "Vibrate.h"
#include "Lanes.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cstdlib>
#include <chrono>
#include <complex>
#include <cstring>
#include <limits>
#include <numeric>
#include <iostream>
#include <random>
#include <sstream>

// Real processor, interleaved stereo f32, 128 samples per block as on the site.
static int renderFile(const juce::StringArray& args)
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(args[1]);
    juce::MemoryBlock input;
    if (!file.loadFileAsData(input) || input.getSize() == 0 || input.getSize() % (2 * sizeof(float)) != 0)
        return 1;
    const bool frames = args[0] == "--frames";
    juce::MemoryInputStream in(input, false);
    juce::MemoryOutputStream out;
    VibrateProcessor processor;
    double rate = 48000.0;
    juce::RangedAudioParameter* sweep = nullptr;
    for (int i = 3; i < args.size(); ++i)
    {
        const auto id = args[i].upToFirstOccurrenceOf("=", false, false);
        const auto value = args[i].fromFirstOccurrenceOf("=", false, false);
        if (id == "rate_hz")
            rate = value.getDoubleValue();
        else if (id == "preset")
            processor.setCurrentProgram(value.getIntValue());
        else if (id == "sweep")
        {
            sweep = processor.apvts.getParameter(value);
            if (sweep == nullptr || value == "bypass") return 1;
        }
        else if (auto* param = processor.apvts.getParameter(id))
            param->setValueNotifyingHost(param->convertTo0to1(value.getFloatValue()));
        else
        {
            std::cerr << "Unknown render option " << id << '\n';
            return 1;
        }
    }
    if (!std::isfinite(rate) || rate < 44100.0 || rate > 192000.0)
        return 1;
    processor.prepareToPlay(rate, 128);
    juce::AudioBuffer<float> buffer(2, 128);
    juce::MidiBuffer midi;
    int remaining = static_cast<int>(input.getSize() / (2 * sizeof(float)));
    std::ostringstream frameText;
    frameText.imbue(std::locale::classic());
    frameText.precision(17);
    for (int b = 0; remaining > 0; ++b)
    {
        if (sweep != nullptr && b > 0 && b % 12 == 0)
        {
            const int step = (b / 12 - 1) % 20;
            sweep->setValueNotifyingHost(static_cast<float>(step <= 10 ? step : 20 - step) / 10.0f);
        }
        const int count = std::min(128, remaining);
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), 2, count);
        for (int i = 0; i < count; ++i)
            for (int channel = 0; channel < 2; ++channel)
                block.setSample(channel, i, in.readFloat());
        processor.processBlock(block, midi);
        if (frames)
        {
            vibrate::Frame frame;
            while (processor.feed.pop(frame))
            {
                frameText << '[' << frame.sequence << ',' << frame.enabled;
                for (auto x : frame.pitchStep) frameText << ',' << x;
                for (auto x : frame.phaseHz) frameText << ',' << x;
                frameText << ',' << frame.panGain << ',' << frame.gapSeconds;
                for (auto x : frame.energy) frameText << ',' << x;
                frameText << ',' << frame.midEnergy << ',' << frame.leftEnergy << ',' << frame.rightEnergy << ',' << frame.cross
                          << ',' << frame.feedEnergy << ',' << frame.midLow << ',' << frame.midHigh << ',' << frame.mixGain << "]\n";
            }
        }
        else
            for (int i = 0; i < count; ++i)
                for (int channel = 0; channel < 2; ++channel)
                    out.writeFloat(block.getSample(channel, i));
        remaining -= count;
    }
    const auto output = juce::File::getCurrentWorkingDirectory().getChildFile(args[2]);
    if (frames)
    {
        const auto data = frameText.str();
        return output.replaceWithData(data.data(), data.size()) ? 0 : 1;
    }
    return output.replaceWithData(out.getData(), out.getDataSize()) ? 0 : 1;
}

namespace
{
using vibrate::Controls;
using vibrate::Engine;
using vibrate::Stereo;

constexpr std::array<double, 5> sampleRates { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
constexpr Controls full { 1, 1, 1, 1, 1, 1 };
int failures = 0;

void check(bool pass, const char* name)
{
    std::cout << (pass ? "PASS " : "FAIL ") << name << '\n';
    failures += pass ? 0 : 1;
}

bool same(float a, float b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
bool same(Stereo a, Stereo b) { return same(a.left, b.left) && same(a.right, b.right); }

void set(VibrateProcessor& processor, std::size_t index, double normal)
{
    auto* param = processor.apvts.getParameter(vibrate::ids[index]);
    param->setValueNotifyingHost(static_cast<float>(normal));
}

std::vector<Stereo> noise(int count)
{
    std::mt19937 rng(0x91de7u);
    auto sample = [&] { return static_cast<float>(static_cast<double>(rng()) * (2.0 / 4294967296.0) - 1.0); };
    std::vector<Stereo> audio(static_cast<std::size_t>(count));
    for (auto& x : audio) x = { sample(), sample() };
    audio[0] = { -0.0f, -0.0f };
    audio[1] = { 0.0f, -0.0f };
    return audio;
}

std::vector<Stereo> runProcessor(const std::vector<Stereo>& input, const Controls& controls,
                                 double rate, int channels, int blockSize, bool reset = false)
{
    VibrateProcessor p;
    p.setChannelLayoutOfBus(true, 0, channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
    for (std::size_t i = 0; i < controls.size(); ++i) set(p, i, controls[i]);
    p.prepareToPlay(rate, 4096);
    juce::AudioBuffer<float> storage(2, 4096);
    juce::MidiBuffer midi;
    std::vector<Stereo> output;
    output.reserve(input.size());
    std::mt19937 rng(325u);
    auto render = [&] {
        output.clear();
        for (std::size_t offset = 0; offset < input.size();)
        {
            const int size = blockSize == 0 ? 1 + static_cast<int>(rng() % 4096u) : blockSize;
            const auto count = std::min(static_cast<std::size_t>(size), input.size() - offset);
            juce::AudioBuffer<float> block(storage.getArrayOfWritePointers(), 2, static_cast<int>(count));
            for (std::size_t i = 0; i < count; ++i)
            {
                block.setSample(0, static_cast<int>(i), input[offset + i].left);
                block.setSample(1, static_cast<int>(i), input[offset + i].right);
            }
            p.processBlock(block, midi);
            for (std::size_t i = 0; i < count; ++i)
                output.push_back({ block.getSample(0, static_cast<int>(i)), block.getSample(1, static_cast<int>(i)) });
            offset += count;
        }
    };
    render();
    if (reset)
    {
        p.reset();
        render();
    }
    return output;
}

void testNull()
{
    const auto in = noise(12517);
    bool ok = true;
    for (double rate : sampleRates)
        for (int channels : { 1, 2 })
            for (auto controls : { Controls { 0, 0, 0, 0, 0, 0.73 }, Controls { 1, 1, 1, 1, 1, 0 } })
            {
                const auto out = runProcessor(in, controls, rate, channels, 0);
                for (std::size_t i = 0; i < in.size(); ++i)
                    ok &= same(out[i], { in[i].left, channels == 1 ? in[i].left : in[i].right });
            }
    check(ok, "all-zero depths and zero mix null bit-exactly including signed zero, mono and stereo, five rates");
}

template <unsigned lane>
bool testMask(const std::vector<Stereo>& input)
{
    Engine zero, masked;
    std::mt19937 rng(7515u);
    bool ok = true;
    for (int setting = 0; setting < 50; ++setting)
    {
        Controls controls;
        for (auto& c : controls) c = static_cast<double>(rng() % 1001u) / 1000.0;
        masked.prepare(48000, controls);
        controls[lane] = 0.0;
        zero.prepare(48000, controls);
        for (const auto& x : input)
            ok &= same(zero.process(x.left, x.right), masked.process<Engine::allLanes ^ (1u << lane)>(x.left, x.right));
    }
    return ok;
}

void testMonoAndDeterminism()
{
    const auto in = noise(16384);
    const auto ref = runProcessor(in, full, 48000, 2, 512);
    bool exact = true;
    for (int size : { 1, 64, 512, 4096, 0 })
    {
        const auto out = runProcessor(in, full, 48000, 2, size, size == 0);
        for (std::size_t i = 0; i < in.size(); ++i)
            exact &= same(out[i], ref[i]);
    }
    double energy = 0, residual = 0;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const double dry = static_cast<double>(in[i].left) + in[i].right;
        const double difference = static_cast<double>(ref[i].left) + ref[i].right - dry;
        energy += dry * dry;
        residual += difference * difference;
    }
    check(exact, "deterministic across processor instances, reset and block partition");
    check(residual < energy * 1e-13, "mono sum null below -130 dB");
}
void testLaneAndMix()
{
    const auto in = noise(32768);
    Engine alone, reference;
    bool lanes = true;
    for (std::size_t i = 0; i < vibrate::mix; ++i)
    {
        Controls parameters {};
        parameters[i] = parameters[vibrate::mix] = 1.0;
        alone.prepare(48000, parameters);
        reference.prepare(48000, full);
        double residual = 0, energy = 0;
        for (const auto& x : in)
        {
            const auto output = alone.process(x.left, x.right);
            double term = 0;
            const auto capture = [&](const vibrate::Sample& sample) { term = sample.terms[i]; };
            switch (i)
            {
                case 0: reference.process<1u << 0>(x.left, x.right, capture); break;
                case 1: reference.process<1u << 1>(x.left, x.right, capture); break;
                case 2: reference.process<1u << 2>(x.left, x.right, capture); break;
                case 3: reference.process<1u << 3>(x.left, x.right, capture); break;
                case 4: reference.process<1u << 4>(x.left, x.right, capture); break;
                default: break;
            }
            const double dl = static_cast<double>(output.left) - x.left - term;
            const double dr = static_cast<double>(output.right) - x.right + term;
            residual += dl * dl + dr * dr;
            energy += static_cast<double>(x.left) * x.left + static_cast<double>(x.right) * x.right;
        }
        lanes &= residual <= energy * 1e-13;
    }
    check(lanes, "all five isolated wet lanes add equal and opposite terms and match the masked reference");

    bool mixLinear = true;
    for (double m : { .1, .5, .9 })
    {
        auto values = full;
        values[vibrate::mix] = m;
        alone.prepare(48000, values);
        reference.prepare(48000, full);
        double residual = 0, energy = 0;
        for (const auto& x : in)
        {
            const auto a = alone.process(x.left, x.right);
            const auto b = reference.process(x.left, x.right);
            const double dl = static_cast<double>(a.left) - x.left - m * (static_cast<double>(b.left) - x.left);
            const double dr = static_cast<double>(a.right) - x.right - m * (static_cast<double>(b.right) - x.right);
            residual += dl * dl + dr * dr;
            energy += static_cast<double>(x.left) * x.left + static_cast<double>(x.right) * x.right;
        }
        mixLinear &= residual <= energy * 1e-12;
    }
    alone.prepare(48000, full);
    for (const auto& x : in) alone.process(x.left, x.right);
    auto zeroMix = full;
    zeroMix[vibrate::mix] = 0;
    alone.setControls(zeroMix);
    bool landed = true;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const auto x = in[i];
        const auto result = alone.process(x.left, x.right);
        if (i >= 1439) landed &= same(result, x);
    }
    check(mixLinear && landed, "mix is linear and reaches a bit-exact null within 30 ms");
}

struct Pink
{
    std::mt19937 rng { 0x8d392fu };
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

    float next()
    {
        const double white = static_cast<double>(rng()) * (2.0 / 4294967296.0) - 1;
        b0 = .99886 * b0 + .0555179 * white;
        b1 = .99332 * b1 + .0750759 * white;
        b2 = .96900 * b2 + .1538520 * white;
        b3 = .86650 * b3 + .3104856 * white;
        b4 = .55000 * b4 + .5329522 * white;
        b5 = -.7616 * b5 - .0168980 * white;
        const double out = b0 + b1 + b2 + b3 + b4 + b5 + b6 + .5362 * white;
        b6 = .115926 * white;
        return static_cast<float>(out * .11);
    }
};

struct Biquad
{
    double a1, a2, b0, b1, b2, z1 = 0, z2 = 0;

    double process(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct KWeight
{
    Biquad shelf { -1.69065929318241, .73248077421585, 1.53512485958697, -2.69169618940638, 1.19839281085285 };
    Biquad highpass { -1.99004745483398, .99007225036621, 1, -2, 1 };
    double energy = 0;

    void add(double x)
    {
        const double y = highpass.process(shelf.process(x));
        energy += y * y;
    }
};

double db(double ratio) { return 10 * std::log10(ratio); }

void testLevel()
{
    constexpr int length = 48000 * 60;
    std::vector<float> input(static_cast<std::size_t>(length));
    Pink pink;
    KWeight dry;
    double dryPeak = 0;
    for (auto& x : input)
    {
        x = pink.next();
        dry.add(x);
        dryPeak = std::max(dryPeak, std::abs(static_cast<double>(x)));
    }
    bool pass = true;
    for (int preset = 0; preset < vibrate::numPresets; ++preset)
    {
        Engine engine;
        engine.prepare(48000, vibrate::presets[preset].values);
        KWeight kLeft, kRight;
        double left = 0, right = 0, cross = 0, peak = 0;
        for (float x : input)
        {
            const auto y = engine.process(x, x);
            const double l = y.left, r = y.right;
            left += l * l;
            right += r * r;
            cross += l * r;
            kLeft.add(l);
            kRight.add(r);
            peak = std::max({ peak, std::abs(l), std::abs(r) });
        }
        const double loudness = db((kLeft.energy + kRight.energy) / (2 * dry.energy));
        const double peakRise = 2 * db(peak / dryPeak);
        const double balance = db(left / right);
        const double corr = cross / std::sqrt(left * right);
        const bool init = preset == 0;
        const bool within = loudness <= (init ? .30 : 1.50) && peakRise <= (init ? 1 : 5)
                         && std::abs(balance) <= .25 && corr >= (init ? .85 : .45);
        if (init || preset == vibrate::numPresets - 1 || !within)
            std::cout << vibrate::presets[preset].name << " pink mono: +" << loudness << " dB K, +"
                      << peakRise << " dB peak, " << balance << " dB L/R, correlation " << corr << '\n';
        pass &= within;
    }
    check(pass, "60 s pink mono loudness, peak, balance and correlation for every preset");

    Engine engine;
    engine.prepare(48000, full);
    double worst = -1000;
    for (double hz : { 20.0, 30.0, 40.0, 50.0, 59.0 })
    {
        engine.reset(full);
        double energy = 0, side = 0;
        for (int i = 0; i < 48000 * 3; ++i)
        {
            const auto x = static_cast<float>(.75 * std::sin(2.0 * vibrate::pi * hz * i / 48000));
            const auto y = engine.process(x, x);
            if (i < 4800) continue;
            const double wet = (static_cast<double>(y.left) - y.right) * .5;
            side += wet * wet;
            energy += static_cast<double>(x) * x;
        }
        worst = std::max(worst, db(side / energy));
    }
    std::cout << "Worst bass added side " << worst << " dB re mid\n";
    check(worst <= -36, "bass stays centred below 60 Hz");
}

void testMonoSettings()
{
    Pink pink;
    constexpr std::size_t size = 4096;
    std::array<std::array<Stereo, size>, 3> input {};
    double sweepPhase = 0;
    for (std::size_t i = 0; i < size; ++i)
    {
        input[0][i] = { pink.next(), pink.next() };
        sweepPhase += 2.0 * vibrate::pi * 20 * std::pow(1000.0, static_cast<double>(i) / static_cast<double>(size - 1)) / 48000;
        const auto sweep = static_cast<float>(.5 * std::sin(sweepPhase));
        input[1][i] = { sweep, sweep * .83f };
        const double t = static_cast<double>(i % 510u) / 48000;
        const auto drum = static_cast<float>(.55 * std::exp(-150 * t) * std::sin(380 * t)
                        + .16 * std::exp(-620 * t) * pink.next());
        input[2][i] = { drum, drum };
    }
    bool pass = true;
    std::mt19937 rng(0x329374u);
    Engine engine;
    for (int setting = 0; setting < vibrate::numPresets + 200; ++setting)
    {
        Controls c;
        if (setting < vibrate::numPresets)
            c = vibrate::presets[setting].values;
        else
            for (auto& value : c) value = static_cast<double>(rng() % 1001u) / 1000;
        for (const auto& source : input)
        {
            engine.prepare(48000, c);
            double reference = 0, residual = 0;
            for (auto x : source)
            {
                const auto out = engine.process(x.left, x.right);
                const double mono = static_cast<double>(x.left) + x.right;
                const double diff = (static_cast<double>(out.left) + out.right) - mono;
                reference += mono * mono;
                residual += diff * diff;
            }
            pass &= residual <= reference * 1e-13;
        }
    }
    check(pass, "mono sum below -130 dB on pink noise, sweep and drums for presets and 200 random settings");

    // A broadband transfer check with a DFT at each octave and third-octave centre, using the same dry/wet sweep.
    engine.reset(full);
    std::array<double, size> wet {};
    for (std::size_t i = 0; i < size; ++i)
    {
        const auto x = input[1][i];
        const auto y = engine.process(x.left, x.right);
        wet[i] = static_cast<double>(y.left) + y.right;
    }
    bool flat = true;
    for (double hz = 20; hz <= 20000; hz *= std::pow(2.0, 1.0 / 3))
    {
        std::complex<double> dryBin {}, wetBin {};
        for (std::size_t i = 0; i < size; ++i)
        {
            const auto basis = std::polar(1.0, 2.0 * vibrate::pi * hz * static_cast<double>(i) / 48000);
            const auto x = input[1][i];
            dryBin += basis * (static_cast<double>(x.left) + x.right);
            wetBin += basis * wet[i];
        }
        flat &= std::abs(db(std::norm(wetBin) / std::norm(dryBin))) < .01;
    }
    check(flat, "mono response within ±0.01 dB across 20 Hz–20 kHz");
}

void testBounds()
{
    bool bounded = true, throwBounded = true;
    const auto pitchChange = [](double a, double b) { return std::abs(1200 * std::log2(1 - (a - b))); };
    for (double fs : sampleRates)
    {
        Engine engine;
        for (double value : { .1, .3, .6, 1.0 })
        {
            Controls c;
            c.fill(value);
            engine.prepare(fs, c);
            vibrate::Sample last {};
            for (int i = 0; i < static_cast<int>(fs * 1.5); ++i)
            {
                if (i == static_cast<int>(fs) && value == 1.0)
                    engine.setControls({ 0, 0, 0, 0, 0, 0 });
                engine.process(.5f, .5f, [&](const vibrate::Sample& now) {
                    for (double tap : { now.pitch1, now.pitch2, now.delay1, now.delay2 })
                        bounded &= tap == 0.0 || (tap >= 1.0 && tap + 2 < .016 * fs);
                    if (now.pitch1 > 0 && last.pitch1 > 0)
                    {
                        const double p = std::max(pitchChange(now.pitch1, last.pitch1), pitchChange(now.pitch2, last.pitch2));
                        const double d = std::max(pitchChange(now.delay1, last.delay1), pitchChange(now.delay2, last.delay2));
                        if (i < fs)
                        {
                            bounded &= p <= 12.5 * value && d <= 1.75 * value * std::sqrt(value)
                                && std::abs(2 * db((1 + now.panGain) / (1 - now.panGain))) <= 3.75 * value
                                && std::abs(now.delay2 - now.delay1) <= .00375 * fs * value * std::sqrt(value) + 1e-9
                                && (now.phase1 == 0 || std::abs(std::log2(now.phase1 / 800)) <= .75 + .75 * value + 1e-9);
                        }
                        else
                            throwBounded &= p <= 38.75 && d <= 28.0;
                    }
                    last = now;
                });
            }
        }
    }
    check(bounded && throwBounded, "tap, cents and pan bounds at all rates, including a full throw");
}

void testSpline()
{
    constexpr int count = 12000;
    std::array<std::vector<double>, vibrate::rates.size()> paths;
    bool autocorrelation = true, independent = true, sampleRatesMatch = true;
    for (std::size_t lane = 0; lane < paths.size(); ++lane)
    {
        vibrate::Spline mod;
        mod.reset(static_cast<int>(lane), 100.0);
        auto& values = paths[lane];
        values.resize(count);
        for (auto& value : values) value = mod.next();
        const double mean = std::accumulate(values.begin(), values.end(), 0.0) / count;
        for (auto& value : values) value -= mean;
        const double norm = std::sqrt(std::inner_product(values.begin(), values.end(), values.begin(), 0.0));
        for (auto& value : values) value /= norm;
        for (int lag = static_cast<int>(400 / vibrate::rates[lane]) + 1; lag < count; ++lag)
            autocorrelation &= std::abs(std::inner_product(values.begin() + lag, values.end(), values.begin(), 0.0)) <= .30;
        for (std::size_t j = 0; j < lane; ++j)
            independent &= std::abs(std::inner_product(values.begin(), values.end(), paths[j].begin(), 0.0)) < .15;

        vibrate::Spline a, b, c;
        a.reset(static_cast<int>(lane), 44100.0);
        b.reset(static_cast<int>(lane), 48000.0);
        c.reset(static_cast<int>(lane), 96000.0);
        for (int i = 0; i < 700; ++i)
        {
            const double x = a.next(), y = b.next(), z = c.next();
            sampleRatesMatch &= std::abs(x - y) <= 1e-3 && std::abs(x - z) <= 1e-3;
            for (int k = 1; k < 441; ++k) a.next();
            for (int k = 1; k < 480; ++k) b.next();
            for (int k = 1; k < 960; ++k) c.next();
        }
    }
    check(autocorrelation && independent, "120 s splines meet autocorrelation and pairwise correlation bounds");
    check(sampleRatesMatch, "sample-rate invariant paths at equal times");
}

void testRateStatistics()
{
    struct Stats { double cents = 0, gap = 0, side = 0; };
    auto render = [](int fs) {
        Stats result;
        Engine e;
        e.prepare(fs, full);
        double lastP = 0, lastQ = 0, count = 0;
        for (int i = 0; i < 10 * fs; ++i)
        {
            const auto x = static_cast<float>(.5 * std::sin(2.0 * vibrate::pi * 731.0 * i / fs));
            e.process(x, x, [&](const vibrate::Sample& sample) {
                if (i > fs / 10)
                {
                    const double p = 1200 * std::log2(1 - (sample.pitch1 - lastP));
                    const double q = 1200 * std::log2(1 - (sample.pitch2 - lastQ));
                    const double gap = (sample.delay2 - sample.delay1) / fs;
                    result.cents += p * p + q * q;
                    result.gap += gap * gap;
                    result.side += sample.terms[vibrate::side] * sample.terms[vibrate::side];
                    ++count;
                }
                lastP = sample.pitch1;
                lastQ = sample.pitch2;
            });
        }
        return Stats { std::sqrt(result.cents / count), std::sqrt(result.gap / count), std::sqrt(result.side / count) };
    };
    const auto ref = render(48000);
    bool match = true;
    for (int fs : { 44100, 96000 })
    {
        const auto stats = render(fs);
        match &= std::abs(stats.cents / ref.cents - 1) < .05 && std::abs(stats.gap / ref.gap - 1) < .05
              && std::abs(stats.side / ref.side - 1) < .05;
    }
    check(match, "cents, gap and side RMS within 5% across 44.1, 48 and 96 kHz");

    Engine e;
    e.prepare(48000, full);
    double sum = 0, max = 0;
    for (int i = 0; i < 120 * 48000; ++i)
        e.process(0, 0, [&](const vibrate::Sample& sample) {
            const double gap = sample.delay2 - sample.delay1;
            sum += gap;
            max = std::max(max, std::abs(gap));
        });
    check(std::abs(sum / (120 * 48000)) <= .1 * max, "120 s mean delay gap under 10% of peak");
}

void testSmoothnessAndSilence()
{
    VibrateProcessor processor;
    processor.setCurrentProgram(15);
    processor.selectAb(1);
    processor.setCurrentProgram(0);
    processor.prepareToPlay(48000, 64);
    juce::AudioBuffer<float> b(2, 64);
    juce::MidiBuffer midi;
    bool smooth = true;
    double previous = 0, difference = 0, maximum = 0;
    for (int block = 0; block < 4200; ++block)
    {
        if (block >= 150 && block <= 3600 && block % 150 == 0)
        {
            const auto i = static_cast<std::size_t>((block / 150 - 1) / 2);
            if (i < vibrate::numControls)
                set(processor, i, block % 300 != 0 ? 1.0 : 0.0);
            else if (i < 8)
                processor.setCurrentProgram(block % 300 != 0 ? 15 : 1);
            else if (i < 10)
                processor.selectAb(block % 300 != 0 ? 0 : 1);
            else
                set(processor, vibrate::numControls, block % 300 != 0 ? 1.0 : 0.0);
        }
        for (int i = 0; i < 64; ++i)
        {
            const auto x = static_cast<float>(.5 * std::sin(2.0 * vibrate::pi * 257 * (64 * block + i) / 48000));
            b.setSample(0, i, x);
            b.setSample(1, i, x);
        }
        processor.processBlock(b, midi);
        for (int i = 0; i < 64; ++i)
        {
            const double wet = (static_cast<double>(b.getSample(0, i)) - b.getSample(1, i)) * .5;
            const double delta = wet - previous;
            if (block >= 12 && std::abs(delta - difference) > maximum) // after every tap's onset (16 ms)
            {
                maximum = std::abs(delta - difference);
                if (std::getenv("VIBRATE_DEBUG")) std::cout << "  max at block " << block << " sample " << i << ": " << maximum << '\n';
            }
            previous = wet;
            difference = delta;
        }
    }
    smooth &= maximum < .0015;
    std::cout << "Maximum wet second difference across automation " << maximum << '\n';
    check(smooth, "full knob steps, preset jumps, A/B and bypass introduce no clicks");

    Engine engine;
    engine.prepare(48000, full);
    bool silence = true;
    for (int i = 0; i < 24000; ++i) engine.process(.1f, .1f);
    for (int i = 0; i < 14400; ++i)
    {
        const auto y = engine.process(0, 0);
        if (i >= 7200) silence &= same(y, { 0, 0 });
    }
    check(silence, "tail reaches exact zero in 150 ms");

    bool finite = true;
    engine.process(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
    for (int i = 0; i < 24000; ++i)
    {
        const auto y = engine.process(.25f, .25f, [&](const vibrate::Sample& sample) {
            finite &= std::all_of(sample.terms.begin(), sample.terms.end(), [](double x) { return std::isfinite(x); });
        });
        finite &= std::isfinite(y.left) && std::isfinite(y.right);
    }
    check(finite, "NaN and infinity do not enter the wet state");
}

void testState()
{
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("vibrate-tests", "", false);
    VibrateProcessor a(folder), b(folder);
    a.setCurrentProgram(9);
    a.selectAb(1);
    a.setCurrentProgram(14);
    a.editorWidth = 1180;
    set(a, vibrate::numControls, 1);
    juce::MemoryBlock state;
    a.getStateInformation(state);
    b.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    bool ok = b.getCurrentProgram() == 14 && b.abSlot() == 1 && b.editorWidth == 1180;
    ok &= b.getBypassParameter()->getValue() == 1.0f;
    b.selectAb(0);
    ok &= b.getCurrentProgram() == 9 && b.abSlot() == 0
       && same(b.apvts.getParameter("pitch")->getValue(), .55f);
    b.selectAb(1);
    ok &= b.getCurrentProgram() == 14 && b.abSlot() == 1;

    const auto preset = a.saveUserPreset();
    b.setCurrentProgram(3);
    b.loadUserPreset(preset.getFileNameWithoutExtension());
    ok &= preset.existsAsFile() && b.presetName() == "user 01" && same(b.apvts.getParameter("pitch")->getValue(), .85f);
    b.stepPreset(1);
    ok &= b.getCurrentProgram() == 0;
    b.stepPreset(-1);
    ok &= b.presetName() == "user 01";
    folder.deleteRecursively();

    const auto fixture = juce::XmlDocument::parse(R"xml(
<VIBRATE stateVersion="1" preset="-1" presetName="kept" ab="1" slotA="9;0.55,0.15,0.5,0.4,0.5,1" slotNameA="" editorWidth="1290">
  <PARAM id="pitch" value="169"/><PARAM id="delay" value="11.5"/><PARAM id="side" value="-1"/>
  <PARAM id="unknown" value="45"/><UNKNOWN id="pan" value="0"/>
</VIBRATE>)xml");
    VibrateProcessor::copyXmlToBinary(*fixture, state);
    b.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    ok &= b.presetName() == "kept" && b.abSlot() == 1 && b.editorWidth == 1290;
    ok &= same(b.apvts.getParameter("pitch")->getValue(), 1) && same(b.apvts.getParameter("delay")->getValue(), .115f)
        && same(b.apvts.getParameter("pan")->getValue(), .4f) && same(b.apvts.getParameter("side")->getValue(), 0)
        && same(b.getBypassParameter()->getValue(), 0);
    b.setStateInformation(state.getData(), static_cast<int>(state.getSize()) - 15);
    b.setStateInformation("invalid", 7);
    ok &= b.presetName() == "kept" && same(b.apvts.getParameter("pitch")->getValue(), 1);
    b.selectAb(0);
    ok &= b.getCurrentProgram() == 9;
    check(ok, "state v1, defaults, bounds, bad input, A/B and program and user preset round trip");
}

double benchmark(bool silent)
{
    VibrateProcessor p;
    p.setCurrentProgram(15);
    p.prepareToPlay(48000, 512);
    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    std::array<float, 512> signal {};
    if (!silent)
        for (std::size_t i = 0; i < signal.size(); ++i)
            signal[i] = static_cast<float>(.35 * std::sin(.17 * static_cast<double>(i))
                                         + .12 * std::sin(.75 * static_cast<double>(i)));
    auto process = [&] {
        for (int c = 0; c < 2; ++c)
            block.copyFrom(c, 0, signal.data(), static_cast<int>(signal.size()));
        p.processBlock(block, midi);
    };
    for (int i = 0; i < 512; ++i) process();
    double best = std::numeric_limits<double>::max();
    // Short trials: the fastest one is the code's own cost, without the machine's other work.
    for (int trial = 0; trial < 48; ++trial)
    {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 64; ++i) process();
        const auto stop = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(stop - start).count() / (64 * 512));
    }
    return best;
}

int bench()
{
    const double loud = benchmark(false), quiet = benchmark(true);
    std::cout << "48 kHz / 512 samples / all 100: " << loud << " ns/frame (" << loud * .0048
              << "% of one core), silence " << quiet << " ns/frame\n";
    check(loud <= 100.0, "DSP benchmark gate ≤100 ns/frame");
    check(quiet <= loud * 1.05, "no denormal slowdown in silence");
    return failures == 0 ? 0 : 1;
}

double testMs = 0;
double testClock() { return testMs; }

juce::Component* find(juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id)
        return &parent;
    for (auto* child : parent.getChildren())
        if (auto* match = find(*child, id))
            return match;
    return nullptr;
}

void click(juce::Component& editor, const char* id)
{
    if (auto* button = dynamic_cast<juce::Button*>(find(editor, id)))
        if (button->onClick)
            button->onClick();
}

void testFeed()
{
    VibrateProcessor p;
    p.setCurrentProgram(15);
    p.prepareToPlay(48000, 240);
    juce::AudioBuffer<float> block(2, 240);
    juce::MidiBuffer midi;
    block.clear();
    p.processBlock(block, midi);
    vibrate::Frame frame;
    const bool gotSilence = p.feed.pop(frame);
    bool ok = gotSilence && frame.midEnergy == 0 && frame.leftEnergy == 0 && frame.rightEnergy == 0
        && std::all_of(frame.energy.begin(), frame.energy.end(), [](double energy) { return energy == 0; });
    check(ok, "silent frame has zero dry, output and lane energies");

    p.reset();
    Engine engine;
    engine.prepare(48000, full);
    vibrate::Sample first {}, last {};
    double gap = 0;
    for (int i = 0; i < 240; ++i)
    {
        const float x = static_cast<float>(.5 * std::sin(2.0 * vibrate::pi * 731.0 * i / 48000));
        block.setSample(0, i, x);
        block.setSample(1, i, x);
        engine.process(x, x, [&](const vibrate::Sample& sample) {
            if (i == 0) first = sample;
            last = sample;
            gap += sample.delay2 - sample.delay1;
        });
    }
    p.processBlock(block, midi);
    const bool gotSound = p.feed.pop(frame);
    ok = gotSound && std::abs(frame.pitchStep[0] - (last.pitch1 - first.pitch1) / 239) < 1e-14
       && std::abs(frame.pitchStep[1] - (last.pitch2 - first.pitch2) / 239) < 1e-14
       && std::abs(frame.gapSeconds - gap / (240 * 48000)) < 1e-14;
    check(ok, "feed reports actual pitch read-pointer motion and actual delay gap");

    for (unsigned k = 0; k < vibrate::Feed::capacity + 20; ++k)
    {
        block.clear();
        p.processBlock(block, midi);
    }
    uint32_t lastSequence = 0;
    unsigned count = 0;
    while (p.feed.pop(frame))
    {
        lastSequence = frame.sequence;
        ++count;
    }
    block.clear();
    p.processBlock(block, midi);
    const bool resumed = p.feed.pop(frame);
    check(count == vibrate::Feed::capacity && resumed && frame.sequence - lastSequence == 21,
          "full FIFO drops frames and resumes on the measured frame sequence");

    vibrate::Lanes lanes;
    p.reset();
    p.setCurrentProgram(15);
    set(p, vibrate::phase, 0);
    set(p, vibrate::mix, 0);
    for (int b = 0; b < 20; ++b)
    {
        for (int i = 0; i < block.getNumSamples(); ++i)
        {
            const auto x = static_cast<float>(.7 * std::sin(.177 * (b * 240 + i)));
            block.setSample(0, i, x);
            block.setSample(1, i, x);
        }
        p.processBlock(block, midi);
        if (p.feed.pop(frame)) lanes.add(frame);
    }
    const unsigned active = lanes.readout().active;
    const auto mixPlot = lanes.plot(vibrate::mix);
    const bool grey = std::all_of(mixPlot.begin(), mixPlot.end(), [](const vibrate::Lanes::Cell& c) { return c.dots == 0; })
                   && std::any_of(mixPlot.begin(), mixPlot.end(), [](const vibrate::Lanes::Cell& c) { return c.backInk != 0 && c.backInk != vibrate::baselineInk; });
    check(active != 0 && (active & (1u << vibrate::phase)) == 0 && (active & (1u << vibrate::mix)) != 0 && grey,
          "zero phase is blank; at zero mix the other lanes still move and the mix lane shows the signal all grey");
}

bool colored(juce::Colour c);

// 0.2.0: every lane's depth at 100 % is 1.25 times 0.1.0's.
void testDepths()
{
    const auto g = Engine::gains(full, false);
    const auto d = Engine::depths(full);
    const double pitchCents = Engine::wanderCents + Engine::flutterCents;
    const double panDb = 20 * std::log10((1 + g[vibrate::pan]) / (1 - g[vibrate::pan]));
    const double gapMs = 2 * d[1] * 1000, octaves = d[2], side = g[vibrate::side];
    std::cout << "Depths at 100 %: pitch " << pitchCents << " c, pan " << panDb << " dB, delay gap " << gapMs << " ms, phase "
              << octaves << " oct, side " << side * 100 << " %\n";
    const bool scaled = pitchCents == 1.25 * 10 && Engine::wanderCents == 1.25 * 9 && Engine::flutterCents == 1.25 * 1
                     && std::abs(panDb - 1.25 * 3.0) < 1e-3 && std::abs(gapMs - 1.25 * 3.0) < 1e-12
                     && std::abs(octaves - 1.25 * 1.2) < 1e-12 && std::abs(Engine::depths({})[2] - 1.25 * .6) < 1e-12
                     && std::abs(side - 1.25 * .28) < 1e-12;
    check(scaled, "depths at 100 % are 1.25 times 0.1.0's: 12.5 c, 3.75 dB, 3.75 ms, 1.5 oct, 35 %");

    // The deepest pitch read stays in the past, and the measured extremes over ten minutes stay inside the new figures.
    const double nearest = Engine::pitchCentre - (Engine::wanderCents / (3462.0 * vibrate::rates[0]) + Engine::flutterCents / (3462.0 * vibrate::rates[2]));
    Engine engine;
    engine.prepare(48000, full);
    double cents = 0, pan = 0, gap = 0, octave = 0, previous1 = 0, previous2 = 0;
    for (int i = 0; i < 48000 * 600; ++i)
        engine.process(.25f, .25f, [&](const vibrate::Sample& s) {
            if (previous1 > 0)
                cents = std::max({ cents, std::abs(1200 * std::log2(1 - (s.pitch1 - previous1))), std::abs(1200 * std::log2(1 - (s.pitch2 - previous2))) });
            previous1 = s.pitch1, previous2 = s.pitch2;
            pan = std::max(pan, std::abs(20 * std::log10((1 + s.panGain) / (1 - s.panGain))));
            gap = std::max(gap, std::abs(s.delay2 - s.delay1) / 48);
            octave = std::max({ octave, std::abs(std::log2(s.phase1 / 800)), std::abs(std::log2(s.phase2 / 800)) });
        });
    std::cout << "Ten minutes at 100 %: peak " << cents << " c, " << pan << " dB, " << gap << " ms, " << octave << " oct; nearest pitch read "
              << nearest * 1000 << " ms\n";
    check(nearest > .00025 && cents <= 12.5 && pan <= 3.75 && gap <= 3.75 && octave <= 1.5,
          "pitch reads stay in the past; ten minutes at 100 % stay inside the 1.25x figures");
}

// The mix lane: the signal's lit dots per column, the lowest mix x of them in the rainbow, the rest grey.
void testMixColours()
{
    bool exact = true, grey = true, rainbow = true;
    for (float mix : { 0.0f, 50.0f, 100.0f })
    {
        VibrateProcessor p;
        p.setCurrentProgram(0);
        set(p, vibrate::mix, mix / 100);
        p.prepareToPlay(48000, 240);
        juce::AudioBuffer<float> block(2, 240);
        juce::MidiBuffer midi;
        vibrate::Lanes lanes;
        vibrate::Frame frame;
        for (int b = 0; b < 1200; ++b)
        {
            for (int i = 0; i < block.getNumSamples(); ++i)
            {
                const int t = b * 240 + i;
                const auto x = static_cast<float>((.15 + .6 * std::abs(std::sin(t * 2e-5))) * std::sin(.0731 * t));
                block.setSample(0, i, x);
                block.setSample(1, i, x);
            }
            p.processBlock(block, midi);
            while (p.feed.pop(frame)) lanes.add(frame);
        }
        exact &= lanes.mixShown() == mix / 100;
        const auto plot = lanes.plot(vibrate::mix);
        int columnsLit = 0, rainbowDots = 0, litDots = 0;
        for (int x = 0; x < vibrate::Lanes::columns; ++x)
        {
            int top = -1, bottom = -1, lit = 0, coloured = 0, firstRainbow = -1;
            for (int y = 0; y < vibrate::Lanes::dotsHigh; ++y)
            {
                const auto& c = plot[static_cast<std::size_t>(y / 4 * vibrate::plotWidth + x / 2)];
                const auto bit = vibrate::brailleBit(x % 2, y % 4);
                const bool over = (c.dots & bit) != 0, under = (c.back & bit) != 0;
                if (!over && !under) continue;
                if (top < 0) top = y;
                bottom = y;
                ++lit;
                if (over)
                {
                    ++coloured;
                    rainbow &= colored(juce::Colour(0xff000000u | c.ink));
                    firstRainbow = firstRainbow < 0 ? y : firstRainbow;
                }
                else
                {
                    exact &= firstRainbow < 0; // no grey dot under a rainbow one
                    grey &= !colored(juce::Colour(0xff000000u | c.backInk));
                }
            }
            if (lit <= 1) continue; // an empty column keeps only its centre-line dot
            exact &= lit == bottom - top + 1 && coloured == vibrate::Lanes::rainbowDots(lit, mix / 100);
            ++columnsLit;
            rainbowDots += coloured;
            litDots += lit;
        }
        std::cout << "Mix " << mix << " %: " << columnsLit << " columns, " << rainbowDots << " of " << litDots << " dots in the rainbow\n";
        exact &= columnsLit > 150 && (mix == 0 ? rainbowDots == 0 : mix == 100 ? rainbowDots == litDots
                                     : std::abs(rainbowDots - .5 * litDots) <= .5 * columnsLit);
    }
    check(exact && grey && rainbow, "mix lane: 0 % all grey, 50 % the lower half of each column rainbow, 100 % all rainbow");
}

using Image = juce::Image;

Image picture(juce::Component& editor, float scale = 1.0f)
{
    return editor.createComponentSnapshot(editor.getLocalBounds(), true, scale);
}

juce::String pixels(const Image& image)
{
    juce::MemoryOutputStream bytes;
    const Image::BitmapData bitmap(image, Image::BitmapData::readOnly);
    for (int y = 0; y < bitmap.height; ++y)
        bytes.write(bitmap.getLinePointer(y), static_cast<std::size_t>(bitmap.width * bitmap.pixelStride));
    return juce::SHA256(bytes.getData(), bytes.getDataSize()).toHexString();
}

std::unique_ptr<juce::AudioProcessorEditor> makeEditor(VibrateProcessor& p)
{
    p.clockMs = testClock;
    testMs = 0;
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    dynamic_cast<juce::Timer&>(*editor).stopTimer();
    return editor;
}

void feedEditor(VibrateProcessor& p, juce::Component& e, const std::vector<Stereo>& in, int blocks, double fs, bool quiet = false)
{
    juce::AudioBuffer<float> block(2, 128);
    juce::MidiBuffer midi;
    auto& timer = dynamic_cast<juce::Timer&>(e);
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < 128; ++i)
        {
            const auto x = quiet ? Stereo { 0, 0 } : in[static_cast<std::size_t>(b * 128 + i) % in.size()];
            block.setSample(0, i, x.left);
            block.setSample(1, i, x.right);
        }
        testMs += 128000.0 / fs;
        p.processBlock(block, midi);
        if (b % 6 == 5) timer.timerCallback();
    }
    timer.timerCallback();
}

bool colored(juce::Colour c) { return std::max({ c.getRed(), c.getGreen(), c.getBlue() }) - std::min({ c.getRed(), c.getGreen(), c.getBlue() }) > 15; }

void testEditor()
{
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("vibrate-editor", "", false);
    VibrateProcessor p(folder);
    p.prepareToPlay(48000, 128);
    auto e = makeEditor(p);
    auto& timer = dynamic_cast<juce::Timer&>(*e);
    const auto idle = picture(*e);
    bool layout = e->getWidth() == 1056 && e->getHeight() == 752;
    auto* layer = e->getChildComponent(0);
    int dials = 0;
    for (auto* child : layer->getChildren())
        if (auto* dial = dynamic_cast<juce::Slider*>(child))
        {
            const auto id = dial->getComponentID();
            const auto i = static_cast<std::size_t>(dials++);
            layout &= i < vibrate::numControls && id == vibrate::ids[i]
                   && dial->getSliderStyle() == juce::Slider::RotaryVerticalDrag
                   && dial->getBounds() == juce::Rectangle<int>(115 * 8, (vibrate::dialRows[i] - 1) * 16, 80, 96);
        }
    layout &= dials == 6 && !find(*e, "bypass") && !find(*e, "C") && !find(*e, "randomize")
              && find(*e, "A") && find(*e, "B") && find(*e, "prev") && find(*e, "next");
    const auto bounds = find(*e, "preset")->getBounds();
    for (int i = 0; i < vibrate::numPresets; ++i)
    {
        layout &= p.getCurrentProgram() == i && bounds == juce::Rectangle<int>(14 * 8, 16, 24 * 8, 16)
                  && find(*e, "preset")->getBounds() == bounds;
        click(*e, "next");
        timer.timerCallback();
    }
    layout &= p.getCurrentProgram() == 0;
    click(*e, "prev");
    layout &= p.getCurrentProgram() == vibrate::numPresets - 1;
    p.setCurrentProgram(0);
    timer.timerCallback();
    check(layout, "six family dials, fixed preset geometry, A/B only and factory wrap");

    auto* dial = dynamic_cast<juce::Slider*>(find(*e, "pitch"));
    const bool keyChanged = dial->keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    bool touch = keyChanged && std::abs(dial->getValue() - .31) < 1e-6;
    touch &= std::abs(dial->getValueFromText("86.7 %") - .867) < 1e-6;
    dial->setValue(dial->getValueFromText("86.7 %"), juce::sendNotificationSync);
    touch &= std::abs(p.apvts.getRawParameterValue("pitch")->load() - 86.7f) < .0001f;
    const auto now = juce::Time::getCurrentTime();
    const juce::Point<float> centre { 40, 48 };
    const juce::MouseEvent doubleClick(juce::Desktop::getInstance().getMainMouseSource(), centre, juce::ModifierKeys::leftButtonModifier,
                                      1, 0, 0, 0, 0, dial, dial, now, centre, now, 2, false);
    dial->mouseDoubleClick(doubleClick);
    touch &= same(p.apvts.getRawParameterValue("pitch")->load(), 30);
    check(touch, "rotary key steps, typed percent and double-click reset reach the parameter");

    click(*e, "preset");
    auto* menu = find(*e, "presetMenu");
    bool dropdown = menu && menu->isVisible();
    if (dropdown)
        for (int i = 0; i < vibrate::numPresets; ++i)
            dropdown &= find(*menu, "preset" + juce::String(i)) != nullptr;
    const auto open = picture(*e);
    click(*e, "preset6");
    dropdown &= p.getCurrentProgram() == 6 && menu != nullptr && !menu->isVisible();
    click(*e, "preset");
    dropdown &= menu != nullptr && menu->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && !menu->isVisible();
    check(dropdown, "grouped preset menu lists all 16; selection and Escape close it");

    p.setCurrentProgram(0);
    p.reset();
    auto signal = noise(8192);
    feedEditor(p, *e, signal, 1790, 48000);
    const auto playing = picture(*e);
    bool printed = true;
    for (int lane = 0; lane < vibrate::numControls; ++lane)
    {
        const int top = (vibrate::dialRows[static_cast<std::size_t>(lane)] - 1) * 16;
        int plot = 0, ring = 0;
        for (int y = top; y < top + 96; ++y)
        {
            for (int x = 3 * 8; x < 97 * 8; ++x) plot += colored(playing.getPixelAt(x, y)) ? 1 : 0;
            for (int x = 116 * 8; x < 124 * 8; ++x) ring += colored(idle.getPixelAt(x, y)) ? 1 : 0;
        }
        printed &= plot > 20 && ring > 20;
    }
    check(printed, "all six playing traces and dial arcs have their lane ink inside the approved cells");

    testMs += 151;
    timer.timerCallback();
    check(pixels(picture(*e)) == pixels(idle), "editor no-block timeout returns to the flat pixel golden");
    feedEditor(p, *e, signal, 30, 48000);
    feedEditor(p, *e, signal, 120, 48000, true);
    check(pixels(picture(*e)) == pixels(idle), "silent audio returns to the flat pixel golden");
    e->setSize(1400, juce::roundToInt(1400 * 752 / 1056.0));
    check(p.editorWidth == 1400 && picture(*e).getWidth() == 1400, "resize scales the 132-by-47 grid and persists the width");

    const auto hashes = std::array<juce::String, 3> { pixels(idle), pixels(playing), pixels(open) };
    std::cout << "Pixel goldens (idle / playing / dropdown): " << hashes[0] << " " << hashes[1] << " " << hashes[2] << '\n';
#if JUCE_LINUX && JUCE_INTEL && JUCE_64BIT
    // Goldens use the local Linux software renderer. Geometry and ink checks also run on the CI platforms.
    if (std::getenv("GITHUB_ACTIONS") == nullptr)
        check(hashes == std::array<juce::String, 3> { "ade682e28c977983bc52688c643513372d96416d29c45d86a3685ee20e6165a8", "c5d51cd59aa49fbc679a0cb5c90dd40c0d6ad96506ebff6366c6de46d2591b7a", "c918a2418632d5eb0ce81e339191d84b2a339f8aa91d77078b82a63930837eb3" }, "local pixel goldens");
#endif
}

int screenshot(const juce::StringArray& args)
{
    const int width = args.size() > 2 ? args[2].getIntValue() : 1440;
    const float scale = args.size() > 3 ? args[3].getFloatValue() : 1.0f;
    if (width < 528 || width > 2112 || scale < .5f || scale > 3.0f)
        return 1;
    VibrateProcessor p;
    p.setCurrentProgram(args.size() > 6 ? args[6].getIntValue() : 0);
    for (const auto& arg : args) // id=value, after the preset (mix=50)
        if (auto* param = arg.contains("=") ? p.apvts.getParameter(arg.upToFirstOccurrenceOf("=", false, false)) : nullptr)
            param->setValueNotifyingHost(param->convertTo0to1(arg.fromFirstOccurrenceOf("=", false, false).getFloatValue()));
    p.prepareToPlay(48000, 128);
    auto e = makeEditor(p);
    e->setSize(width, juce::roundToInt(width * 752 / 1056.0));
    if (args.size() > 4 && args[4] != "-")
    {
        juce::MemoryBlock bytes;
        if (!juce::File::getCurrentWorkingDirectory().getChildFile(args[4]).loadFileAsData(bytes)
            || bytes.isEmpty() || bytes.getSize() % (2 * sizeof(float)) != 0)
            return 1;
        std::vector<Stereo> samples(bytes.getSize() / (2 * sizeof(float)));
        juce::MemoryInputStream source(bytes, false);
        for (auto& x : samples)
            x = { source.readFloat(), source.readFloat() };
        feedEditor(p, *e, samples, args.size() > 5 ? args[5].getIntValue() : 1920, 48000);
    }
    if (args.contains("menu"))
        click(*e, "preset");
    auto file = juce::File::getCurrentWorkingDirectory().getChildFile(args[1]);
    juce::FileOutputStream out(file);
    if (!out.openedOk() || !out.setPosition(0) || out.truncate().failed())
        return 1;
    return juce::PNGImageFormat().writeImageToStream(picture(*e, scale), out) ? 0 : 1;
}

int benchEditor()
{
    for (const int width : { 1056, 2112 })
    {
        VibrateProcessor p;
        p.setCurrentProgram(15);
        p.prepareToPlay(48000, 128);
        auto e = makeEditor(p);
        e->setSize(width, juce::roundToInt(width * 752 / 1056.0));
        auto& timer = dynamic_cast<juce::Timer&>(*e);
        auto surface = picture(*e);
        const auto input = noise(128);
        juce::AudioBuffer<float> block(2, 128);
        juce::MidiBuffer midi;
        double spent = 0, max = 0;
        for (int frame = 0; frame < 200; ++frame)
        {
            for (int n = 0; n < 6; ++n)
            {
                for (int i = 0; i < 128; ++i)
                {
                    block.setSample(0, i, input[static_cast<std::size_t>(i)].left);
                    block.setSample(1, i, input[static_cast<std::size_t>(i)].right);
                }
                p.processBlock(block, midi);
                testMs += 128000.0 / 48000;
            }
            const auto start = std::chrono::steady_clock::now();
            timer.timerCallback();
            juce::Graphics graphics(surface);
            e->paintEntireComponent(graphics, false);
            const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            spent += elapsed;
            max = std::max(max, elapsed);
        }
        std::cout << width << " px editor, timer + paint " << spent / 200 << " ms mean, " << max << " ms max\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juce;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(argv[i]);
    if (args.size() >= 3 && (args[0] == "--render" || args[0] == "--frames"))
        return renderFile(args);
    if (args.size() == 1 && args[0] == "--bench")
        return bench();
    if (args.size() == 1 && args[0] == "--bench-editor")
        return benchEditor();
    if (args.size() >= 2 && args[0] == "--screenshot")
        return screenshot(args);
    testNull();
    const auto input = noise(1536);
    check(testMask<vibrate::pitch>(input) && testMask<vibrate::pan>(input) && testMask<vibrate::delay>(input)
          && testMask<vibrate::phase>(input) && testMask<vibrate::side>(input),
          "all five knobs null identically to the compile-time lane mask");
    testMonoAndDeterminism();
    testLaneAndMix();
    testMonoSettings();
    testLevel();
    testBounds();
    testSpline();
    testRateStatistics();
    testSmoothnessAndSilence();
    testState();
    testFeed();
    testDepths();
    testMixColours();
    testEditor();
    return failures == 0 ? 0 : 1;
}
