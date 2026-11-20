#pragma once
#include <iostream>

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
