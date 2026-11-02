#include "PluginEditor.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

namespace
{
int failures = 0;

void expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

juce::Component* requireChild(juce::Component& editor, const juce::String& id)
{
    auto* child = editor.findChildWithID(id);
    expect(child != nullptr, "missing component " + id.toStdString());
    if (child == nullptr)
        return nullptr;

    expect(child->getWidth() > 8 && child->getHeight() > 8,
           id.toStdString() + " has no usable bounds ("
               + std::to_string(child->getWidth()) + "x" + std::to_string(child->getHeight()) + ")");
    return child;
}

int countSliders(juce::Component& component)
{
    int count = dynamic_cast<juce::Slider*>(&component) != nullptr ? 1 : 0;
    for (auto* child : component.getChildren())
        count += countSliders(*child);
    return count;
}

bool imageHasPaint(const juce::Image& image)
{
    if (!image.isValid() || image.getWidth() < 16 || image.getHeight() < 16)
        return false;

    juce::Colour first = image.getPixelAt(8, 8);
    for (int y = 0; y < image.getHeight(); y += 6)
    {
        for (int x = 0; x < image.getWidth(); x += 6)
        {
            const auto pixel = image.getPixelAt(x, y);
            if (pixel.getAlpha() > 0
                && (std::abs(pixel.getRed() - first.getRed()) > 12
                    || std::abs(pixel.getGreen() - first.getGreen()) > 12
                    || std::abs(pixel.getBlue() - first.getBlue()) > 12))
                return true;
        }
    }
    return false;
}

bool writePng(const juce::File& file, const juce::Image& image)
{
    file.getParentDirectory().createDirectory();
    if (file.existsAsFile())
        file.deleteFile();

    juce::FileOutputStream stream(file);
    if (stream.failedToOpen())
        return false;

    juce::PNGImageFormat format;
    return format.writeImageToStream(image, stream);
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceGui;

    auto tank = std::make_unique<SpringTankProcessor>();
    juce::AudioProcessor& processor = *tank;

    expect(processor.hasEditor(), "hasEditor() must be true");
    expect(processor.getNumPrograms() == 4, "expected 4 factory programs");
    expect(processor.getProgramName(0) == "Surf Guitar", "program 0 name");
    expect(processor.getProgramName(1) == "Dub Splash", "program 1 name");
    expect(processor.getProgramName(2) == "Subtle Room Spring", "program 2 name");
    expect(processor.getProgramName(3) == "Vocal Shimmer", "program 3 name");
    expect(processor.getBypassParameter() != nullptr, "bypass parameter missing");

    processor.setCurrentProgram(1);
    const float dubDecay = tank->getApvts().getRawParameterValue("decay")->load();
    expect(std::abs(dubDecay - 5.0f) < 0.08f, "Dub Splash did not set decay");
    processor.setCurrentProgram(2);
    const float roomDecay = tank->getApvts().getRawParameterValue("decay")->load();
    expect(roomDecay < 1.2f, "Subtle Room Spring decay should be short");
    processor.setCurrentProgram(0);

    std::unique_ptr<juce::AudioProcessorEditor> editor(tank->createEditor());
    expect(editor != nullptr, "createEditor() returned null");
    if (editor == nullptr)
        return 1;

    expect(editor->getWidth() == SpringTankEditor::kWidth, "editor width is not the default");
    expect(editor->getHeight() == SpringTankEditor::kHeight, "editor height is not the default");
    expect(editor->isResizable(), "editor should be resizable");

    const char* ids[] = {
        "knob-decay", "knob-tension", "knob-tone", "knob-drip",
        "knob-predelay", "knob-mix", "knob-output",
        "preset-box", "preset-prev", "preset-next",
        "bypass", "meter-in", "meter-out"
    };
    for (const char* id : ids)
        requireChild(*editor, id);

    expect(countSliders(*editor) >= 7, "expected a slider inside every knob");

    const auto snapshot = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
    expect(snapshot.isValid(), "snapshot was not created");
    expect(snapshot.getWidth() == editor->getWidth() && snapshot.getHeight() == editor->getHeight(),
           "snapshot size does not match the editor");
    expect(imageHasPaint(snapshot), "snapshot looks blank");

    if (argc >= 2)
    {
        const juce::File png(juce::String::fromUTF8(argv[1]));
        expect(writePng(png, snapshot), "could not write " + png.getFullPathName().toStdString());
        if (failures == 0)
            std::cout << "wrote " << png.getFullPathName() << "\n";
    }

    if (failures != 0)
    {
        std::cerr << failures << " editor check(s) failed\n";
        return 1;
    }

    std::cout << "spring tank editor checks passed\n";
    return 0;
}
