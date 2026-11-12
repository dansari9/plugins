#include "TestFramework.h"

#if defined(__SSE__) || defined(_M_X64)
 #include <xmmintrin.h>
#endif

int main(int argc, char** argv)
{
   #if defined(__SSE__) || defined(_M_X64)
    // The plug-in runs inside juce::ScopedNoDenormals. Match that here so
    // decaying filter tails do not make the tests crawl on x86.
    _mm_setcsr(_mm_getcsr() | 0x8040u);
   #endif

    return testing::runAll(argc, argv);
}
