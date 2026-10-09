/**
 * Wayland test suite for fizzy's patches
 *
 * "Wayland: frame insets, for a shadow the application draws round its own
 * decorations": SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_*_NUMBER give the
 * margins of an xdg-shell toplevel's surface outside its frame. While the
 * window floats, the window geometry is the frame, the input region is the
 * frame and a band of at most 8 units round it, the min/max sizes are the
 * frame's and popups anchor to the frame; maximized or fullscreen the insets
 * are zero. The insets in effect are published as
 * SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_*_NUMBER.
 *
 * What SDL asks of the compositor is read from libwayland's own trace of the
 * requests it sends (WAYLAND_DEBUG=client), not from SDL's state: each test
 * reconnects SDL to the compositor with the trace on and stderr captured to a
 * file, does its window operations, then parses the requests sent on its own
 * objects (found by id through wl_proxy_get_id).
 *
 * The tests run only on the Wayland video driver, and skip elsewhere. They need
 * a GPU driver to present frames with (lavapipe will do), so the windows map.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_test.h>
#include "testautomation_fizzy.h"

#ifdef SDL_PLATFORM_LINUX
#include <stdio.h>
#include <unistd.h>
#define FIZZY_WAYLAND_TRACE 1
#endif

/* The patch's properties, for SDL releases without them: there, creating a
 * window ignores them and none are published, so every test fails on its
 * first check instead of failing to build.
 */
#ifndef SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_LEFT_NUMBER
#define SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_LEFT_NUMBER   "SDL.window.create.wayland.frame_inset.left"
#define SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_TOP_NUMBER    "SDL.window.create.wayland.frame_inset.top"
#define SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_RIGHT_NUMBER  "SDL.window.create.wayland.frame_inset.right"
#define SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_BOTTOM_NUMBER "SDL.window.create.wayland.frame_inset.bottom"
#endif
#ifndef SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_LEFT_NUMBER
#define SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_LEFT_NUMBER   "SDL.window.wayland.frame_inset.left"
#define SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_TOP_NUMBER    "SDL.window.wayland.frame_inset.top"
#define SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_RIGHT_NUMBER  "SDL.window.wayland.frame_inset.right"
#define SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_BOTTOM_NUMBER "SDL.window.wayland.frame_inset.bottom"
#endif

#ifdef FIZZY_WAYLAND_TRACE

/* Insets that differ on every side, one of them under the 8-unit input band,
 * round a 400x300 surface: the frame is 346x254 at (24, 6).
 */
enum
{
    INSET_LEFT = 24,
    INSET_TOP = 6,
    INSET_RIGHT = 30,
    INSET_BOTTOM = 40,
    INPUT_BAND = 8,
    SURFACE_W = 400,
    SURFACE_H = 300
};

static const char *inset_names[4] = { "left", "top", "right", "bottom" };
static const int insets_asked[4] = { INSET_LEFT, INSET_TOP, INSET_RIGHT, INSET_BOTTOM };
static const int insets_zero[4] = { 0, 0, 0, 0 };

typedef struct
{
    int x, y, w, h;
} TraceRect;

/* What the trace says SDL last asked of the compositor for one window. */
typedef struct
{
    bool geometry_set;
    TraceRect geometry; /* xdg_surface.set_window_geometry */

    bool input_set;
    bool input_infinite; /* wl_surface.set_input_region(nil) */
    int input_rects;     /* wl_region.add calls on the region set */
    bool input_subtracted;
    TraceRect input; /* the first rectangle added */

    bool min_set, max_set;
    int min_w, min_h, max_w, max_h; /* xdg_toplevel.set_{min,max}_size */

    bool configured;
    int configure_w, configure_h; /* the last xdg_toplevel.configure event */

    bool anchor_set, offset_set;
    TraceRect anchor; /* xdg_positioner.set_anchor_rect */
    int offset_x, offset_y; /* xdg_positioner.set_offset */
} TraceState;

/* Wayland object ids of a window's objects, 0 where it has none. */
typedef struct
{
    Uint32 surface, xdg_surface, toplevel, popup, positioner;
} TraceIds;

/* What SDL reports at a point the test marks in the trace. */
typedef struct
{
    const char *mark;
    int w, h; /* SDL_GetWindowSize: the surface, frame and insets */
    Sint64 insets[4]; /* published, -1 where a property is missing */
    SDL_WindowFlags flags;
} Checkpoint;

typedef Uint32(SDLCALL *wl_proxy_get_id_fn)(void *proxy);

static FILE *trace_file = NULL;
static int saved_stderr = -1;
static char *trace_text = NULL;
static int video_inits = 0; /* how many times video was initialized before the trace */
static SDL_SharedObject *wayland_client = NULL;
static wl_proxy_get_id_fn get_proxy_id = NULL;

/* Private helpers */

static void TraceFree(void)
{
    SDL_free(trace_text);
    trace_text = NULL;
    if (wayland_client) {
        SDL_UnloadObject(wayland_client);
        wayland_client = NULL;
        get_proxy_id = NULL;
    }
}

