/* Game controllers through SDL (host/hos_sdl.c: SDL3, else SDL2, third_party/sdl2/SDL2.dll; zlib license, loaded at
   run time): DualSense, DualShock 4, Switch Pro, Xbox and most other pads, over USB or Bluetooth. XInput alone sees
   only Xbox-style pads.

   A thread of its own (on Linux the window's, which runs SDL there too) opens every controller SDL knows (and ones
   plugged in later), polls them every 4 ms and keeps
   the merged state; nn_hid.c ORs it into the input with XInput, keyboard and mouse. Without SDL nothing
   changes. HOS_NO_SDL=1 leaves SDL out. The inputs go by position (HOS_PAD_*, SDL's own order: the bottom face button
   first, cross on a DualSense, A on an Xbox pad); nn_hid.c's controls make them the Switch's buttons. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "hos_sdl.h"
#include "../gpu/vk/vk_api.h"   /* (HOS_PAD_*) */

void hos_host_thread_register_self(const char *name);   /* hos_prof.c */

/* (BTN_A ... BTN_RIGHT are HOS_PAD_SOUTH ... HOS_PAD_RIGHT) */
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_BACK, BTN_GUIDE, BTN_START, BTN_LSTICK, BTN_RSTICK, BTN_LSHOULDER, BTN_RSHOULDER,
       BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_MISC1, BTN_N = 21 };
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT };

#define MAX_PADS 8
static struct { void *gc; int32_t id; uint16_t rumble_lo, rumble_hi; int64_t rumble_ns; int sensors; } pads[MAX_PADS];
static SRWLOCK state_lock = SRWLOCK_INIT;
static uint64_t state_buttons;
static int32_t state_stick[4];
static int state_count;
static float state_accel[3], state_gyro[3];   /* (the first pad with sensors: SDL's m/s^2 and rad/s) */
static int state_motion;
static volatile LONG rumble_low, rumble_high;   /* 0..65535, set by the vibration HLE */

static int32_t stick_value(int16_t v, int flip) {
    int32_t a = v < -32767 ? -32767 : v;
    if (flip) a = -a;
    return (a > 7849 || a < -7849) ? a : 0;   /* XInput's dead zone, as nn_hid.c uses */
}

/* SDL's controller support started; 0 when it could not be */
static int pads_start(void) {
    /* poll in the background, whatever window is in front (nn_hid.c decides when input counts) */
    hos_sdl_hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    /* (SDL2: PlayStation pads' rumble asked for; SDL3 turns it on when a rumble is first sent) */
    hos_sdl_hint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1");
    hos_sdl_hint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "1");
    /* (the map below is by position; SDL2 reports Nintendo pads by their labels unless told not to, which put a Switch Pro
       Controller's A, on the right, at the bottom button's place: A acted as B and X as Y: SkyboundTotk issue 12. SDL3's
       are by position always) */
    hos_sdl_hint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
    /* (hookshot) the Steam Controller through SDL's own HIDAPI driver, which SDL leaves off because Steam usually drives the pad:
       with it the pad's buttons, pads and gyroscope reach the game without Steam Input. HOS_SDL_STEAM_HIDAPI=0: as SDL has it */
    if (!getenv("HOS_SDL_STEAM_HIDAPI") || atoi(getenv("HOS_SDL_STEAM_HIDAPI")) != 0) hos_sdl_hint("SDL_JOYSTICK_HIDAPI_STEAM", "1");
    /* (with no window nothing reads SDL's quit event: SDL's own SIGINT/SIGTERM handlers turned a timeout's or a service
       manager's SIGTERM into that event, and the run went on until SIGKILL. Without them the signals end it as usual) */
    if (getenv("HOS_HEADLESS")) hos_sdl_hint("SDL_NO_SIGNAL_HANDLERS", "1");
    if (!hos_sdl_init(HOS_SDL_GAMEPAD)) {
        hos_log("hid: SDL could not start its controller support: %s", hos_sdl_error());
        return 0;
    }
    hos_log("hid: SDL controller support ready (DualSense, DualShock 4, Switch Pro, Xbox and others)");
    return 1;
}

/* SDL_GameControllerRumble runs only for the duration it is given, then stops, and a repeat of the same numbers was
   skipped. A rumble the game holds, and one that was already playing when this controller opened, never reached it. */
