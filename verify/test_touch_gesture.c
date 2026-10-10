/* Behavioral unit tests for the touch gesture engine (host-runnable).
 * Build & run:  make -C verify test
 */
#include <assert.h>
#include <stdio.h>

#include "touch_gesture.h"

static int haptic_count;
static uint32_t haptic_last_ms;

static void test_haptic(uint32_t ms)
{
	haptic_count++;
	haptic_last_ms = ms;
}

// 800x600 window, 100px black border on the left, game resolution 1024x768.
static void init_engine(void)
{
	struct tg_config cfg = {
		.screen_w = 800, .screen_h = 600,
		.viewport_x = 100, .viewport_y = 0,
		.viewport_w = 700, .viewport_h = 600,
		.game_w = 1024, .game_h = 768,
		.hold_ms = 350, .drag_threshold = 0.015f, .scroll_step = 0.02f,
	};
	haptic_count = 0;
	touch_gesture_init(&cfg);
	touch_gesture_set_haptic_hook(test_haptic);
}

static void feed(uint32_t now, uint32_t id, float x, float y, int down_up_move)
{
	if (down_up_move > 0)
		touch_gesture_finger_down(now, id, x, y);
	else if (down_up_move < 0)
		touch_gesture_finger_up(now, id, x, y);
	else
		touch_gesture_finger_motion(now, id, x, y);
}

// The frame loop: tick then poll.
static struct tg_output frame(uint32_t now)
{
	touch_gesture_tick(now);
	return touch_gesture_poll();
}

static void test_tap(void)
{
	init_engine();
	feed(100, 1, 0.5f, 0.5f, +1);
	struct tg_output o = frame(200);
	assert(!o.lbutton && !o.rbutton && !o.control);
	feed(220, 1, 0.5f, 0.5f, -1);
	o = frame(230);
	assert(o.lbutton_pulse && "tap must pulse VK_LBUTTON");
	assert(!o.lbutton && !o.rbutton);
	o = frame(240); // pulse is one-shot
	assert(!o.lbutton_pulse);
	assert(touch_gesture_get_state() == TG_IDLE);
	printf("PASS tap -> VK_LBUTTON pulse\n");
}

// Upstream bug abolished: a tap on the letterbox border is a normal left
// tap, NOT a right click. Long-press in place is the only right-click.
static void test_no_black_border_rightclick(void)
{
	init_engine();
	feed(0, 1, 0.01f, 0.5f, +1); // x=8px < viewport_x=100 => outside viewport
	struct tg_output o = frame(50);
	feed(60, 1, 0.01f, 0.5f, -1);
	o = frame(70);
	assert(o.lbutton_pulse && "border tap must still be a left tap");
	assert(!o.rbutton && !o.rbutton_pulse && "black-border right-click must be abolished");
	printf("PASS black-border tap is a left tap (upstream rule abolished)\n");
}

static void test_long_press_in_place_rightclick(void)
{
	init_engine();
	feed(1000, 1, 0.5f, 0.5f, +1);
	struct tg_output o = frame(1200);
	assert(touch_gesture_get_state() == TG_PENDING_TAP);
	o = frame(1000 + 349);
	assert(touch_gesture_get_state() == TG_PENDING_TAP && "349ms is still pending");
	o = frame(1000 + 350);
	assert(touch_gesture_get_state() == TG_HOLDING && "350ms -> HOLDING");
	assert(o.rbutton && "long-press holds VK_RBUTTON");
	assert(!o.lbutton);
	assert(haptic_count == 1 && haptic_last_ms == 30 && "haptic hook fired once");
	// position stays at the touch origin; viewport mapping:
	// wx=400,wy=300 -> game ((400-100)*1024/700, 300*768/600) = (438, 384)
	assert(o.game_x == 438 && o.game_y == 384 && "viewport-mapped game coords");
	feed(1500, 1, 0.5f, 0.5f, -1);
	o = frame(1510);
	assert(!o.rbutton && "release drops VK_RBUTTON");
	assert(touch_gesture_get_state() == TG_IDLE);
	printf("PASS long-press 350ms -> in-place VK_RBUTTON + haptic\n");
}

static void test_drag(void)
{
	init_engine();
	feed(0, 1, 0.5f, 0.5f, +1);
	frame(10);
	// 10% of width = 80px > 1.5% diagonal (~15.3px)
	feed(20, 1, 0.6f, 0.5f, 0);
	struct tg_output o = frame(30);
	assert(touch_gesture_get_state() == TG_DRAGGING);
	assert(o.lbutton && "dragging holds VK_LBUTTON");
	feed(40, 1, 0.7f, 0.5f, 0);
	o = frame(50);
	assert(o.lbutton);
	feed(60, 1, 0.7f, 0.5f, -1);
	o = frame(70);
	assert(!o.lbutton && "finger up releases VK_LBUTTON");
	printf("PASS drag >1.5%% diagonal -> VK_LBUTTON held, released on finger up\n");
}

static void test_small_jitter_is_still_a_tap(void)
{
	init_engine();
	feed(0, 1, 0.5f, 0.5f, +1);
	frame(10);
	feed(20, 1, 0.505f, 0.5f, 0); // 4px < threshold
	assert(touch_gesture_get_state() == TG_PENDING_TAP);
	feed(100, 1, 0.505f, 0.5f, -1);
	struct tg_output o = frame(110);
	assert(o.lbutton_pulse);
	printf("PASS sub-threshold jitter stays a tap\n");
}