/* Reads the trace, reconnects SDL as it was and takes stderr back. */
static void TraceStop(void)
{
    long length;

    if (!trace_file) {
        return;
    }
    /* Disconnect while the trace still goes to the file. */
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    fflush(stderr);
    SDL_unsetenv_unsafe("WAYLAND_DEBUG");
    SDL_ResetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR);

    fseek(trace_file, 0, SEEK_END);
    length = ftell(trace_file);
    fseek(trace_file, 0, SEEK_SET);
    trace_text = (char *)SDL_malloc((size_t)(length > 0 ? length : 0) + 1);
    if (trace_text) {
        const size_t got = length > 0 ? fread(trace_text, 1, (size_t)length, trace_file) : 0;
        trace_text[got] = '\0';
    }
    fseek(trace_file, 0, SEEK_END);

    /* Video as it was, for whatever runs after this suite, still into the file:
     * libwayland keeps tracing a client once it has, whatever the environment
     * says, and the reconnection's requests would flood the log. */
    while (video_inits-- > 0) {
        SDL_InitSubSystem(SDL_INIT_VIDEO);
    }
    video_inits = 0;

    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    saved_stderr = -1;
    fclose(trace_file);
    trace_file = NULL;
}

/**
 * Reconnects SDL's video to the compositor with libwayland's request trace
 * on, written to a file in place of stderr, and libdecor off so windows are
 * plain xdg-shell toplevels.
 *
 * Returns TEST_COMPLETED to go on, TEST_SKIPPED off Wayland, or -1 after a
 * failed check.
 */
static int TraceStart(void)
{
    const char *driver = SDL_GetCurrentVideoDriver();
    bool ok;

    if (!driver || SDL_strcmp(driver, "wayland") != 0) {
        SDLTest_Log("Video driver is '%s', not 'wayland', skipping test", driver ? driver : "(none)");
        return TEST_SKIPPED;
    }

    wayland_client = SDL_LoadObject("libwayland-client.so.0");
    get_proxy_id = wayland_client ? (wl_proxy_get_id_fn)SDL_LoadFunction(wayland_client, "wl_proxy_get_id") : NULL;
    if (!SDLTest_AssertCheck(get_proxy_id != NULL, "Validate that wl_proxy_get_id was found in libwayland-client.so.0, got error: %s", get_proxy_id ? "none" : SDL_GetError())) {
        TraceFree();
        return -1;
    }

    trace_file = tmpfile();
    if (!SDLTest_AssertCheck(trace_file != NULL, "Validate that a file for the trace was made")) {
        TraceFree();
        return -1;
    }

    /* Video is reference counted: shut it down as many times as it was
     * initialized (the harness and SDLTest_CommonInit both do), and bring it
     * back as many times after. */
    video_inits = 0;
    while (SDL_WasInit(SDL_INIT_VIDEO) && video_inits < 16) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        ++video_inits;
    }
    if (!SDLTest_AssertCheck(!SDL_WasInit(SDL_INIT_VIDEO), "Validate that video was shut down to reconnect with the trace on")) {
        fclose(trace_file);
        trace_file = NULL;
        while (video_inits-- > 0) {
            SDL_InitSubSystem(SDL_INIT_VIDEO);
        }
        video_inits = 0;
        TraceFree();
        return -1;
    }

    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    dup2(fileno(trace_file), STDERR_FILENO);
    SDL_setenv_unsafe("WAYLAND_DEBUG", "client", 1);
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0");

    ok = SDL_InitSubSystem(SDL_INIT_VIDEO);
    driver = ok ? SDL_GetCurrentVideoDriver() : NULL;
    ok = ok && driver && SDL_strcmp(driver, "wayland") == 0;
    if (!ok) {
        char error[256];
        SDL_strlcpy(error, SDL_GetError(), sizeof(error));
        TraceStop();
        SDLTest_AssertCheck(false, "Validate that video reconnected to Wayland with the trace on, got error: %s", error);
        TraceFree();
        return -1;
    }
    return TEST_COMPLETED;
}

/* A line in the trace, which a test's checkpoint names. */
static void TraceMark(const char *mark)
{
    fprintf(stderr, "\nfizzy-mark: %s\n", mark);
    fflush(stderr);
}

static const char *TraceFindMark(const char *mark)
{
    char line[128];
    const char *found;

    if (!trace_text) {
        return NULL;
    }
    SDL_snprintf(line, sizeof(line), "fizzy-mark: %s\n", mark);
    found = SDL_strstr(trace_text, line);
    return found;
}

static Uint32 ProxyId(SDL_Window *window, const char *property)
{
    void *proxy = SDL_GetPointerProperty(SDL_GetWindowProperties(window), property, NULL);
    return proxy && get_proxy_id ? get_proxy_id(proxy) : 0;
}

static void GetIds(SDL_Window *window, TraceIds *ids)
{
    ids->surface = ProxyId(window, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER);
    ids->xdg_surface = ProxyId(window, SDL_PROP_WINDOW_WAYLAND_XDG_SURFACE_POINTER);
    ids->toplevel = ProxyId(window, SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_POINTER);
    ids->popup = ProxyId(window, SDL_PROP_WINDOW_WAYLAND_XDG_POPUP_POINTER);
    ids->positioner = ProxyId(window, SDL_PROP_WINDOW_WAYLAND_XDG_POSITIONER_POINTER);
}

