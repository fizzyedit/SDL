/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Runs the suites for fizzy's patches (testautomation_fizzy*.c), the way
 * testautomation runs SDL's own: --filter, --seed and --list work the same.
 */

#include <stdlib.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_test.h>

#include "testautomation_fizzy.h"

static SDLTest_CommonState *state;
static SDLTest_TestSuiteRunner *runner;

/* All test suites */
static SDLTest_TestSuiteReference *testSuites[] = {
    &fizzyGPUTestSuite,
    &fizzyXkbTestSuite,
    &fizzyWaylandTestSuite,
    NULL
};

/* Call this instead of exit(), so we can clean up SDL: atexit() is evil. */
static void
quit(int rc)
{
    SDLTest_DestroyTestSuiteRunner(runner);
    SDLTest_CommonQuit(state);
    /* Let 'main()' return normally */
    if (rc != 0) {
        exit(rc);
    }
}

int main(int argc, char *argv[])
{
    int result;
    int i, done;
    SDL_Event event;
    int list = 0;

    /* Initialize test framework */
    state = SDLTest_CommonCreateState(argv, SDL_INIT_VIDEO);
    if (!state) {
        return 1;
    }

    /* The suites create the windows they need */
    state->num_windows = 0;

    runner = SDLTest_CreateTestSuiteRunner(state, testSuites);

    /* Parse commandline */
    for (i = 1; i < argc;) {
        int consumed;

        consumed = SDLTest_CommonArg(state, i);
        if (consumed == 0) {
            consumed = -1;

            if (SDL_strcasecmp(argv[i], "--list") == 0) {
                consumed = 1;
                list = 1;
            } else if (SDL_strcasecmp(argv[i], "--require-gpu") == 0 && argv[i + 1]) {
                consumed = 2;
                fizzyGPURequiredDrivers = argv[i + 1];
            }
        }
        if (consumed < 0) {
            static const char *options[] = {
                "[--list]",
                "[--require-gpu driver[,driver]]",
                NULL };
            SDLTest_CommonLogUsage(state, argv[0], options);
            quit(1);
        }

        i += consumed;
    }

    /* List all suites. */
    if (list) {
        int suiteCounter;
        for (suiteCounter = 0; testSuites[suiteCounter]; ++suiteCounter) {
            int testCounter;
            SDLTest_TestSuiteReference *testSuite = testSuites[suiteCounter];
            SDL_Log("Test suite: %s", testSuite->name);
            for (testCounter = 0; testSuite->testCases[testCounter]; ++testCounter) {
                const SDLTest_TestCaseReference *testCase = testSuite->testCases[testCounter];
                SDL_Log("      test: %s%s", testCase->name, testCase->enabled ? "" : " (disabled)");
            }
        }
        return 0;
    }

    /* Initialize common state */
    if (!SDLTest_CommonInit(state)) {
        quit(2);
    }

    /* Call Harness */
    result = SDLTest_ExecuteTestSuiteRunner(runner);

    /* Empty event queue */
    done = 0;
    for (i = 0; i < 100; i++) {
        while (SDL_PollEvent(&event)) {
            SDLTest_CommonEvent(state, &event, &done);
        }
        SDL_Delay(10);
    }

    /* Shutdown everything */
    quit(0);
    return result;
}