static int rumble_due(unsigned lo, unsigned hi, unsigned sent_lo, unsigned sent_hi, long long now, long long sent_ns) {
    if (lo != sent_lo || hi != sent_hi) return 1;
    if ((lo || hi) && now - sent_ns >= 500000000LL) return 1;   /* (half of the 1 s effect, so it does not lapse) */
    return 0;
}

/* one poll: controllers plugged in or gone (each second), then every open one's buttons, sticks and rumble */
static void pads_poll(void) {
    static int64_t next_scan = 0;
    hos_sdl_pads_update();
        const int64_t now = hos_now_ns();
        if (now >= next_scan) {   /* new controllers (and ones that went away) */
            next_scan = now + 1000000000LL;
            for (int i = 0; i < MAX_PADS; i++)
                if (pads[i].gc && !hos_sdl_pad_connected(pads[i].gc)) {
                    hos_log("hid: controller \"%s\" disconnected (SDL)", hos_sdl_pad_name(pads[i].gc));
                    hos_sdl_pad_close(pads[i].gc);
                    pads[i].gc = NULL;
                    pads[i].rumble_lo = pads[i].rumble_hi = 0;
                    pads[i].rumble_ns = 0;
                    pads[i].sensors = 0;
                }
            int32_t ids[32];
            int opens[32];
            const int nj = hos_sdl_pads_list(ids, opens, 32);
            for (int j = 0; j < nj; j++) {
                const int32_t id = ids[j];
                int have = 0, slot = -1;
                for (int i = 0; i < MAX_PADS; i++) {
                    if (pads[i].gc && pads[i].id == id) have = 1;
                    if (!pads[i].gc && slot < 0) slot = i;
                }
                if (have || slot < 0) continue;
                void *gc = hos_sdl_pad_open(opens[j]);
                if (!gc) continue;
                pads[slot].gc = gc;
                pads[slot].id = hos_sdl_pad_id(gc);
                pads[slot].rumble_lo = pads[slot].rumble_hi = 0;
                pads[slot].rumble_ns = 0;
                pads[slot].sensors = hos_sdl_pad_sensors_enable(gc);   /* (motion aiming, nn_hid.c: where the pad has a gyroscope) */
                hos_log("hid: controller \"%s\" connected (SDL%s)", hos_sdl_pad_name(gc), pads[slot].sensors ? ", with motion sensors" : "");
            }
        }
        uint64_t b = 0;
        int32_t s[4] = {0};
        int count = 0;
        float accel[3] = {0}, gyro[3] = {0};
        int motion = 0;
        const uint16_t lo = (uint16_t)rumble_low, hi = (uint16_t)rumble_high;
        for (int i = 0; i < MAX_PADS; i++) {
            void *gc = pads[i].gc;
            if (!gc) continue;
            count++;
            for (int k = BTN_A; k <= BTN_RIGHT; k++)   /* (SDL3's gamepad buttons are in SDL2's order) */
                if (hos_sdl_pad_button(gc, k)) b |= 1ULL << k;
            if (hos_sdl_pad_axis(gc, AX_LT) > 8000) b |= 1ULL << HOS_PAD_LTRIGGER;
            if (hos_sdl_pad_axis(gc, AX_RT) > 8000) b |= 1ULL << HOS_PAD_RTRIGGER;
            int rx_option(int id, int otherwise);
            const int inv_x = rx_option(16, 0);
            const int inv_y = rx_option(17, 0);
            const int32_t v[4] = {stick_value(hos_sdl_pad_axis(gc, AX_LX), 0), stick_value(hos_sdl_pad_axis(gc, AX_LY), 1),
                                  stick_value(hos_sdl_pad_axis(gc, AX_RX), inv_x), stick_value(hos_sdl_pad_axis(gc, AX_RY), inv_y ? 0 : 1)};
            for (int k = 0; k < 4; k++)
                if ((v[k] < 0 ? -v[k] : v[k]) > (s[k] < 0 ? -s[k] : s[k])) s[k] = v[k];
            if (!motion && pads[i].sensors && hos_sdl_pad_sensors(gc, accel, gyro)) motion = 1;
            if (rumble_due(lo, hi, pads[i].rumble_lo, pads[i].rumble_hi, now, pads[i].rumble_ns)) {
                hos_sdl_pad_rumble(gc, lo, hi, lo || hi ? 1000 : 0);
                pads[i].rumble_lo = lo;
                pads[i].rumble_hi = hi;
                pads[i].rumble_ns = now;
            }
        }
        AcquireSRWLockExclusive(&state_lock);
        state_buttons = b;
        memcpy(state_stick, s, sizeof s);
        state_count = count;
        memcpy(state_accel, accel, sizeof accel);
        memcpy(state_gyro, gyro, sizeof gyro);
        state_motion = motion;
        ReleaseSRWLockExclusive(&state_lock);
}