/* Copies a trace line without terminal colour escapes. */
static void CopyLine(const char *begin, const char *end, char *out, size_t size)
{
    size_t n = 0;

    while (begin < end && n + 1 < size) {
        if (*begin == '\033') {
            /* ESC [ ... letter */
            ++begin;
            if (begin < end && *begin == '[') {
                ++begin;
                while (begin < end && !SDL_isalpha((unsigned char)*begin)) {
                    ++begin;
                }
            }
            if (begin < end) {
                ++begin;
            }
            continue;
        }
        out[n++] = *begin++;
    }
    out[n] = '\0';
}

/* Parses up to `count` integers from a message's arguments. */
static int ParseInts(const char *args, int *values, int count)
{
    int n = 0;
    const char *p = args;

    while (n < count && *p && *p != ')') {
        char *end;
        long v;

        while (*p == ' ' || *p == ',') {
            ++p;
        }
        v = SDL_strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        values[n++] = (int)v;
        p = end;
    }
    return n;
}

/* The id of an object argument ("wl_region#12", "wl_region@12", "new id
 * wl_region#12"), or 0 for "nil". */
static Uint32 ParseObjectArg(const char *args)
{
    const char *p = args;

    while (*p && *p != ')' && *p != '#' && *p != '@') {
        ++p;
    }
    if (*p == '#' || *p == '@') {
        return (Uint32)SDL_strtoul(p + 1, NULL, 10);
    }
    return 0;
}

#define MAX_REGIONS 64

typedef struct
{
    Uint32 id;
    int rects;
    bool subtracted;
    TraceRect first;
} TraceRegion;

static TraceRegion *FindRegion(TraceRegion *regions, int *count, Uint32 id, bool reset)
{
    int i;

    for (i = 0; i < *count; ++i) {
        if (regions[i].id == id) {
            if (reset) {
                SDL_zero(regions[i]);
                regions[i].id = id;
            }
            return &regions[i];
        }
    }
    if (*count == MAX_REGIONS) {
        /* Reuse the oldest; the window's region is made just before it is set */
        SDL_memmove(regions, regions + 1, sizeof(*regions) * (MAX_REGIONS - 1));
        --*count;
    }
    SDL_zero(regions[*count]);
    regions[*count].id = id;
    return &regions[(*count)++];
}

/**
 * Reads the requests sent (and the toplevel configure events received) on a
 * window's objects from the start of the trace up to `mark`, keeping the last
 * of each.
 */
static bool ParseTrace(const char *mark, const TraceIds *ids, TraceState *state)
{
    TraceRegion regions[MAX_REGIONS];
    int region_count = 0;
    const char *end = TraceFindMark(mark);
    const char *p = trace_text;

    SDL_zerop(state);
    if (!end) {
        return false;
    }

    while (p < end) {
        const char *eol = SDL_strchr(p, '\n');
        char line[1024];
        const char *msg;
        char iface[64];
        Uint32 id;
        char name[64];
        const char *args;
        bool sent;
        size_t n;
        char *after;

        if (!eol || eol > end) {
            eol = end;
        }
        CopyLine(p, eol, line, sizeof(line));
        p = eol + 1;

        /* "[ 1234.567] {Default Queue}  -> wl_surface#3.attach(...)": requests
         * have the arrow, events don't. The queue name is libwayland 1.23+. */
        msg = SDL_strstr(line, "-> ");
        sent = msg != NULL;
        if (sent) {
            msg += 3;
        } else {
            msg = SDL_strchr(line, ']');
            if (!msg) {
                continue;
            }
            ++msg;
            if (SDL_strchr(msg, '}')) {
                msg = SDL_strchr(msg, '}') + 1;
            }
        }
        while (*msg == ' ') {
            ++msg;
        }

        n = 0;
        while (msg[n] && msg[n] != '#' && msg[n] != '@' && msg[n] != ' ' && n + 1 < sizeof(iface)) {
            iface[n] = msg[n];
            ++n;
        }
        iface[n] = '\0';
        if (msg[n] != '#' && msg[n] != '@') {
            continue;
        }
        id = (Uint32)SDL_strtoul(msg + n + 1, &after, 10);
        if (*after != '.') {
            continue;
        }
        ++after;
        n = 0;
        while (after[n] && after[n] != '(' && n + 1 < sizeof(name)) {
            name[n] = after[n];
            ++n;
        }
        name[n] = '\0';
        if (after[n] != '(') {
            continue;
        }
        args = after + n + 1;

        if (!sent) {
            if (ids->toplevel && id == ids->toplevel && SDL_strcmp(iface, "xdg_toplevel") == 0 && SDL_strcmp(name, "configure") == 0) {
                int v[2];
                if (ParseInts(args, v, 2) == 2) {
                    state->configured = true;
                    state->configure_w = v[0];
                    state->configure_h = v[1];
                }
            }
            continue;
        }

        if (SDL_strcmp(iface, "wl_compositor") == 0 && SDL_strcmp(name, "create_region") == 0) {
            FindRegion(regions, &region_count, ParseObjectArg(args), true);
        } else if (SDL_strcmp(iface, "wl_region") == 0 && SDL_strcmp(name, "add") == 0) {
            TraceRegion *region = FindRegion(regions, &region_count, id, false);
            int v[4];
            if (ParseInts(args, v, 4) == 4) {
                if (region->rects == 0) {
                    region->first.x = v[0];
                    region->first.y = v[1];
                    region->first.w = v[2];
                    region->first.h = v[3];
                }
                ++region->rects;
            }
        } else if (SDL_strcmp(iface, "wl_region") == 0 && SDL_strcmp(name, "subtract") == 0) {
            FindRegion(regions, &region_count, id, false)->subtracted = true;
        } else if (id == ids->surface && SDL_strcmp(iface, "wl_surface") == 0 && SDL_strcmp(name, "set_input_region") == 0) {
            const Uint32 region_id = ParseObjectArg(args);
            state->input_set = true;
            state->input_infinite = region_id == 0;
            state->input_rects = 0;
            state->input_subtracted = false;
            if (region_id) {
                const TraceRegion *region = FindRegion(regions, &region_count, region_id, false);
                state->input_rects = region->rects;
                state->input_subtracted = region->subtracted;
                state->input = region->first;
            }
        } else if (id == ids->xdg_surface && SDL_strcmp(iface, "xdg_surface") == 0 && SDL_strcmp(name, "set_window_geometry") == 0) {
            int v[4];
            if (ParseInts(args, v, 4) == 4) {
                state->geometry_set = true;
                state->geometry.x = v[0];
                state->geometry.y = v[1];
                state->geometry.w = v[2];
                state->geometry.h = v[3];
            }
        } else if (id == ids->toplevel && SDL_strcmp(iface, "xdg_toplevel") == 0 && SDL_strcmp(name, "set_min_size") == 0) {
            int v[2];
            if (ParseInts(args, v, 2) == 2) {
                state->min_set = true;
                state->min_w = v[0];
                state->min_h = v[1];
            }
        } else if (id == ids->toplevel && SDL_strcmp(iface, "xdg_toplevel") == 0 && SDL_strcmp(name, "set_max_size") == 0) {
            int v[2];
            if (ParseInts(args, v, 2) == 2) {
                state->max_set = true;
                state->max_w = v[0];
                state->max_h = v[1];
            }
        } else if (id == ids->positioner && SDL_strcmp(iface, "xdg_positioner") == 0 && SDL_strcmp(name, "set_anchor_rect") == 0) {
            int v[4];
            if (ParseInts(args, v, 4) == 4) {
                state->anchor_set = true;
                state->anchor.x = v[0];
                state->anchor.y = v[1];
                state->anchor.w = v[2];
                state->anchor.h = v[3];
            }
        } else if (id == ids->positioner && SDL_strcmp(iface, "xdg_positioner") == 0 && SDL_strcmp(name, "set_offset") == 0) {
            int v[2];
            if (ParseInts(args, v, 2) == 2) {
                state->offset_set = true;
                state->offset_x = v[0];
                state->offset_y = v[1];
            }
        }
    }
    return true;
}

