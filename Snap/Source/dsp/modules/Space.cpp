#include "Space.h"
#include "SpaceIRData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <bit>
#include <chrono>
#include <numbers>
#include <string>
#include <vector>

namespace snap
{
namespace
{
using namespace spaceConstants;
using Frame = std::array<float, 2>;

struct Bin
{
    float real = 0, imag = 0;
};

Bin multiply(Bin a, Bin b)
{
    return { a.real * b.real - a.imag * b.imag, a.real * b.imag + a.imag * b.real };
}

// Radix-2 FFT with all tables built on the worker. FFT execution has no shared state.
class FourierPlan
{
public:
    explicit FourierPlan(int size) : reversed((size_t) size), roots((size_t) size / 2)
    {
        for (int i = 1; i < size; ++i)
            reversed[(size_t) i] = reversed[(size_t) (i / 2)] / 2 + (i % 2) * (size / 2);
        for (size_t i = 0; i < roots.size(); ++i)
        {
            const double angle = -std::numbers::pi * (double) i / (double) roots.size();
            roots[i] = { (float) std::cos(angle), (float) std::sin(angle) };
        }
    }

    void run(std::vector<Bin>& data, bool inverse) const
    {
        for (size_t i = 0; i < data.size(); ++i)
            if (const auto j = (size_t) reversed[i]; i < j)
                std::swap(data[i], data[j]);

        for (size_t count = 2; count <= data.size(); count *= 2)
        {
            const size_t half = count / 2, stride = data.size() / count;
            for (size_t start = 0; start < data.size(); start += count)
                for (size_t i = 0; i < half; ++i)
                {
                    auto root = roots[i * stride];
                    if (inverse)
                        root.imag = -root.imag;
                    const auto a = data[start + i];
                    const auto b = multiply(root, data[start + half + i]);
                    data[start + i] = { a.real + b.real, a.imag + b.imag };
                    data[start + half + i] = { a.real - b.real, a.imag - b.imag };
                }
        }
    }

private:
    std::vector<int> reversed;
    std::vector<Bin> roots;
};

// Each tier starts at its partition size in the IR. That one-partition offset
// gives us a full input block to compute the next output block with no latency.
class PartitionTier
{
public:
    PartitionTier(const juce::AudioBuffer<float>& ir, int size, int end)
        : block(size), bins(size + 1), partitions((end - 1) / size), fft(2 * size)
    {
        jassert(end > size);
        for (int c = 0; c < ir.getNumChannels(); ++c)
        {
            auto& channel = channels[(size_t) c];
            channel.ir.resize((size_t) partitions * (size_t) bins);
            channel.history.resize(channel.ir.size());
            channel.scratch.resize((size_t) block * 2);
            channel.input.resize((size_t) block);
            channel.output.resize((size_t) block);
            channel.overlap.resize((size_t) block);

            for (int p = 0; p < partitions; ++p)
            {
                const int offset = (p + 1) * block;
                std::fill(channel.scratch.begin(), channel.scratch.end(), Bin {});
                for (int i = 0; i < std::min(block, end - offset); ++i)
                    channel.scratch[(size_t) i].real = ir.getSample(c, offset + i);
                fft.run(channel.scratch, false);
                std::copy_n(channel.scratch.begin(), bins, channel.ir.begin() + p * bins);
            }
        }
    }

    void reset(int numChannels)
    {
        write = position = valid = 0;
        for (int c = 0; c < numChannels; ++c)
        {
            auto& channel = channels[(size_t) c];
            std::fill(channel.output.begin(), channel.output.end(), 0.0f);
            std::fill(channel.overlap.begin(), channel.overlap.end(), 0.0f);
        }
    }

    void process(const Frame& in, Frame& out, int numChannels)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            auto& channel = channels[(size_t) c];
            channel.input[(size_t) position] = in[(size_t) c];
            out[(size_t) c] += channel.output[(size_t) position];
        }

        if (++position < block)
            return;

