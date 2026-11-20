# Include after Snap and SnapTests are declared in Snap/CMakeLists.txt.
set(snap_subhiss_root "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB snap_subhiss_noise CONFIGURE_DEPENDS "${snap_subhiss_root}/Resources/noise/*.wav")
juce_add_binary_data(SnapSubHissNoise
    HEADER_NAME SubHissNoiseData.h
    NAMESPACE SubHissNoiseData
    SOURCES ${snap_subhiss_noise}
)
target_sources(Snap PRIVATE "${snap_subhiss_root}/Source/dsp/modules/SubHiss.cpp")
target_link_libraries(Snap PRIVATE SnapSubHissNoise)
target_sources(SnapTests PRIVATE "${snap_subhiss_root}/Tests/SubHissTests.cpp")