/* Logs the trace's lines about a window's shell objects and input region, and
 * the marks, so a failure in CI can be read without rerunning it. */
static void LogTrace(const TraceIds *ids)
{
    const char *p = trace_text;
    char objects[5][48];
    int count = 0;

    if (!p) {
        return;
    }
    if (ids->xdg_surface) {
        SDL_snprintf(objects[count++], sizeof(objects[0]), "xdg_surface#%" SDL_PRIu32 ".", ids->xdg_surface);
    }
    if (ids->toplevel) {
        SDL_snprintf(objects[count++], sizeof(objects[0]), "xdg_toplevel#%" SDL_PRIu32 ".", ids->toplevel);
    }
    if (ids->popup) {
        SDL_snprintf(objects[count++], sizeof(objects[0]), "xdg_popup#%" SDL_PRIu32 ".", ids->popup);
    }
    if (ids->positioner) {
        SDL_snprintf(objects[count++], sizeof(objects[0]), "xdg_positioner#%" SDL_PRIu32 ".", ids->positioner);
    }
    SDL_snprintf(objects[count++], sizeof(objects[0]), "wl_surface#%" SDL_PRIu32 ".set_input_region", ids->surface);

    while (*p) {
        const char *eol = SDL_strchr(p, '\n');
        char line[1024];
        bool show;
        int i;

        if (!eol) {
            eol = p + SDL_strlen(p);
        }
        CopyLine(p, eol, line, sizeof(line));
        p = *eol ? eol + 1 : eol;

        /* Older libwayland writes "@" for "#" */
        for (i = 0; line[i]; ++i) {
            if (line[i] == '@') {
                line[i] = '#';
            }
        }
        show = SDL_strstr(line, "fizzy-mark: ") != NULL || SDL_strstr(line, "wl_region#") != NULL;
        for (i = 0; !show && i < count; ++i) {
            show = SDL_strstr(line, objects[i]) != NULL;
        }
        if (show) {
            SDLTest_Log("trace: %s", line);
        }
    }
}

static void TakeCheckpoint(SDL_Window *window, const char *mark, Checkpoint *checkpoint)
{
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);

    checkpoint->mark = mark;
    SDL_GetWindowSize(window, &checkpoint->w, &checkpoint->h);
    checkpoint->insets[0] = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_LEFT_NUMBER, -1);
    checkpoint->insets[1] = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_TOP_NUMBER, -1);
    checkpoint->insets[2] = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_RIGHT_NUMBER, -1);
    checkpoint->insets[3] = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_WAYLAND_FRAME_INSET_BOTTOM_NUMBER, -1);
    checkpoint->flags = SDL_GetWindowFlags(window);
    TraceMark(mark);
}