        valid = std::min(valid + 1, partitions);
        for (int c = 0; c < numChannels; ++c)
            transform(channels[(size_t) c]);
        if (++write == partitions)
            write = 0;
        position = 0;
    }

private:
    struct Channel
    {
        std::vector<Bin> ir, history, scratch;
        std::vector<float> input, output, overlap;
    };

    void transform(Channel& channel) const
    {
        auto& data = channel.scratch;
        for (int i = 0; i < block; ++i)
            data[(size_t) i] = { channel.input[(size_t) i], 0.0f };
        std::fill(data.begin() + block, data.end(), Bin {});
        fft.run(data, false);
        std::copy_n(data.begin(), bins, channel.history.begin() + write * bins);

        std::fill_n(data.begin(), bins, Bin {});
        int read = write;
        for (int p = 0; p < valid; ++p)
        {
            const auto* input = channel.history.data() + read * bins;
            const auto* kernel = channel.ir.data() + p * bins;
            for (int i = 0; i < bins; ++i)
            {
                const auto product = multiply(input[i], kernel[i]);
                data[(size_t) i].real += product.real;
                data[(size_t) i].imag += product.imag;
            }
            if (--read < 0)
                read = partitions - 1;
        }

        for (int i = 1; i < block; ++i)
            data[data.size() - (size_t) i] = { data[(size_t) i].real, -data[(size_t) i].imag };
        fft.run(data, true);

        const float scale = 1.0f / (2.0f * (float) block);
        for (int i = 0; i < block; ++i)
        {
            channel.output[(size_t) i] = data[(size_t) i].real * scale + channel.overlap[(size_t) i];
            channel.overlap[(size_t) i] = data[(size_t) (i + block)].real * scale;
        }
    }

    int block, bins, partitions;
    FourierPlan fft;
    std::array<Channel, 2> channels;
    int position = 0, write = 0, valid = 0;
};

std::vector<float> resamplingTable(double ratio)
{
    constexpr int taps = 2 * resampleHalfTaps;
    std::vector<float> table((size_t) (resamplePhases + 1) * taps);
    const double cutoff = resampleCutoff * std::min(1.0, ratio);
    for (int p = 0; p <= resamplePhases; ++p)
    {
        auto* coefficients = table.data() + p * taps;
        double total = 0;
        for (int i = 0; i < taps; ++i)
        {
            const double x = (double) (i + 1 - resampleHalfTaps) - (double) p / resamplePhases;
            const double angle = std::numbers::pi * x;
            const double window = 0.5 + 0.5 * std::cos(angle / resampleHalfTaps);
            const double sinc = std::abs(x) < 1.0e-12 ? cutoff : std::sin(cutoff * angle) / angle;
            coefficients[i] = (float) (window * sinc);
            total += coefficients[i];
        }
        for (int i = 0; i < taps; ++i)
            coefficients[i] = (float) (coefficients[i] / total);
    }
    return table;
}

