/*
 * Governor behaviour metrics with a deterministic fake clock.
 *
 * governor.o is linked against THIS file's SDL_GetTicks/SDL_WaitEventTimeout
 * stubs (no libSDL2), so every millisecond number below is exact.
 * Prints a JSON object with the measured metrics plus pass/fail
 * assertions; exit code 0 iff all assertions hold.
 */
#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>

#include "governor.h"

// ---- fake clock + SDL stubs (win over libSDL2: we do not link it) ----
static uint32_t fake_now;
Uint32 SDL_GetTicks(void) { return fake_now; }
int SDL_WaitEventTimeout(SDL_Event *e, int t) { (void)e; (void)t; return 0; }

bool scene_is_dirty = false;

static int failures;
static void expect(const char *name, double got, double want_lo, double want_hi)
{
	bool ok = got >= want_lo && got <= want_hi;
	printf("  \"%s\": %.1f, \"_want\": \"[%g, %g]\", \"_ok\": %s,\n",
	       name, got, want_lo, want_hi, ok ? "true" : "false");
	if (!ok) {
		fprintf(stderr, "METRIC FAIL: %s = %.1f (want [%g, %g])\n",
		        name, got, want_lo, want_hi);
		failures++;
	}
}

int main(void)
{
	printf("{\n");

	// ---- init ---------------------------------------------------------
	fake_now = 10000;
	governor_init(NULL); // defaults: 16 / 100 / 2000

	// ---- 1. idle_enter: exactly 2000 ms of stillness -> POWER_SAVE ----
	uint32_t t0 = fake_now;
	uint32_t entered_at = 0;
	for (int ms = 0; ms <= 2500; ms += 2) {
		fake_now = t0 + ms;
		governor_tick(fake_now);
		if (governor_power_save()) {
			entered_at = fake_now - t0;
			break;
		}
	}
	expect("power_save_enter_ms", entered_at, 2000, 2010);

	// ---- 2. static-scene pump cadence: 100 ms frame slots -------------
	// Simulate the engine pump: step the clock in 5 ms increments; each
	// step calls governor_ms_until_next_frame (as fire_deferred_events
	// does) and consumes a render slot when should_render grants one.
	uint32_t last_grant = fake_now;
	double grant_intervals[16];
	int n_grants = 0;
	uint32_t slot_start = fake_now;
	double slot_intervals[16];
	int n_slots = 0;
	bool warmup = true; // the first grant closes a pre-static interval
	for (int ms = 0; ms < 700 && n_grants < 6; ms += 2) {
		fake_now = slot_start + ms;
		governor_tick(fake_now);
		uint32_t remain = governor_ms_until_next_frame(fake_now);
		if (remain == 0 && ms > 0) {
			slot_intervals[n_slots++] = (double)(fake_now - slot_start);
			slot_start = fake_now;
		}
		(void)remain;
		if (governor_should_render(fake_now)) {
			if (warmup) {
				warmup = false; // interval back to the pre-static era
			} else {
				grant_intervals[n_grants++] = (double)(fake_now - last_grant);
			}
			last_grant = fake_now;
		}
	}
	double mean = 0;
	for (int i = 0; i < n_grants; i++)
		mean += grant_intervals[i];
	mean /= n_grants > 0 ? n_grants : 1;
	expect("static_render_interval_mean_ms", mean, 95, 115);
	double slot_mean = 0;
	for (int i = 0; i < n_slots; i++)
		slot_mean += slot_intervals[i];
	slot_mean /= n_slots > 0 ? n_slots : 1;
	expect("static_pump_slot_mean_ms", slot_mean, 95, 112);

	// ---- 3. input wake latency: 0 ms (force-render latch) -------------
	governor_tick(fake_now);
	if (!governor_power_save()) {
		fprintf(stderr, "PRECONDITION FAIL: not in power save before wake test\n");
		failures++;
	}
	uint32_t wake_t = fake_now;
	governor_mark_input(wake_t);
	bool active_same_tick = !governor_power_save();
	bool render_same_tick = governor_should_render(wake_t);
	double wake_latency = (active_same_tick && render_same_tick) ? 0.0 : 1e9;
	expect("input_wake_latency_ms", wake_latency, 0, 0);

	// ---- 4. active pacing: 16 ms floor ---------------------------------
	// After the forced wake render, subsequent renders are paced by
	// active_frame_ms.
	uint32_t first_paced = 0;
	for (int ms = 1; ms <= 40; ms++) {
		fake_now = wake_t + ms;
		if (governor_should_render(fake_now)) {
			first_paced = ms;
			break;
		}
	}
	expect("active_frame_floor_ms", first_paced, 16, 17);

	// ---- 5. scene-dirty wake latency: 0 ms -----------------------------
	// Back to power save, then mark the scene dirty.
	fake_now = wake_t + 2100;
	governor_tick(fake_now);
	if (!governor_power_save()) {
		fprintf(stderr, "PRECONDITION FAIL: not in power save before dirty test\n");
		failures++;
	}
	uint32_t dirty_t = fake_now;
	governor_mark_scene_dirty();
	bool render_after_dirty = governor_should_render(dirty_t);
	double dirty_latency = (!governor_power_save() && render_after_dirty) ? 0.0 : 1e9;
	expect("dirty_wake_latency_ms", dirty_latency, 0, 0);

	// ---- 6. render-request wake: an animating scene resumes full rate --
	fake_now = dirty_t + 2100;
	governor_tick(fake_now);
	if (!governor_power_save()) {
		fprintf(stderr, "PRECONDITION FAIL: not in power save before rr test\n");
		failures++;
	}
	governor_note_render_request(fake_now);
	bool rr_ok = !governor_power_save() && governor_should_render(fake_now);
	expect("render_request_wake_ok", rr_ok ? 1.0 : 0.0, 1, 1);

	printf("  \"assertions_failed\": %d\n}\n", failures);
	return failures ? 1 : 0;
}
