/**
 * Wayland keyboard test suite for fizzy's patches
 *
 * "Wayland: xkb_keymap_mod_get_mask() is optional at runtime": libxkbcommon is
 * loaded at runtime, but a build against libxkbcommon 1.10 or newer required
 * xkb_keymap_mod_get_mask() (new in 1.10), so the whole Wayland driver failed
 * to load where the system's libxkbcommon is older (Ubuntu 24.04 has 1.6).
 *
 * xkb_waylandDriverLoads starts the Wayland driver whenever there is a
 * compositor to connect to (WAYLAND_DISPLAY is set). xkb_keymapModifierLevels
 * checks the modifier masks the keymap handler computed, through the SDL keymap
 * they built: each level of a key is entered under the SDL modifiers its xkb
 * modifier mask translates to, so a wrong mask drops or misfiles the level. It
 * needs a compositor with a keyboard and the layout named by the
 * FIZZY_TEST_XKB_LAYOUT environment variable; only "de" is known, and without
 * the variable the test is skipped.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_test.h>
#include "testautomation_fizzy.h"

/* Private helpers */

static bool VideoDriverIsCompiled(const char *name)
{
    int i;

    for (i = 0; i < SDL_GetNumVideoDrivers(); ++i) {
        if (SDL_strcmp(SDL_GetVideoDriver(i), name) == 0) {
            return true;
        }
    }
    return false;
}

/* How many times the runner had initialized the video subsystem, and with
 * which driver hint */
static int videoRefCount = 0;
static char *videoDriverHint = NULL;

/**
 * Restarts the video subsystem with the Wayland driver. The runner initializes
 * video more than once, so it is quit until it is down. The hint overrides
 * SDL_VIDEO_DRIVER from the environment, which the runner may have started
 * with another driver.
 */
static bool StartWaylandVideo(void)
{
    bool result;

    SDL_free(videoDriverHint);
    videoDriverHint = SDL_GetHint(SDL_HINT_VIDEO_DRIVER) ? SDL_strdup(SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) : NULL;
    videoRefCount = 0;
    while (SDL_WasInit(SDL_INIT_VIDEO) && videoRefCount < 255) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        ++videoRefCount;
    }
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "wayland", SDL_HINT_OVERRIDE);
    result = SDL_InitSubSystem(SDL_INIT_VIDEO);
    SDLTest_AssertPass("Call to SDL_InitSubSystem(SDL_INIT_VIDEO) with the 'wayland' driver");
    SDLTest_AssertCheck(result, "Validate that the Wayland video driver started, got error: %s", result ? "none" : SDL_GetError());
    if (result) {
        const char *driver = SDL_GetCurrentVideoDriver();
        SDLTest_AssertCheck(driver && SDL_strcmp(driver, "wayland") == 0, "Validate the current video driver, expected: wayland, got: %s", driver ? driver : "(null)");
    }
    return result;
}

/**
 * Puts back the video driver the runner started with, as many times over.
 */
static void RestoreVideo(void)
{
    int i;

    while (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    if (videoDriverHint) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, videoDriverHint);
        SDL_free(videoDriverHint);
        videoDriverHint = NULL;
    }
    for (i = 0; i < videoRefCount; ++i) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            SDLTest_LogError("Couldn't restart the video subsystem: %s", SDL_GetError());
            break;
        }
    }
}

static void PumpEventsFor(Uint64 ms)
{
    const Uint64 end = SDL_GetTicks() + ms;
    SDL_Event event;

    while (SDL_GetTicks() < end) {
        while (SDL_PollEvent(&event)) {
        }
        SDL_Delay(10);
    }
}

static void CheckKey(SDL_Scancode scancode, SDL_Keymod modstate, SDL_Keycode expected, const char *why)
{
    const SDL_Keycode key = SDL_GetKeyFromScancode(scancode, modstate, false);

    SDLTest_AssertCheck(key == expected, "Validate %s with modifiers 0x%.4x (%s), expected: 0x%" SDL_PRIx32 ", got: 0x%" SDL_PRIx32,
                        SDL_GetScancodeName(scancode), (unsigned int)modstate, why, (Uint32)expected, (Uint32)key);
}

/* Test case functions */

/**
 * Starts the Wayland video driver whenever a compositor is there to connect to.
 */
static int SDLCALL xkb_waylandDriverLoads(void *arg)
{
    if (!VideoDriverIsCompiled("wayland")) {
        SDLTest_Log("The Wayland video driver isn't compiled in, skipping test");
        return TEST_SKIPPED;
    }
    if (!SDL_getenv("WAYLAND_DISPLAY")) {
        SDLTest_Log("WAYLAND_DISPLAY isn't set, so there is no compositor to connect to, skipping test");
        return TEST_SKIPPED;
    }

    StartWaylandVideo();
    RestoreVideo();
    return TEST_COMPLETED;
}

/**
 * Checks the keymap the Wayland driver built from the compositor's, for the
 * levels that depend on the shift, caps lock and level 3 masks.
 */
