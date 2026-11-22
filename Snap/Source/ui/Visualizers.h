#pragma once
#include "Style.h"

class SnapProcessor;

// A module's 217 x 104 visualizer (feedback, not metering). The editor calls update() at 30 Hz with the levels it took
// from the processor's display feed this tick; each visualizer also reads its own parameters and the feed's module
// readouts, animates, and repaints itself. Message thread only; reads the audio thread through atomics alone.
namespace snap::ui
{
struct Levels
{
    float in = 0, out = 0; // linear peaks since the previous tick
    float spaceWet = 0, driveIn = 0, driveAdded = 0, pushIn = 0, pushAdded = 0; // the modules' own, likewise
    bool flowing = false; // audio arrived since the previous tick (a stopped host stops calling the processor)
};

class Visualizer : public juce::Component
{
public:
    Visualizer(SnapProcessor&, Module);
    ~Visualizer() override;
    void update(const Levels&);
    bool dim = false; // the module is switched off or the plug-in bypassed: the picture settles and stays still
    int tab = 0;      // sub/hiss: 0 sub, 1 hiss (whose sensitivity and detect range are drawn)
    void paint(juce::Graphics&) override;

private:
    struct State;
    SnapProcessor& proc;
    Module module;
    std::unique_ptr<State> state;
};
} // namespace snap::ui
