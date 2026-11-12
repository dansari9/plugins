
// ---- fixture generator (worktree only) ----
static int writeOldState(const char* path)
{
    RootnoteProcessor processor;
    const auto setValue = [&](const char* id, float v) {
        auto* p = processor.getApvts().getParameter(id);
        p->setValueNotifyingHost(p->convertTo0to1(v));
    };
    processor.loadPreset(3); // Snare Core
    setValue("boost_note", 31.25f);
    setValue("boost_fine", -12.5f);
    setValue("boost_gain", 14.2f);
    setValue("boost_q", 33.0f);
    setValue("bands", 11.0f);
    setValue("blend", 0.3f);
    setValue("focus_link", 0.0f);
    setValue("focus", 140.0f);
    setValue("range", 2.25f);
    setValue("tilt", -4.5f);
    setValue("q_mode", 1.0f);
    setValue("cut_on", 1.0f);
    setValue("cut_note", 40.5f);
    setValue("cut_depth", -21.0f);
    setValue("cut_q", 15.0f);
    setValue("cut_overtones", 35.0f);
    setValue("cut_bands", 9.0f);
    setValue("ring", 45.0f);
    setValue("ring_decay", 1200.0f);
    setValue("ring_drop", 3.5f);
    setValue("midi_mode", 2.0f);
    setValue("glide", 80.0f);
    setValue("note_off", 1.0f);
    setValue("key", 9.0f);
    setValue("scale", 7.0f);
    setValue("key_lock", 1.0f);
    setValue("a4", 432.0f);
    setValue("mix", 85.0f);
    setValue("output", -4.5f);
    setValue("limiter", 0.0f);
    setValue("bypass", 0.0f);
    processor.storeEditorSize(1200, 675);
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    return juce::File(path).replaceWithData(state.getData(), state.getSize()) ? 0 : 1;
}
