# Syntax-level compile check + behavioral tests for the xsystem4 Android
# port additions.
#
# Compiles src/touch_gesture.c and src/governor.c into objects on any
# host that has GCC and SDL2 development headers (pkg-config sdl2).
# No Android NDK required; the real NDK build is driven by the
# CMake superbuild (see PORTING_NOTES.md).
#
# Usage:  make -C verify            # compile check
#         make -C verify test       # gesture FSM unit tests
#         make -C verify test-input # REAL input.c path driven by SDL events
#         make -C verify test-gov   # governor metrics -> governor_metrics.json
#         make -C verify all-tests  # everything, in order

CC         ?= gcc
CFLAGS     ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -O1
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
XSYS4      := ../xsystem4
INC        := -I$(XSYS4)/include
# Full engine include path (for compiling the real input.c).
INC_FULL   := -I$(XSYS4)/src -I$(XSYS4)/include \
              -I$(XSYS4)/subprojects/cglm/include \
              -I$(XSYS4)/subprojects/libsys4/include
CHECK      := touch_gesture.o governor.o

all: check

check: $(CHECK)
	@echo "CHECK OK: $(CHECK) compiled with zero errors"

# Regenerate when the source or its public header changes.
touch_gesture.o: $(XSYS4)/src/touch_gesture.c $(XSYS4)/include/touch_gesture.h
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

governor.o: $(XSYS4)/src/governor.c $(XSYS4)/include/governor.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(INC) -c $< -o $@

TEST_SRC := test_touch_gesture.c

test: check test_touch_gesture
	./test_touch_gesture

test_touch_gesture: $(TEST_SRC) touch_gesture.o
	$(CC) $(CFLAGS) $(INC) $(TEST_SRC) touch_gesture.o -o $@

# Real-path integration test: compiles the REAL src/input.c (via
# #include) with -DXSYS4_TOUCH_GESTURES and drives handle_events()
# through the real SDL event queue.
test-input: test_input_touch

test_input_touch: test_input_touch.c $(XSYS4)/src/input.c touch_gesture.o governor.o
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(INC_FULL) -DXSYS4_TOUCH_GESTURES \
	    test_input_touch.c touch_gesture.o governor.o $(SDL_LIBS) -lm -o $@

# Governor behaviour metrics (fake clock, no SDL link): prints JSON.
test-gov: test_governor_metrics

test_governor_metrics: test_governor_metrics.c governor.o
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(INC) test_governor_metrics.c governor.o -o $@
	./test_governor_metrics > governor_metrics.json
	@echo "wrote governor_metrics.json"

all-tests: test test-input test-gov

clean:
	rm -f $(CHECK) test_touch_gesture test_input_touch test_governor_metrics governor_metrics.json

.PHONY: all check test test-input test-gov all-tests clean