/* Clears and presents one frame, so the window maps and keeps up. */
static void Present(SDL_GPUDevice *device, SDL_Window *window)
{
    SDL_GPUCommandBuffer *cmdbuf = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUTexture *swapchain = NULL;

    if (!cmdbuf) {
        return;
    }
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmdbuf, window, &swapchain, NULL, NULL) && swapchain) {
        SDL_GPUColorTargetInfo target;
        SDL_GPURenderPass *pass;

        SDL_zero(target);
        target.texture = swapchain;
        target.clear_color.r = 0.2f;
        target.clear_color.g = 0.3f;
        target.clear_color.b = 0.4f;
        target.clear_color.a = 1.0f;
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        pass = SDL_BeginGPURenderPass(cmdbuf, &target, 1, NULL);
        if (pass) {
            SDL_EndGPURenderPass(pass);
        }
    }
    SDL_SubmitGPUCommandBuffer(cmdbuf);
}

/* Lets the compositor and SDL settle: sync, then pump events and present. */
static void Settle(SDL_GPUDevice *device, SDL_Window *window)
{
    int i;

    SDL_SyncWindow(window);
    for (i = 0; i < 10; ++i) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
        }
        Present(device, window);
        SDL_Delay(5);
    }
    SDL_SyncWindow(window);
}

/* A test's window and the GPU device presenting it, made with the trace on. */
typedef struct
{
    SDL_GPUDevice *device;
    SDL_Window *window;
    bool claimed;
} TestWindow;

/* Makes a resizable, borderless window with the insets, as fizzy's Linux
 * backend does, shows it and presents into it. Nothing is asserted here, as
 * stderr is the trace: the caller checks after TraceStop. */
static void CreateTestWindow(TestWindow *tw)
{
    SDL_PropertiesID props = SDL_CreateProperties();

    SDL_zerop(tw);
    tw->device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_DXIL, false, NULL);

    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "testautomation_fizzy_wayland");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, SURFACE_W);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, SURFACE_H);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_LEFT_NUMBER, INSET_LEFT);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_TOP_NUMBER, INSET_TOP);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_RIGHT_NUMBER, INSET_RIGHT);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WAYLAND_FRAME_INSET_BOTTOM_NUMBER, INSET_BOTTOM);
    tw->window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);

    if (tw->device && tw->window) {
        tw->claimed = SDL_ClaimWindowForGPUDevice(tw->device, tw->window);
    }
    if (tw->claimed) {
        Settle(tw->device, tw->window);
    }
}

static void DestroyTestWindow(TestWindow *tw)
{
    if (tw->claimed) {
        SDL_WaitForGPUIdle(tw->device);
        SDL_ReleaseWindowFromGPUDevice(tw->device, tw->window);
    }
    if (tw->window) {
        SDL_DestroyWindow(tw->window);
    }
    if (tw->device) {
        SDL_DestroyGPUDevice(tw->device);
    }
    SDL_zerop(tw);
}

/* Checks the window was made and mapped; after TraceStop. */
static bool CheckTestWindow(bool made, bool device, bool claimed, const TraceIds *ids)
{
    SDLTest_AssertCheck(device, "Validate that a GPU device was made to present with");
    SDLTest_AssertCheck(made, "Validate that the window was made");
    SDLTest_AssertCheck(claimed, "Validate that the window was claimed for the GPU device");
    SDLTest_AssertCheck(ids->surface != 0 && ids->xdg_surface != 0 && ids->toplevel != 0,
                        "Validate that the window is an xdg-shell toplevel, got wl_surface %" SDL_PRIu32 ", xdg_surface %" SDL_PRIu32 ", xdg_toplevel %" SDL_PRIu32,
                        ids->surface, ids->xdg_surface, ids->toplevel);
    return made && device && claimed && ids->xdg_surface && ids->toplevel;
}

static void CheckPublishedInsets(const Checkpoint *checkpoint, const int expected[4])
{
    int i;

    for (i = 0; i < 4; ++i) {
        SDLTest_AssertCheck(checkpoint->insets[i] == expected[i],
                            "%s: validate the published %s frame inset, expected: %d, got: %" SDL_PRIs64 "%s",
                            checkpoint->mark, inset_names[i], expected[i], checkpoint->insets[i],
                            checkpoint->insets[i] < 0 ? " (not published)" : "");
    }
}

/* While floating: the window geometry is the frame, inside the insets, and the
 * input region the frame and a band of at most 8 units round it. */