static DWORD WINAPI pad_main(void *arg) {
    (void)arg;
    hos_host_thread_register_self("gamepads (SDL)");
    if (!pads_start()) return 0;
    for (;;) {
        pads_poll();
        Sleep(4);
    }
}
#ifndef _WIN32
int hos_display_owns_sdl(void);   /* host/posix/display_sdl.c */
/* (linux) SDL also runs the window (host/posix/display_sdl.c), and its event queue is not for two threads. Polled
   from the window's thread, between its events. Two threads pumping it corrupted the heap once a controller was
   attached. */
static volatile LONG pads_loaded;
void hos_sdl_pads_pump(void) {
    static int state;   /* 0 not started, 1 polling, -1 not available */
    if (!state && pads_loaded) state = pads_start() ? 1 : -1;
    if (state > 0) pads_poll();
}
#endif

/* The SDL thread (or, on Linux, the window's polling), started by the first call of hos_sdl_pads or hos_sdl_sensors; 0 when there
   is no SDL */
static int ensure_started(void) {
    static volatile LONG started, absent;
    if (!started && !InterlockedExchange(&started, 1)) {
        if (getenv("HOS_NO_SDL") || !hos_sdl_load()) {
            if (!getenv("HOS_NO_SDL")) hos_log("hid: no SDL beside the exe or on the system: only XInput controllers (Xbox pads) work");
            InterlockedExchange(&absent, 1);
            return 0;
        }
#ifdef _WIN32
        CreateThread(NULL, 0, pad_main, NULL, 0, NULL);
#else
        if (hos_display_owns_sdl())
            InterlockedExchange(&pads_loaded, 1);   /* (the window's thread polls: hos_sdl_pads_pump) */
        else
            CreateThread(NULL, 0, pad_main, NULL, 0, NULL);   /* (no window: nothing else pumps SDL) */
#endif
    }
    return !absent;
}

/* The controllers' merged state (nn_hid.c): ORs their inputs in (1 << HOS_PAD_*), keeps the larger stick
   deflections; returns how many controllers SDL has open. Starts the SDL thread on the first call. */
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]) {
    if (!ensure_started()) return 0;
    AcquireSRWLockShared(&state_lock);
    const int n = state_count;
    *buttons |= state_buttons;
    for (int k = 0; k < 4; k++)
        if ((state_stick[k] < 0 ? -state_stick[k] : state_stick[k]) > (stick[k] < 0 ? -stick[k] : stick[k]))
            stick[k] = state_stick[k];
    ReleaseSRWLockShared(&state_lock);
    return n;
}

/* (hookshot) The first pad with sensors' motion (nn_hid.c's six-axis states), in SDL's units and axes: acceleration m/s^2,
   angular velocity rad/s, x right, y up, z toward the player. 0 while no such pad is open. */
int hos_sdl_sensors(float accel[3], float gyro[3]) {
    if (!ensure_started()) return 0;
    AcquireSRWLockShared(&state_lock);
    const int motion = state_motion;
    memcpy(accel, state_accel, sizeof state_accel);
    memcpy(gyro, state_gyro, sizeof state_gyro);
    ReleaseSRWLockShared(&state_lock);
    return motion;
}

/* Vibration from the game (0..1 amplitudes of the low and high bands), for the pads that rumble. */
void hos_sdl_rumble(float low, float high) {
    if (low < 0) low = 0;
    if (low > 1) low = 1;
    if (high < 0) high = 0;
    if (high > 1) high = 1;
    InterlockedExchange(&rumble_low, (LONG)(low * 65535));
    InterlockedExchange(&rumble_high, (LONG)(high * 65535));
}