juce::AudioBuffer<float> makeImpulse(int type, double decayPercent, double stretchPercent,
                                    const juce::dsp::ProcessSpec& spec)
{
    const auto& response = responses[(size_t) type];
    const std::string key = std::string(response.name) + "_wav";
    int bytes = 0;
    const auto* resource = SpaceIRData::getNamedResource(key.c_str(), bytes);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(
        new juce::MemoryInputStream(resource, (size_t) bytes, false), true));

    const int channels = (int) spec.numChannels;
    if (!reader || reader->numChannels != 2)
    {
        jassertfalse;
        juce::AudioBuffer<float> silence(channels, 1);
        silence.clear();
        return silence;
    }

    const double stretch = std::exp2(stretchPercent / 100.0);
    const double ratio = spec.sampleRate * stretch / reader->sampleRate;
    const double rt60 = response.rt60Seconds * std::lerp(minDecayScale, maxDecayScale, decayPercent / 100.0);
    const double end = response.decayLeadInSeconds + rt60 * irFloorDb / -60.0;
    const int sourceLength = (int) reader->lengthInSamples;
    const int length = std::max(1, (int) std::ceil(std::min(sourceLength * ratio, end * stretch * spec.sampleRate)));
    const int fade = std::max(1, (int) std::lround(endFadeSeconds * spec.sampleRate));
    juce::AudioBuffer<float> source(2, sourceLength);
    reader->read(&source, 0, sourceLength, 0, true, true);

    if (channels == 1)
    {
        source.addFrom(0, 0, source, 1, 0, sourceLength);
        source.applyGain(0, 0, sourceLength, 0.5f);
    }

    juce::AudioBuffer<float> impulse(channels, length);
    const auto table = juce::approximatelyEqual(ratio, 1.0) ? std::vector<float>() : resamplingTable(ratio);
    constexpr int taps = 2 * resampleHalfTaps;
    const double timePerSample = 1.0 / (spec.sampleRate * stretch);
    const double exponent = std::log(0.001) / rt60;

    for (int i = 0; i < length; ++i)
    {
        const double gain = std::exp(exponent * std::max(0.0, i * timePerSample - response.decayLeadInSeconds));
        const double taper = std::min(1.0, (double) (length - 1 - i) / fade);
        const float scale = (float) (gain * taper / ratio);
        const double at = i / ratio;
        const int index = (int) at;
        const int phase = (int) std::lround((at - index) * resamplePhases);
        for (int c = 0; c < channels; ++c)
        {
            const auto* input = source.getReadPointer(c);
            float sample = 0.0f;
            if (table.empty())
                sample = input[index];
            else
            {
                const auto* weights = table.data() + phase * taps;
                const int start = index + 1 - resampleHalfTaps;
                for (int j = std::max(0, -start); j < std::min(taps, sourceLength - start); ++j)
                    sample += weights[j] * input[start + j];
            }
            impulse.setSample(c, i, sample * scale);
        }
    }
    return impulse;
}
} // namespace

class SpaceConvolver
{
public:
    explicit SpaceConvolver(const juce::AudioBuffer<float>& ir)
        : channels(ir.getNumChannels()), taps(std::min(ir.getNumSamples(), headSize))
    {
        for (int c = 0; c < channels; ++c)
            std::copy_n(ir.getReadPointer(c), taps, head[(size_t) c].begin());

        tiers.reserve(partitionSizes.size());
        for (size_t i = 0; i < partitionSizes.size(); ++i)
        {
            const int size = partitionSizes[i];
            if (ir.getNumSamples() <= size)
                break;
            const int end = i + 1 < partitionSizes.size() ? std::min(ir.getNumSamples(), partitionSizes[i + 1])
                                                          : ir.getNumSamples();
            tiers.emplace_back(ir, size, end);
        }
    }

    void reset()
    {
        history = {};
        position = 0;
        for (auto& tier : tiers)
            tier.reset(channels);
    }