static void CheckFloatingFrame(const Checkpoint *checkpoint, const TraceState *state)
{
    const int frame_w = checkpoint->w - INSET_LEFT - INSET_RIGHT;
    const int frame_h = checkpoint->h - INSET_TOP - INSET_BOTTOM;
    const int band_l = SDL_min(INSET_LEFT, INPUT_BAND);
    const int band_t = SDL_min(INSET_TOP, INPUT_BAND);
    const int band_r = SDL_min(INSET_RIGHT, INPUT_BAND);
    const int band_b = SDL_min(INSET_BOTTOM, INPUT_BAND);

    CheckPublishedInsets(checkpoint, insets_asked);

    SDLTest_AssertCheck(state->geometry_set, "%s: validate that xdg_surface.set_window_geometry was sent", checkpoint->mark);
    if (state->geometry_set) {
        SDLTest_AssertCheck(state->geometry.x == INSET_LEFT && state->geometry.y == INSET_TOP && state->geometry.w == frame_w && state->geometry.h == frame_h,
                            "%s: validate the window geometry is the frame, expected: (%d, %d, %d, %d), got: (%d, %d, %d, %d)",
                            checkpoint->mark, INSET_LEFT, INSET_TOP, frame_w, frame_h,
                            state->geometry.x, state->geometry.y, state->geometry.w, state->geometry.h);
    }

    SDLTest_AssertCheck(state->input_set && !state->input_infinite && state->input_rects == 1 && !state->input_subtracted,
                        "%s: validate that the input region set is one rectangle, got: %s, %d rectangles%s",
                        checkpoint->mark, !state->input_set ? "never set" : state->input_infinite ? "the whole surface (nil)" : "a region",
                        state->input_rects, state->input_subtracted ? ", with subtractions" : "");
    if (state->input_set && state->input_rects == 1) {
        const int x = INSET_LEFT - band_l, y = INSET_TOP - band_t;
        const int w = frame_w + band_l + band_r, h = frame_h + band_t + band_b;
        SDLTest_AssertCheck(state->input.x == x && state->input.y == y && state->input.w == w && state->input.h == h,
                            "%s: validate the input region is the frame and a band of at most %d, expected: (%d, %d, %d, %d), got: (%d, %d, %d, %d)",
                            checkpoint->mark, INPUT_BAND, x, y, w, h, state->input.x, state->input.y, state->input.w, state->input.h);
    }
}

/* Maximized or fullscreen: no insets, the window geometry the whole surface
 * (the size the compositor configured), input everywhere. */
static void CheckFilledFrame(const Checkpoint *checkpoint, const TraceState *state)
{
    CheckPublishedInsets(checkpoint, insets_zero);

    SDLTest_AssertCheck(state->configured, "%s: validate that the toplevel was configured", checkpoint->mark);
    if (state->configured && state->configure_w && state->configure_h) {
        SDLTest_AssertCheck(checkpoint->w == state->configure_w && checkpoint->h == state->configure_h,
                            "%s: validate the surface is the configured size, expected: %dx%d, got: %dx%d",
                            checkpoint->mark, state->configure_w, state->configure_h, checkpoint->w, checkpoint->h);
    }
    SDLTest_AssertCheck(state->geometry_set, "%s: validate that xdg_surface.set_window_geometry was sent", checkpoint->mark);
    if (state->geometry_set) {
        SDLTest_AssertCheck(state->geometry.x == 0 && state->geometry.y == 0 && state->geometry.w == checkpoint->w && state->geometry.h == checkpoint->h,
                            "%s: validate the window geometry is the surface, expected: (0, 0, %d, %d), got: (%d, %d, %d, %d)",
                            checkpoint->mark, checkpoint->w, checkpoint->h,
                            state->geometry.x, state->geometry.y, state->geometry.w, state->geometry.h);
    }
    SDLTest_AssertCheck(state->input_set && state->input_infinite,
                        "%s: validate that the input region is the whole surface (nil), got: %s",
                        checkpoint->mark, !state->input_set ? "never set" : state->input_infinite ? "nil" : "a region");
}

static bool ParseAt(const Checkpoint *checkpoint, const TraceIds *ids, TraceState *state)
{
    return SDLTest_AssertCheck(ParseTrace(checkpoint->mark, ids, state), "Validate that the trace holds the mark '%s'", checkpoint->mark);
}

/* Maximizes (or fullscreens) a floating window and restores it. */
static int frameInsetsFillAndRestore(bool fullscreen)
{
    TestWindow tw;
    TraceIds ids;
    Checkpoint floating = { 0 }, filled = { 0 }, restored = { 0 };
    TraceState state;
    bool made, device, claimed, ok;
    int result = TraceStart();

    if (result != TEST_COMPLETED) {
        return result < 0 ? TEST_COMPLETED : result;
    }
    SDL_zero(state);

    CreateTestWindow(&tw);
    made = tw.window != NULL;
    device = tw.device != NULL;
    claimed = tw.claimed;
    SDL_zero(ids);
    if (claimed) {
        GetIds(tw.window, &ids);
        TakeCheckpoint(tw.window, "floating", &floating);

        if (fullscreen) {
            SDL_SetWindowFullscreen(tw.window, true);
        } else {
            SDL_MaximizeWindow(tw.window);
        }
        Settle(tw.device, tw.window);
        TakeCheckpoint(tw.window, fullscreen ? "fullscreen" : "maximized", &filled);

        if (fullscreen) {
            SDL_SetWindowFullscreen(tw.window, false);
        } else {
            SDL_RestoreWindow(tw.window);
        }
        Settle(tw.device, tw.window);
        TakeCheckpoint(tw.window, "restored", &restored);
    }
    DestroyTestWindow(&tw);
    TraceStop();

    ok = CheckTestWindow(made, device, claimed, &ids);
    if (ok) {
        const SDL_WindowFlags flag = fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_MAXIMIZED;

        if (ParseAt(&floating, &ids, &state)) {
            CheckFloatingFrame(&floating, &state);
        }

        SDLTest_AssertCheck((filled.flags & flag) != 0, "%s: validate that the compositor %s the window", filled.mark, fullscreen ? "made fullscreen" : "maximized");
        if (ParseAt(&filled, &ids, &state)) {
            CheckFilledFrame(&filled, &state);
        }

        SDLTest_AssertCheck((restored.flags & flag) == 0, "%s: validate that the window floats again", restored.mark);
        SDLTest_AssertCheck(restored.w == SURFACE_W && restored.h == SURFACE_H,
                            "%s: validate the surface is back to its floating size, expected: %dx%d, got: %dx%d",
                            restored.mark, SURFACE_W, SURFACE_H, restored.w, restored.h);
        if (ParseAt(&restored, &ids, &state)) {
            CheckFloatingFrame(&restored, &state);
        }
    }
    LogTrace(&ids);
    TraceFree();
    return TEST_COMPLETED;
}