static int SDLCALL xkb_keymapModifierLevels(void *arg)
{
    const char *layout = SDL_getenv("FIZZY_TEST_XKB_LAYOUT");
    SDL_Window *window;
    Uint64 deadline;
    SDL_Keymod modstate;

    if (!layout) {
        SDLTest_Log("FIZZY_TEST_XKB_LAYOUT isn't set, skipping test");
        return TEST_SKIPPED;
    }
    if (SDL_strcmp(layout, "de") != 0) {
        SDLTest_Log("No expectations for the keyboard layout '%s', skipping test", layout);
        return TEST_SKIPPED;
    }
    if (!VideoDriverIsCompiled("wayland")) {
        SDLTest_Log("The Wayland video driver isn't compiled in, skipping test");
        return TEST_SKIPPED;
    }

    if (!StartWaylandVideo()) {
        RestoreVideo();
        return TEST_COMPLETED;
    }

    window = SDL_CreateWindow("testautomation_fizzy_xkb", 320, 240, 0);
    SDLTest_AssertPass("Call to SDL_CreateWindow('Title',320,240,0)");
    SDLTest_AssertCheck(window != NULL, "Validate that returned window is not NULL, got error: %s", window ? "none" : SDL_GetError());
    if (!window) {
        RestoreVideo();
        return TEST_COMPLETED;
    }

    /* The compositor sends the keymap as soon as the seat's keyboard is bound */
    deadline = SDL_GetTicks() + 3000;
    while (!SDL_HasKeyboard() && SDL_GetTicks() < deadline) {
        PumpEventsFor(10);
    }
    SDLTest_AssertCheck(SDL_HasKeyboard(), "Validate that the compositor's seat has a keyboard");
    PumpEventsFor(250);

    /* Shift (level 2) */
    CheckKey(SDL_SCANCODE_Y, SDL_KMOD_NONE, 'z', "the compositor's keymap is in use");
    CheckKey(SDL_SCANCODE_2, SDL_KMOD_NONE, '2', "level 1");
    CheckKey(SDL_SCANCODE_2, SDL_KMOD_LSHIFT, '"', "the shift mask");
    CheckKey(SDL_SCANCODE_3, SDL_KMOD_RSHIFT, 0xa7 /* section sign */, "the shift mask");

    /* Caps lock (level 2 of an alphabetic key only) */
    CheckKey(SDL_SCANCODE_Q, SDL_KMOD_CAPS, 'Q', "the caps lock mask");
    CheckKey(SDL_SCANCODE_2, SDL_KMOD_CAPS, '2', "the caps lock mask");

    /* AltGr (level 3): LevelThree, which the standard keymaps put on Mod5 */
    CheckKey(SDL_SCANCODE_Q, SDL_KMOD_MODE, '@', "the level 3 mask");
    CheckKey(SDL_SCANCODE_E, SDL_KMOD_MODE, 0x20ac /* euro sign */, "the level 3 mask");
    CheckKey(SDL_SCANCODE_7, SDL_KMOD_MODE, '{', "the level 3 mask");

    /* Nothing is held down. Whether the window has keyboard focus, and so got
     * the compositor's modifier state, depends on the compositor. */
    deadline = SDL_GetTicks() + 2000;
    while (SDL_GetKeyboardFocus() != window && SDL_GetTicks() < deadline) {
        PumpEventsFor(10);
    }
    modstate = SDL_GetModState();
    SDLTest_Log("Keyboard focus: %s, modifier state: 0x%.4x", SDL_GetKeyboardFocus() == window ? "yes" : "no", (unsigned int)modstate);
    SDLTest_AssertCheck((modstate & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI | SDL_KMOD_MODE | SDL_KMOD_LEVEL5)) == 0,
                        "Validate that no modifier is held, got modifier state: 0x%.4x", (unsigned int)modstate);

    SDL_DestroyWindow(window);
    RestoreVideo();
    return TEST_COMPLETED;
}

/* ================= Test References ================== */

/* Wayland keyboard test cases */
static const SDLTest_TestCaseReference xkbTestWaylandDriverLoads = {
    xkb_waylandDriverLoads, "xkb_waylandDriverLoads", "Starts the Wayland video driver when a compositor is there", TEST_ENABLED
};

static const SDLTest_TestCaseReference xkbTestKeymapModifierLevels = {
    xkb_keymapModifierLevels, "xkb_keymapModifierLevels", "Checks the shift, caps lock and level 3 levels of the compositor's keymap", TEST_ENABLED
};

/* Sequence of Wayland keyboard test cases */
static const SDLTest_TestCaseReference *xkbTests[] = {
    &xkbTestWaylandDriverLoads,
    &xkbTestKeymapModifierLevels,
    NULL
};

/* Wayland keyboard test suite (global) */
SDLTest_TestSuiteReference fizzyXkbTestSuite = {
    "FizzyXkb",
    NULL,
    xkbTests,
    NULL
};
