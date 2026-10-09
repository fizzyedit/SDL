/**
 * Test suites for fizzy's patches to SDL.
 *
 * Each suite checks one patch on the fizzy-3.4 branch of fizzyedit/SDL and is
 * expected to fail on the SDL release the branch is based on. testfizzy.c runs
 * them.
 */

#ifndef testautomation_fizzy_h_
#define testautomation_fizzy_h_

#include <SDL3/SDL_test.h>

/* Test collections */
extern SDLTest_TestSuiteReference fizzyGPUTestSuite;

/* Comma-separated GPU drivers ("metal,vulkan") a test must find: when one of
 * them is missing the test fails instead of being skipped. Set by testfizzy's
 * --require-gpu option, for CI runners that are known to have the driver.
 */
extern const char *fizzyGPURequiredDrivers;

#endif /* testautomation_fizzy_h_ */