#endif /* FIZZY_WAYLAND_TRACE */

/* Test case functions */

/**
 * A floating window: the window geometry is the frame, the input region the
 * frame and a band, the published insets those asked for, the min/max sizes
 * the frame's, and all of it follows a resize.
 */
static int SDLCALL wayland_frameInsetsFloating(void *arg)
{
#ifdef FIZZY_WAYLAND_TRACE
    TestWindow tw;
    TraceIds ids;
    Checkpoint shown = { 0 }, limited = { 0 }, resized = { 0 };
    TraceState state;
    bool made, device, claimed;
    int result = TraceStart();

    if (result != TEST_COMPLETED) {
        return result < 0 ? TEST_COMPLETED : result;
    }
    SDL_zero(state);

    CreateTestWindow(&tw);
    made = tw.window != NULL;
    device = tw.device != NULL;
    claimed = tw.claimed;
    SDL_zero(ids);
    if (claimed) {
        GetIds(tw.window, &ids);
        TakeCheckpoint(tw.window, "shown", &shown);

        SDL_SetWindowMinimumSize(tw.window, 200, 150);
        SDL_SetWindowMaximumSize(tw.window, 800, 600);
        Settle(tw.device, tw.window);
        TakeCheckpoint(tw.window, "limits", &limited);

        SDL_SetWindowSize(tw.window, 500, 380);
        Settle(tw.device, tw.window);
        TakeCheckpoint(tw.window, "resized", &resized);
    }
    DestroyTestWindow(&tw);
    TraceStop();

    if (CheckTestWindow(made, device, claimed, &ids)) {
        SDLTest_AssertCheck(shown.w == SURFACE_W && shown.h == SURFACE_H,
                            "%s: validate the surface is the size made, expected: %dx%d, got: %dx%d",
                            shown.mark, SURFACE_W, SURFACE_H, shown.w, shown.h);
        if (ParseAt(&shown, &ids, &state)) {
            CheckFloatingFrame(&shown, &state);
        }

        if (ParseAt(&limited, &ids, &state)) {
            const int min_w = 200 - INSET_LEFT - INSET_RIGHT, min_h = 150 - INSET_TOP - INSET_BOTTOM;
            const int max_w = 800 - INSET_LEFT - INSET_RIGHT, max_h = 600 - INSET_TOP - INSET_BOTTOM;
            SDLTest_AssertCheck(state.min_set && state.min_w == min_w && state.min_h == min_h,
                                "%s: validate xdg_toplevel.set_min_size is the frame's, expected: %dx%d, got: %s%dx%d",
                                limited.mark, min_w, min_h, state.min_set ? "" : "(never sent) ", state.min_w, state.min_h);
            SDLTest_AssertCheck(state.max_set && state.max_w == max_w && state.max_h == max_h,
                                "%s: validate xdg_toplevel.set_max_size is the frame's, expected: %dx%d, got: %s%dx%d",
                                limited.mark, max_w, max_h, state.max_set ? "" : "(never sent) ", state.max_w, state.max_h);
        }

        SDLTest_AssertCheck(resized.w == 500 && resized.h == 380,
                            "%s: validate the surface took the new size, expected: 500x380, got: %dx%d",
                            resized.mark, resized.w, resized.h);
        if (ParseAt(&resized, &ids, &state)) {
            CheckFloatingFrame(&resized, &state);
        }
    }
    LogTrace(&ids);
    TraceFree();
    return TEST_COMPLETED;
#else
    SDLTest_Log("Wayland only, skipping test");
    return TEST_SKIPPED;
#endif
}

/**
 * Maximized: no insets, the window geometry and the input region the whole
 * surface; restored: the insets come back.
 */
static int SDLCALL wayland_frameInsetsMaximized(void *arg)
{
#ifdef FIZZY_WAYLAND_TRACE
    return frameInsetsFillAndRestore(false);
#else
    SDLTest_Log("Wayland only, skipping test");
    return TEST_SKIPPED;
#endif
}

/**
 * Fullscreen: no insets, the window geometry and the input region the whole
 * surface; back to windowed: the insets come back.
 */
static int SDLCALL wayland_frameInsetsFullscreen(void *arg)
{
#ifdef FIZZY_WAYLAND_TRACE
    return frameInsetsFillAndRestore(true);
#else
    SDLTest_Log("Wayland only, skipping test");
    return TEST_SKIPPED;
#endif
}

/**
 * A popup of a window with insets is placed relative to the parent's frame:
 * the positioner's anchor rectangle is the frame and its offset is the popup's
 * position less the parent's left and top insets.
 */