    Frame process(const Frame& input)
    {
        position = (position + headSize - 1) & (headSize - 1);
        Frame output {};
        for (int c = 0; c < channels; ++c)
        {
            auto& past = history[(size_t) c];
            past[(size_t) position] = past[(size_t) (position + headSize)] = input[(size_t) c];
            const auto* samples = past.data() + position;
            const auto* kernel = head[(size_t) c].data();
            for (int i = 0; i < taps; ++i)
                output[(size_t) c] += samples[i] * kernel[i];
        }
        for (auto& tier : tiers)
            tier.process(input, output, channels);
        return output;
    }

private:
    static_assert(std::has_single_bit((unsigned) headSize));
    int channels, taps, position = 0;
    std::array<std::array<float, headSize>, 2> head {};
    std::array<std::array<float, 2 * headSize>, 2> history {};
    std::vector<PartitionTier> tiers;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

Space::Space() = default;
Space::~Space() { stopWorker(); }

void Space::stopWorker()
{
    if (!worker.joinable())
        return;
    stopRequested.store(true, std::memory_order_relaxed);
    worker.join();
}

Space::Request Space::encode(const Settings& settings)
{
    const auto type = (Request) std::clamp(settings.type, 0, (int) responses.size() - 1);
    const auto decay = (Request) std::lround(std::clamp(settings.decay, 0.0f, 100.0f) * requestStepsPerPercent);
    const auto stretch = (Request) std::lround((std::clamp(settings.stretch, -100.0f, 100.0f) + 100.0f)
                                               * requestStepsPerPercent);
    return (type << 32) | (decay << 16) | stretch;
}

void Space::buildResponses()
{
    while (!stopRequested.load(std::memory_order_relaxed))
    {
        const auto request = requested.load(std::memory_order_relaxed);
        const auto alreadyBuilt = [&](const Slot& slot)
        {
            return slot.occupied.load(std::memory_order_acquire) && slot.request == request;
        };
        if (std::none_of(slots.begin(), slots.end(), alreadyBuilt))
            for (auto& slot : slots)
                if (!slot.occupied.load(std::memory_order_acquire))
                {
                    const auto type = (int) (request >> 32);
                    const double decay = (double) ((request >> 16) & 0xffff) / requestStepsPerPercent;
                    const double stretch = (double) (request & 0xffff) / requestStepsPerPercent - 100.0;
                    slot.engine = std::make_unique<SpaceConvolver>(makeImpulse(type, decay, stretch, preparedSpec));
                    slot.request = request;
                    slot.occupied.store(true, std::memory_order_release);
                    responseReady.signal();
                    break;
                }
        std::this_thread::sleep_for(std::chrono::milliseconds(workerPollMs));
    }
}

void Space::prepare(const juce::dsp::ProcessSpec& spec, const Settings& initial)
{
    jassert(spec.numChannels == 1 || spec.numChannels == 2);
    stopWorker();
    preparedSpec = spec;
    for (auto& slot : slots)
    {
        slot.engine.reset();
        slot.occupied.store(false, std::memory_order_relaxed);
    }

    active = 0;
    transitioning = false;
    idle = !initial.enabled || initial.amount <= 0.0f;
    warmupLength = (int) std::lround(spec.sampleRate * warmupSeconds);
    fadeLength = std::max(2, (int) std::lround(spec.sampleRate * crossfadeSeconds));
    highpass.setCutoff(std::min(initial.focusLoHz, initial.focusHiHz));
    lowpass.setCutoff(std::max(initial.focusLoHz, initial.focusHiHz));
    highpass.prepare(spec.sampleRate, (int) spec.numChannels);
    lowpass.prepare(spec.sampleRate, (int) spec.numChannels);
    ducker.prepare(spec.sampleRate);
    delayRing.setSize((int) spec.numChannels, (int) std::ceil(spec.sampleRate * maxPredelayMs / 1000.0) + 1);
    for (auto* control : { &amount, &duck, &pan, &width, &delayBlend })
        control->prepare(spec.sampleRate);

    setContinuousTargets(initial);
    amount.reset(amount.targetValue());
    requested.store(encode(initial), std::memory_order_relaxed);
    responseReady.reset();
    stopRequested.store(false, std::memory_order_relaxed);
    worker = std::thread([this] { buildResponses(); });
    (void) responseReady.wait();
    clearHistory();
}

void Space::clearHistory()
{
    if (slots[active].engine == nullptr)
        return;
    slots[active].engine->reset();
    highpass.reset();
    lowpass.reset();
    ducker.reset();
    delayRing.clear();
    delayWrite = 0;
    delayFrom = delayTo = delayTarget;
    delayBlend.reset(1.0f);
    for (auto* control : { &duck, &pan, &width })
        control->reset(control->targetValue());
}

void Space::reset()
{
    if (transitioning)
        finishTransition();
    amount.reset(amount.targetValue());
    idle = amount.value() <= 0.0f;
    clearHistory();
}

void Space::setContinuousTargets(const Settings& settings)
{
    amount.setTarget(settings.enabled ? std::clamp(settings.amount, 0.0f, 100.0f) * percentScale : 0.0f);
    duck.setTarget(std::clamp(settings.duck, 0.0f, 100.0f) * percentScale);
    pan.setTarget(std::clamp(settings.pan, -100.0f, 100.0f) * percentScale);
    width.setTarget(std::clamp(settings.width, -100.0f, 100.0f) * percentScale);
    highpass.setCutoff(std::min(settings.focusLoHz, settings.focusHiHz));
    lowpass.setCutoff(std::max(settings.focusLoHz, settings.focusHiHz));
    delayTarget = (int) std::lround(preparedSpec.sampleRate * std::clamp(settings.predelayMs, 0.0f, maxPredelayMs) / 1000.0);
}

void Space::startTransition(Request request)
{
    auto& other = slots[1 - active];
    if (transitioning)
    {
        if (warmupLeft == 0 || other.request == request)
            return;
        transitioning = false;
    }
    if (!other.occupied.load(std::memory_order_acquire))
        return;
    if (other.request != request)
    {
        other.occupied.store(false, std::memory_order_release);
        return;
    }
    if (idle)
    {
        finishTransition();
        return;
    }
    warmupLeft = warmupLength;
    fadePosition = 0;
    transitioning = true;
}

void Space::finishTransition()
{
    slots[active].occupied.store(false, std::memory_order_release);
    active = 1 - active;
    transitioning = false;
}

void Space::process(juce::AudioBuffer<float>& io, const Settings& settings)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = io.getNumChannels();
    jassert(channels == (int) preparedSpec.numChannels);

