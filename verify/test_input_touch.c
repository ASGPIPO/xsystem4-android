/*
 * x86 real-path integration test for the Android touch input layer.
 *
 * Compiles the REAL src/input.c (with -DXSYS4_TOUCH_GESTURES) into this
 * translation unit and drives it through the REAL public entry point
 * handle_events(), with finger events injected through the REAL SDL
 * event queue (SDL_PushEvent -> SDL_PollEvent inside handle_events).
 * Asserts the game-visible System4 key state sequences (VK_LBUTTON
 * pulse, VK_RBUTTON in place, VK_CONTROL hold, wheel), the
 * mouse_get_pos() override, and the gesture-owner/physical-input
 * coexistence rules.
 *
 * The R1/R3 scenarios pin the tap-pulse contract on top: the press
 * frame must report the tap's game coordinates (not the
 * SDL-synthesized mouse position, which is parked at a known spot via
 * the dummy video driver), and rapid taps delivered in one event
 * batch must come out as distinct press edges, never merged.
 *
 * Time-based transitions (350 ms long-press) use the real clock; each
 * such scenario is bounded by a generous timeout and fails hard if the
 * transition does not fire.
 *
 * Link: gcc ... -DXSYS4_TOUCH_GESTURES test_input_touch.c \
 *          ../xsystem4/src/touch_gesture.c ../xsystem4/src/governor.c \
 *          $(pkg-config --cflags --libs sdl2)
 */
#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system4.h"
#include "xsystem4.h"
#include "input.h"
#include "touch_gesture.h"
#include "governor.h"

// ---- stubs for the engine symbols input.c references -----------------

struct sdl_private sdl;      // gfx state: logical size + viewport
struct config config;        // view_width/view_height read at init
bool scene_is_dirty = false; // governor activity signal

int gfx_swap_calls;
void gfx_swap(void) { gfx_swap_calls++; }
void gfx_update_screen_scale(void) {}
void gfx_flush_pending_present(void) {}
void screenshot_save(void) {}
void vm_exit(int code) { (void)code; exit(2); }
bool dbg_dap = false;
bool dbg_start_in_debugger = false;
void dbg_repl(int a, const char *b) { (void)a; (void)b; }
void dbg_dap_handle_messages(void) {}
void *_xmalloc(size_t n, const char *caller) { void *p = malloc(n); if (!p) abort(); (void)caller; return p; }
void vm_stack_trace(void) {}
void gfx_toggle_fullscreen(void) {}

// Pull in the real implementation.  Static functions become reachable
// for the focused geometry checks below.
#include "input.c"

// ---- helpers -----------------------------------------------------------

