# Include after Snap and SnapTests are declared in Snap/CMakeLists.txt.
set(snap_space_root "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB snap_space_irs CONFIGURE_DEPENDS "${snap_space_root}/Resources/irs/*.wav")
juce_add_binary_data(SnapSpaceIRs
    HEADER_NAME SpaceIRData.h
    NAMESPACE SpaceIRData
    SOURCES ${snap_space_irs}
)
target_sources(Snap PRIVATE "${snap_space_root}/Source/dsp/modules/Space.cpp")
target_link_libraries(Snap PRIVATE SnapSpaceIRs)
target_sources(SnapTests PRIVATE "${snap_space_root}/Tests/SpaceTests.cpp")
target_link_libraries(SnapTests PRIVATE SnapSpaceIRs)