    const auto request = encode(settings);
    requested.store(request, std::memory_order_relaxed);
    setContinuousTargets(settings);
    if (amount.value() <= 0.0f && !amount.isSmoothing())
    {
        if (!idle)
        {
            idle = true;
            reset();
        }
        startTransition(request);
        return;
    }

    startTransition(request);
    if (idle)
    {
        delayFrom = delayTo = delayTarget;
        idle = false;
    }

    const int ringSize = delayRing.getNumSamples();
    auto* const* ring = delayRing.getArrayOfWritePointers();
    auto* const* audio = io.getArrayOfWritePointers();
    for (int i = 0; i < io.getNumSamples(); ++i)
    {
        if (!delayBlend.isSmoothing() && delayTo != delayTarget)
        {
            delayFrom = delayTo;
            delayTo = delayTarget;
            delayBlend.reset(0.0f);
            delayBlend.setTarget(1.0f);
        }

        highpass.step();
        lowpass.step();
        const float blend = delayBlend.next();
        const auto fromIndex = (delayWrite + ringSize - delayFrom) % ringSize;
        const auto toIndex = (delayWrite + ringSize - delayTo) % ringSize;
        Frame send {};
        float key = 0.0f;
        for (int c = 0; c < channels; ++c)
        {
            const float input = audio[c][i];
            key = std::max(key, std::abs(input));
            float unused, high, band;
            highpass.split(c, input, unused, high);
            lowpass.split(c, high, band, unused);
            ring[c][delayWrite] = band;
            send[(size_t) c] = std::lerp(ring[c][fromIndex], ring[c][toIndex], blend);
        }
        if (++delayWrite == ringSize)
            delayWrite = 0;

        Frame wet = slots[active].engine->process(send);
        if (transitioning)
        {
            const auto next = slots[1 - active].engine->process(send);
            if (warmupLeft > 0)
                --warmupLeft;
            else
            {
                const float phase = (float) fadePosition / (float) (fadeLength - 1);
                const float fade = 0.5f - 0.5f * std::cos(std::numbers::pi_v<float> * phase);
                for (int c = 0; c < channels; ++c)
                    wet[(size_t) c] = std::lerp(wet[(size_t) c], next[(size_t) c], fade);
                if (++fadePosition == fadeLength)
                    finishTransition();
            }
        }

        const float gain = amount.next() * ducker.next(key, duck.next());
        const float stereoWidth = width.next(), balance = pan.next();
        if (channels == 2)
        {
            const float middle = 0.5f * (wet[0] + wet[1]);
            const float side = 0.5f * (wet[0] - wet[1]) * stereoWidth;
            wet[0] = (middle + side) * (1.0f - std::max(balance, 0.0f));
            wet[1] = (middle - side) * (1.0f + std::min(balance, 0.0f));
        }
        if (gain > 0.0f)
            for (int c = 0; c < channels; ++c)
                audio[c][i] += wet[(size_t) c] * gain;
    }
}

} // namespace snap
