#pragma once
#include <iostream>
namespace juce { class File; }

// Shared by the test files: failure count, CHECK, and the audio-thread allocation probe (defined in SnapTests.cpp).
inline int failures = 0;
#define CHECK(c) ((c) ? (void) 0 : (void) (++failures, std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #c "\n"))
void setAudioThread(bool);   // allocations on this thread count while set
int takeAudioAllocations(); // returns the count and resets it

void testDsp();   // DspTests.cpp
void testPress(); // PressTests.cpp
void testDrive(); // DriveTests.cpp
void testPush();  // PushTests.cpp
void testShift(); // ShiftTests.cpp
void testMaster(); // MasterTests.cpp
void testSpace();  // SpaceTests.cpp
void testSubHiss(); // SubHissTests.cpp
int renderSpace(const juce::File&);   // SpaceTests.cpp: --render-space <dir>
int renderSubHiss(const juce::File&); // SubHissTests.cpp: --render-subhiss <dir>
