CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
LDFLAGS ?=
LDLIBS ?=
PYTHON ?= python3

OBJECTS = g711_alaw.o audio_wire.o audio_guard_wire.o audio_guard.o

.PHONY: all check clean

all: libjooan_audio.a jooan-audio-router

libjooan_audio.a: $(OBJECTS)
	$(AR) rcs $@ $^

test_audio: test_audio.o $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

jooan-audio-router: audio_router.o $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -pthread

check: test_audio jooan-audio-router
	./test_audio
	$(PYTHON) ./test_audio_router.py

clean:
	$(RM) $(OBJECTS) audio_router.o test_audio.o test_audio \
		jooan-audio-router libjooan_audio.a