static int SDLCALL wayland_frameInsetsPopup(void *arg)
{
#ifdef FIZZY_WAYLAND_TRACE
    TestWindow tw;
    SDL_Window *popup = NULL;
    TraceIds parent_ids, popup_ids;
    Checkpoint parent = { 0 }, shown = { 0 };
    TraceState state;
    bool made, device, claimed, popup_made = false;
    int popup_x = 0, popup_y = 0;
    int result = TraceStart();

    if (result != TEST_COMPLETED) {
        return result < 0 ? TEST_COMPLETED : result;
    }
    SDL_zero(state);

    CreateTestWindow(&tw);
    made = tw.window != NULL;
    device = tw.device != NULL;
    claimed = tw.claimed;
    SDL_zero(parent_ids);
    SDL_zero(popup_ids);
    if (claimed) {
        GetIds(tw.window, &parent_ids);
        TakeCheckpoint(tw.window, "parent", &parent);

        /* A tooltip: an xdg_popup without a grab, which needs no seat. */
        popup = SDL_CreatePopupWindow(tw.window, 50, 60, 100, 40, SDL_WINDOW_TOOLTIP);
        popup_made = popup != NULL;
        if (popup) {
            Settle(tw.device, tw.window);
            GetIds(popup, &popup_ids);
            SDL_GetWindowPosition(popup, &popup_x, &popup_y);
            TakeCheckpoint(popup, "popup", &shown);
            SDL_DestroyWindow(popup);
        }
    }
    DestroyTestWindow(&tw);
    TraceStop();

    if (CheckTestWindow(made, device, claimed, &parent_ids)) {
        const int frame_w = parent.w - INSET_LEFT - INSET_RIGHT;
        const int frame_h = parent.h - INSET_TOP - INSET_BOTTOM;

        CheckPublishedInsets(&parent, insets_asked);
        SDLTest_AssertCheck(popup_made, "Validate that the popup was made, got error: %s", popup_made ? "none" : SDL_GetError());
        SDLTest_AssertCheck(!popup_made || (popup_ids.popup != 0 && popup_ids.positioner != 0),
                            "Validate that the popup is an xdg_popup with a positioner, got xdg_popup %" SDL_PRIu32 ", xdg_positioner %" SDL_PRIu32,
                            popup_ids.popup, popup_ids.positioner);
        if (popup_made && popup_ids.positioner && ParseAt(&shown, &popup_ids, &state)) {
            SDLTest_AssertCheck(state.anchor_set && state.anchor.x == 0 && state.anchor.y == 0 && state.anchor.w == frame_w && state.anchor.h == frame_h,
                                "%s: validate the anchor rectangle is the parent's frame, expected: (0, 0, %d, %d), got: %s(%d, %d, %d, %d)",
                                shown.mark, frame_w, frame_h, state.anchor_set ? "" : "(never sent) ",
                                state.anchor.x, state.anchor.y, state.anchor.w, state.anchor.h);
            SDLTest_AssertCheck(state.offset_set && state.offset_x == 50 - INSET_LEFT && state.offset_y == 60 - INSET_TOP,
                                "%s: validate the offset is from the parent's frame, expected: (%d, %d), got: %s(%d, %d)",
                                shown.mark, 50 - INSET_LEFT, 60 - INSET_TOP, state.offset_set ? "" : "(never sent) ",
                                state.offset_x, state.offset_y);
            /* Back from the compositor, in the parent's coordinates again */
            SDLTest_AssertCheck(popup_x == 50 && popup_y == 60,
                                "%s: validate the popup's position, expected: (50, 60), got: (%d, %d)",
                                shown.mark, popup_x, popup_y);
        }
    }
    LogTrace(&popup_ids);
    TraceFree();
    return TEST_COMPLETED;
#else
    SDLTest_Log("Wayland only, skipping test");
    return TEST_SKIPPED;
#endif
}

/* ================= Test References ================== */

static const SDLTest_TestCaseReference waylandTestFrameInsetsFloating = {
    wayland_frameInsetsFloating, "wayland_frameInsetsFloating", "A floating window's geometry, input region, published insets and min/max sizes are its frame's", TEST_ENABLED
};

static const SDLTest_TestCaseReference waylandTestFrameInsetsMaximized = {
    wayland_frameInsetsMaximized, "wayland_frameInsetsMaximized", "A maximized window has no insets, and gets them back when restored", TEST_ENABLED
};

static const SDLTest_TestCaseReference waylandTestFrameInsetsFullscreen = {
    wayland_frameInsetsFullscreen, "wayland_frameInsetsFullscreen", "A fullscreen window has no insets, and gets them back when windowed", TEST_ENABLED
};

static const SDLTest_TestCaseReference waylandTestFrameInsetsPopup = {
    wayland_frameInsetsPopup, "wayland_frameInsetsPopup", "A popup is anchored to its parent's frame", TEST_ENABLED
};

static const SDLTest_TestCaseReference *waylandTests[] = {
    &waylandTestFrameInsetsFloating,
    &waylandTestFrameInsetsMaximized,
    &waylandTestFrameInsetsFullscreen,
    &waylandTestFrameInsetsPopup,
    NULL
};

/* Wayland test suite (global) */
SDLTest_TestSuiteReference fizzyWaylandTestSuite = {
    "FizzyWayland",
    NULL,
    waylandTests,
    NULL
};