static int failures;
#define CHECK(cond, msg) do { \
	if (cond) { printf("PASS %s\n", msg); } \
	else { printf("FAIL %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

// Pump n frames, recording the peak key state seen between pumps (tap
// pulses live for exactly one pump).
static bool peak_lb, peak_rb, peak_ct;
static void reset_peaks(void) { peak_lb = peak_rb = peak_ct = false; }
static void pump(int n)
{
	for (int i = 0; i < n; i++) {
		handle_events();
		if (key_state[VK_LBUTTON])
			peak_lb = true;
		if (key_state[VK_RBUTTON])
			peak_rb = true;
		if (key_state[VK_CONTROL])
			peak_ct = true;
	}
}

static void push_finger(Uint32 type, SDL_FingerID id, float x, float y)
{
	SDL_Event e = {0};
	e.type = type;
	e.tfinger.touchId = 1;
	e.tfinger.fingerId = id;
	e.tfinger.x = x;
	e.tfinger.y = y;
	// timestamp 0 -> SDL stamps the event with the real tick count, so
	// the FSM clock and the event clock agree.
	SDL_PushEvent(&e);
	pump(6);
}

static bool wait_until(bool (*cond)(void), Uint32 timeout_ms, const char *what)
{
	Uint32 t0 = SDL_GetTicks();
	while (SDL_GetTicks() - t0 < timeout_ms) {
		handle_events();
		if (cond())
			return true;
		SDL_Delay(5);
	}
	fprintf(stderr, "timeout waiting for %s\n", what);
	return false;
}

// Pump frames with no finger down; the gesture layer must not touch
// the key state once it has gone idle.
static bool key_is(bool lb, bool rb, bool ct)
{
	return key_state[VK_LBUTTON] == lb && key_state[VK_RBUTTON] == rb
		&& key_state[VK_CONTROL] == ct;
}

static bool lb_down(void)  { return key_state[VK_LBUTTON]; }
static bool rb_down(void)  { return key_state[VK_RBUTTON]; }
static bool ct_down(void)  { return key_state[VK_CONTROL]; }

// ------------------------------------------------------------------------

static void setup_geometry(int win_w, int win_h, int vx, int vy, int vw, int vh,
                           int game_w, int game_h)
{
	// Logical/game size reported by the engine stub.
	sdl.w = game_w;
	sdl.h = game_h;
	sdl.viewport.x = vx;
	sdl.viewport.y = vy;
	sdl.viewport.w = vw;
	sdl.viewport.h = vh;
	config.view_width = game_w;
	config.view_height = game_h;

	// First finger event initializes the engine; with no real window
	// (headless) the fallback geometry is used and we then overwrite it
	// through the public geometry API, exactly like a SIZE_CHANGED
	// event would.
	push_finger(SDL_FINGERDOWN, 100, 0.5f, 0.5f);
	push_finger(SDL_FINGERUP, 100, 0.5f, 0.5f);
	pump(8); // let the tap pulse clear
	touch_gesture_set_screen_size(win_w, win_h);
	touch_gesture_set_viewport(vx, vy, vw, vh);
}

int main(void)
{
	// Real SDL mouse state (R1): the dummy video driver gives us a
	// window and SDL_WarpMouseInWindow() with no display server, so
	// the SDL-synthesized mouse position can be parked at a known
	// spot while the gesture engine publishes a tap.
	SDL_SetHint("SDL_VIDEODRIVER", "dummy");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) < 0) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	sdl.window = SDL_CreateWindow("test_input_touch", 0, 0, 1600, 1200, 0);
	if (!sdl.window) {
		fprintf(stderr, "dummy window failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_PumpEvents();

	// 1600x1200 window, 4:3 game letterboxed to 1280x960 centered
	// (viewport 160,120,1280,960), game coordinates 1024x768.
	setup_geometry(1600, 1200, 160, 120, 1280, 960, 1024, 768);

	// -- 1. tap: one-frame VK_LBUTTON pulse ----------------------------
	reset_peaks();
	push_finger(SDL_FINGERDOWN, 1, 0.5f, 0.5f);
	CHECK(touch_gesture_get_state() == TG_PENDING_TAP, "finger down -> PENDING_TAP");
	push_finger(SDL_FINGERUP, 1, 0.51f, 0.5f);
	CHECK(peak_lb, "tap -> VK_LBUTTON pulse seen");
	CHECK(key_is(false, false, false), "tap pulse released after one pump");
	CHECK(touch_gesture_get_state() == TG_IDLE, "tap ends in TG_IDLE");

	// -- 1b. tap pulse frame must carry the tap coordinates (R1) ------
	// Park the SDL-synthesized mouse far away; the game must see the
	// tap position on the very frame key_state[VK_LBUTTON] turns
	// true, or the script-side hit-test runs at the wrong spot.
	SDL_WarpMouseInWindow(sdl.window, 999, 999);
	pump(4); // drain the warp motion event
	int mx, my;
	SDL_GetMouseState(&mx, &my);
	CHECK(mx == 999 && my == 999, "SDL mouse parked at (999,999)");
	reset_peaks();
	// window (348,496) -> viewport (160,120,1280,960) -> game (150,300)
	const float tap_x = 348.0f / 1600.0f, tap_y = 496.0f / 1200.0f;
	push_finger(SDL_FINGERDOWN, 12, tap_x, tap_y);
	SDL_Event tap_up = {0};
	tap_up.type = SDL_FINGERUP;
	tap_up.tfinger.touchId = 1;
	tap_up.tfinger.fingerId = 12;
	tap_up.tfinger.x = tap_x;
	tap_up.tfinger.y = tap_y;
	SDL_PushEvent(&tap_up);
	bool saw_press = false, press_pos_ok = false;
	int press_frames = 0;
	for (int i = 0; i < 8; i++) {
		handle_events();
		if (key_state[VK_LBUTTON]) {
			press_frames++;
			saw_press = true;
			int px, py;
			mouse_get_pos(&px, &py);
			if (px == 150 && py == 300)
				press_pos_ok = true;
		}
	}
	CHECK(saw_press, "R1 tap press observed");
	CHECK(press_frames == 1, "R1 tap press visible exactly one frame");
	CHECK(press_pos_ok, "R1 pulse frame mouse_get_pos == tap game coords (150,300)");
	CHECK(key_is(false, false, false), "R1 keys idle after tap");
	CHECK(touch_gesture_get_state() == TG_IDLE, "R1 tap ends in TG_IDLE");

	// -- 1c. rapid taps are never merged or lost (R3) ------------------
	// Power-save pacing means one frame can carry a whole batch:
	// tap1's up plus tap2's down AND up in the same event queue.
	// The game must still observe two distinct press edges.
	reset_peaks();
	push_finger(SDL_FINGERDOWN, 13, tap_x, tap_y);
	SDL_Event batch = {0};
	batch.type = SDL_FINGERUP;
	batch.tfinger.touchId = 1;
	batch.tfinger.fingerId = 13;
	batch.tfinger.x = tap_x;
	batch.tfinger.y = tap_y;
	SDL_PushEvent(&batch);
	batch.type = SDL_FINGERDOWN;
	batch.tfinger.fingerId = 14;
	SDL_PushEvent(&batch);
	batch.type = SDL_FINGERUP;
	SDL_PushEvent(&batch);
	int press_edges = 0;
	bool prev_lb = false;
	for (int i = 0; i < 10; i++) {
		handle_events();
		if (key_state[VK_LBUTTON] && !prev_lb)
			press_edges++;
		prev_lb = key_state[VK_LBUTTON];
	}
	CHECK(press_edges == 2, "R3 rapid taps in one batch: two press edges, none lost");
	CHECK(key_is(false, false, false), "R3 rapid taps: keys released after both pulses");
	CHECK(touch_gesture_get_state() == TG_IDLE, "R3 rapid taps end in TG_IDLE");

	// -- 2. long-press: VK_RBUTTON held at the ORIGINAL position -------
	reset_peaks();
	push_finger(SDL_FINGERDOWN, 2, 0.25f, 0.25f);
	bool held = wait_until(rb_down, 1500, "long-press VK_RBUTTON");
	CHECK(held, "350ms long-press -> VK_RBUTTON held");
	CHECK(!lb_down() && !ct_down(), "long-press does not touch other keys");
	int gx, gy;
	mouse_get_pos(&gx, &gy);
	// finger at window (400,300) -> viewport (240,180) -> game (192,144)
	CHECK(gx == 192 && gy == 144, "long-press reports the touch origin");
	// small drift must NOT move the reported position
	push_finger(SDL_FINGERMOTION, 2, 0.30f, 0.30f);
	mouse_get_pos(&gx, &gy);
	CHECK(gx == 192 && gy == 144, "HOLDING position ignores finger drift");
	push_finger(SDL_FINGERUP, 2, 0.30f, 0.30f);
	pump(8);
	CHECK(key_is(false, false, false), "finger up releases VK_RBUTTON");

	// -- 3. drag: VK_LBUTTON held, cursor follows the finger -----------
	reset_peaks();
	push_finger(SDL_FINGERDOWN, 3, 0.10f, 0.10f);
	push_finger(SDL_FINGERMOTION, 3, 0.60f, 0.60f); // way past threshold
	CHECK(lb_down(), "drag past threshold -> VK_LBUTTON held");
	CHECK(touch_gesture_get_state() == TG_DRAGGING, "state is TG_DRAGGING");
	mouse_get_pos(&gx, &gy);
	// finger at window (960,720) -> viewport (800,600) -> game (640,480)
	CHECK(gx == 640 && gy == 480, "drag cursor follows the finger");
	push_finger(SDL_FINGERMOTION, 3, 0.30f, 0.30f);
	mouse_get_pos(&gx, &gy);
	CHECK(gx == 256 && gy == 192, "drag cursor tracks motion (480,360->256,192)");
	push_finger(SDL_FINGERUP, 3, 0.30f, 0.30f);
	pump(8);
	CHECK(key_is(false, false, false), "drag release clears VK_LBUTTON");

	// -- 4. two-finger swipe down -> wheel backward ---------------------
	reset_peaks();
	mouse_clear_wheel();
	push_finger(SDL_FINGERDOWN, 4, 0.50f, 0.30f);
	push_finger(SDL_FINGERDOWN, 5, 0.60f, 0.30f);
	CHECK(touch_gesture_get_state() == TG_MULTI_TOUCH, "second finger -> MULTI_TOUCH");
	push_finger(SDL_FINGERMOTION, 4, 0.50f, 0.40f);
	push_finger(SDL_FINGERMOTION, 5, 0.60f, 0.40f);
	int fwd, back;
	mouse_get_wheel(&fwd, &back);
	CHECK(back && !fwd, "two-finger swipe down -> wheel backward (backlog)");
	push_finger(SDL_FINGERUP, 4, 0.50f, 0.40f);
	push_finger(SDL_FINGERUP, 5, 0.60f, 0.40f);
	pump(8);

	// -- 5. two-finger swipe up -> VK_CONTROL (fast-forward) ------------
	reset_peaks();
	push_finger(SDL_FINGERDOWN, 6, 0.50f, 0.60f);
	push_finger(SDL_FINGERDOWN, 7, 0.40f, 0.60f);
	push_finger(SDL_FINGERMOTION, 6, 0.50f, 0.45f);
	push_finger(SDL_FINGERMOTION, 7, 0.40f, 0.45f);
	CHECK(ct_down(), "two-finger swipe up -> VK_CONTROL held");
	CHECK(!lb_down() && !rb_down(), "fast-forward leaves buttons alone");
	push_finger(SDL_FINGERUP, 6, 0.50f, 0.45f);
	push_finger(SDL_FINGERUP, 7, 0.40f, 0.45f);
	pump(8);
	CHECK(key_is(false, false, false), "two-finger release clears VK_CONTROL");

	// -- 6. two-finger quick tap -> VK_RBUTTON cancel pulse -------------
	reset_peaks();
	push_finger(SDL_FINGERDOWN, 8, 0.50f, 0.50f);
	push_finger(SDL_FINGERDOWN, 9, 0.55f, 0.50f);
	push_finger(SDL_FINGERUP, 8, 0.50f, 0.50f);
	push_finger(SDL_FINGERUP, 9, 0.55f, 0.50f);
	CHECK(peak_rb, "two-finger quick tap -> VK_RBUTTON cancel pulse");
	pump(8);

	// -- 7. physical input survives the gesture layer -------------------
	// Gesture engine idle; simulate a hardware key press through the
	// real SDL event path and pump frames: the gesture layer must not
	// clobber it.
	SDL_Event k = {0};
	k.type = SDL_KEYDOWN;
	k.key.keysym.scancode = SDL_SCANCODE_LCTRL;
	SDL_PushEvent(&k);
	pump(8);
	CHECK(key_state[VK_CONTROL], "physical Ctrl press survives idle gesture layer");
	for (int i = 0; i < 20; i++)
		handle_events();
	CHECK(key_state[VK_CONTROL], "physical Ctrl stays down across frames (idle gesture layer)");
	k.type = SDL_KEYUP;
	SDL_PushEvent(&k);
	pump(8);
	CHECK(!key_state[VK_CONTROL], "physical Ctrl release honored");

	// -- 8. gesture layer overrides keys only while active ---------------
	reset_peaks();
	push_finger(SDL_FINGERDOWN, 10, 0.30f, 0.30f);
	push_finger(SDL_FINGERDOWN, 11, 0.70f, 0.30f);
	push_finger(SDL_FINGERMOTION, 10, 0.30f, 0.15f);
	push_finger(SDL_FINGERMOTION, 11, 0.70f, 0.15f);
	CHECK(ct_down(), "2-finger swipe-up overrides physical Ctrl state");
	push_finger(SDL_FINGERUP, 10, 0.30f, 0.15f);
	push_finger(SDL_FINGERUP, 11, 0.70f, 0.15f);
	pump(8);
	CHECK(key_is(false, false, false), "gesture end releases owned keys once");

	// -- 9. SYNC mouse events are ignored --------------------------------
	SDL_Event m = {0};
	m.type = SDL_MOUSEBUTTONDOWN;
	m.button.button = SDL_BUTTON_LEFT;
	m.button.which = SDL_TOUCH_MOUSEID;
	SDL_PushEvent(&m);
	pump(8);
	CHECK(!key_state[VK_LBUTTON], "SDL_TOUCH_MOUSEID click ignored (no border rule)");

	// -- 10. governor integration through handle_events -------------------
	governor_init(NULL);
	CHECK(governor_get_state() == GOV_ACTIVE, "governor active after init");
	// A key event through the real path marks input (no state change
	// expected while fresh, but it must not crash and must stay active).
	k.type = SDL_KEYDOWN;
	k.key.keysym.scancode = SDL_SCANCODE_A;
	SDL_PushEvent(&k);
	k.type = SDL_KEYUP;
	SDL_PushEvent(&k);
	pump(8);
	CHECK(governor_get_state() == GOV_ACTIVE, "key input keeps governor active");

	printf(failures ? "INPUT INTEGRATION: %d FAILURES\n" : "ALL INPUT INTEGRATION TESTS PASSED\n",
	       failures);
	return failures ? 1 : 0;
}