static void test_two_finger_swipe_down_wheel(void)
{
	init_engine();
	feed(0, 1, 0.3f, 0.3f, +1);
	feed(10, 2, 0.7f, 0.3f, +1);
	assert(touch_gesture_get_state() == TG_MULTI_TOUCH);
	struct tg_output o = frame(20);
	assert(!o.lbutton && !o.rbutton && "multi-touch cancels pending click");
	// avg y 0.30 -> 0.35: crosses one 0.02 step
	feed(30, 1, 0.3f, 0.4f, 0);
	feed(30, 2, 0.7f, 0.3f, 0);
	o = frame(40);
	assert(o.wheel == 1 && "swipe down emits positive wheel (backlog)");
	// further movement crosses another step (0.40+0.30)/2=0.35 -> 0.42
	feed(50, 1, 0.3f, 0.54f, 0);
	feed(50, 2, 0.7f, 0.3f, 0);
	o = frame(60);
	assert(o.wheel == 1 && "one wheel step per 2% of height");
	feed(70, 1, 0.3f, 0.54f, -1);
	feed(80, 2, 0.7f, 0.3f, -1);
	o = frame(90);
	assert(!o.control && !o.rbutton_pulse && !o.lbutton_pulse);
	assert(touch_gesture_get_state() == TG_IDLE);
	printf("PASS two-finger swipe down -> mouse wheel (backlog)\n");
}

static void test_two_finger_swipe_up_control(void)
{
	init_engine();
	feed(0, 1, 0.3f, 0.6f, +1);
	feed(10, 2, 0.7f, 0.6f, +1);
	frame(20);
	feed(30, 1, 0.3f, 0.5f, 0);
	feed(30, 2, 0.7f, 0.5f, 0);
	struct tg_output o = frame(40);
	assert(o.control && "swipe up holds VK_CONTROL (fast-forward)");
	feed(50, 1, 0.3f, 0.4f, 0);
	feed(50, 2, 0.7f, 0.4f, 0);
	o = frame(60);
	assert(o.control && "still held while swiping");
	feed(70, 1, 0.3f, 0.4f, -1);
	feed(80, 2, 0.7f, 0.4f, -1);
	o = frame(90);
	assert(!o.control && "released when both fingers are up");
	printf("PASS two-finger swipe up -> VK_CONTROL fast-forward\n");
}

static void test_two_finger_hold_control(void)
{
	init_engine();
	feed(0, 1, 0.3f, 0.5f, +1);
	feed(10, 2, 0.7f, 0.5f, +1);
	struct tg_output o = frame(20);
	assert(!o.control);
	o = frame(360);
	assert(o.control && "2-finger hold >=350ms -> VK_CONTROL");
	// lifting one finger aborts
	feed(400, 2, 0.7f, 0.5f, -1);
	o = frame(410);
	assert(!o.control && "abort releases VK_CONTROL");
	// remaining finger must be inert
	feed(420, 1, 0.3f, 0.2f, 0);
	o = frame(430);
	assert(!o.lbutton && !o.lbutton_pulse && !o.control);
	feed(440, 1, 0.3f, 0.2f, -1);
	o = frame(450);
	assert(!o.lbutton_pulse && touch_gesture_get_state() == TG_IDLE);
	printf("PASS two-finger hold -> VK_CONTROL, abort on finger lift\n");
}

static void test_two_finger_tap_cancel(void)
{
	init_engine();
	feed(0, 1, 0.3f, 0.5f, +1);
	feed(10, 2, 0.7f, 0.5f, +1);
	frame(20);
	feed(100, 1, 0.3f, 0.5f, -1);
	feed(110, 2, 0.7f, 0.5f, -1);
	struct tg_output o = frame(120);
	assert(o.rbutton_pulse && "2-finger quick tap = right-click cancel");
	assert(!o.control && !o.lbutton_pulse);
	o = frame(130);
	assert(!o.rbutton_pulse && "pulse is one-shot");
	printf("PASS two-finger tap -> VK_RBUTTON cancel pulse\n");
}

// A long-press that is then disturbed by a second finger must not leave
// VK_RBUTTON stuck.
static void test_hold_cancelled_by_second_finger(void)
{
	init_engine();
	feed(0, 1, 0.5f, 0.5f, +1);
	struct tg_output o = frame(400);
	assert(o.rbutton);
	feed(410, 2, 0.2f, 0.8f, +1);
	o = frame(420);
	assert(!o.rbutton && "second finger cancels the held right-click");
	assert(touch_gesture_get_state() == TG_MULTI_TOUCH);
	feed(500, 1, 0.5f, 0.5f, -1);
	feed(510, 2, 0.2f, 0.8f, -1);
	assert(touch_gesture_get_state() == TG_IDLE);
	printf("PASS second finger cancels an in-flight long-press\n");
}

int main(void)
{
	test_tap();
	test_no_black_border_rightclick();
	test_long_press_in_place_rightclick();
	test_drag();
	test_small_jitter_is_still_a_tap();
	test_two_finger_swipe_down_wheel();
	test_two_finger_swipe_up_control();
	test_two_finger_hold_control();
	test_two_finger_tap_cancel();
	test_hold_cancelled_by_second_finger();
	printf("ALL GESTURE TESTS PASSED\n");
	return 0;
}