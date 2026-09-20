CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
WARNINGS = -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wconversion \
	-Wstrict-prototypes -Wmissing-prototypes
BUILD = build
COMMON = guard_policy.c guard_sha256.c
AUDIO_GUARD = ../audio/audio_guard_wire.c ../audio/g711_alaw.c
AUDIO_HEADERS = ../audio/audio_guard_wire.h ../audio/audio_wire.h \
	../audio/audio_mic_wire.h ../audio/g711_alaw.h

.PHONY: all clean test

all: $(BUILD)/libjooan_guard.so

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/libjooan_guard.so: guard_shim.c $(COMMON) $(AUDIO_GUARD) \
	guard.h guard_sha256.h $(AUDIO_HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c99 -fPIC -shared \
		-o $@ guard_shim.c $(COMMON) $(AUDIO_GUARD) -ldl -pthread

$(BUILD)/libjooan_guard_test.so: guard_shim.c $(COMMON) $(AUDIO_GUARD) \
	guard.h guard_sha256.h $(AUDIO_HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c99 -DGUARD_TESTING \
		-fPIC -shared -o $@ guard_shim.c $(COMMON) $(AUDIO_GUARD) -ldl -pthread

$(BUILD)/test_guard: test_guard.c $(COMMON) guard.h guard_sha256.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c99 \
		-o $@ test_guard.c $(COMMON)

$(BUILD)/guard_probe: guard_probe.c guard_policy.c guard_sha256.c $(AUDIO_GUARD) guard.h \
	$(AUDIO_HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c99 \
		-o $@ guard_probe.c guard_policy.c guard_sha256.c $(AUDIO_GUARD) -pthread

$(BUILD)/guard_child_probe: guard_child_probe.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c99 -o $@ guard_child_probe.c

test: $(BUILD)/libjooan_guard.so $(BUILD)/libjooan_guard_test.so \
	$(BUILD)/test_guard $(BUILD)/guard_probe $(BUILD)/guard_child_probe
	$(BUILD)/test_guard
	@set -eu; \
	case "$(CURDIR)" in *:*) echo "test path cannot contain ':'" >&2; exit 1;; esac; \
	test_tmp=$$(mktemp -d /tmp/jooan-guard-test.XXXXXX); \
	trap 'rm -f "$$test_tmp/dsp" "$$test_tmp/talkback.sock" "$$test_tmp/mic.sock" "$$test_tmp/speaker-direction" "$$test_tmp/speaker-value" "$$test_tmp/speaker-ready" "$$test_tmp/speaker-released" "$$test_tmp/jooanipc" "$$test_tmp/rtsp.password" "$$test_tmp/rtsp.synced"; rmdir "$$test_tmp"' EXIT; \
	printf out > "$$test_tmp/speaker-direction"; \
	printf 0 > "$$test_tmp/speaker-value"; \
	printf released > "$$test_tmp/speaker-released"; \
	export JOOAN_GUARD_TEST_EXE="$(CURDIR)/$(BUILD)/guard_probe"; \
	export JOOAN_GUARD_TEST_DSP_PATH="$$test_tmp/dsp"; \
	export JOOAN_GUARD_TEST_TALKBACK_PATH="$$test_tmp/talkback.sock"; \
	export JOOAN_GUARD_TEST_MIC_SOCKET_PATH="$$test_tmp/mic.sock"; \
	export JOOAN_GUARD_TEST_SPEAKER_DIRECTION_PATH="$$test_tmp/speaker-direction"; \
	export JOOAN_GUARD_TEST_SPEAKER_VALUE_PATH="$$test_tmp/speaker-value"; \
	export JOOAN_GUARD_TEST_SPEAKER_READY_PATH="$$test_tmp/speaker-ready"; \
	export JOOAN_GUARD_TEST_SPEAKER_RELEASED_PATH="$$test_tmp/speaker-released"; \
	export JOOAN_GUARD_TEST_FAKE_MIC_IOCTL=1; \
	export JOOAN_GUARD_TEST_FAKE_AO_IOCTL=1; \
	export JOOAN_GUARD_TEST_RTSP_PASSWORD_PATH="$$test_tmp/rtsp.password"; \
	export JOOAN_GUARD_TEST_RTSP_SYNC_PATH="$$test_tmp/rtsp.synced"; \
	export JOOAN_GUARD_TEST_RTSP_GATE_ALL=1; \
	export LD_PRELOAD="$(CURDIR)/$(BUILD)/libjooan_guard_test.so"; \
	$(BUILD)/guard_probe policy; \
	$(BUILD)/guard_probe talkback; \
	$(BUILD)/guard_probe microphone; \
	$(BUILD)/guard_probe child "$(CURDIR)/$(BUILD)/guard_child_probe"; \
	LD_PRELOAD="$(CURDIR)/$(BUILD)/libjooan_guard.so" \
		$(BUILD)/guard_child_probe; \
	cp $(BUILD)/guard_child_probe "$$test_tmp/jooanipc"; \
	chmod 700 "$$test_tmp/jooanipc"; \
	set +e; LD_PRELOAD="$(CURDIR)/$(BUILD)/libjooan_guard.so" \
		"$$test_tmp/jooanipc"; unsupported_rc=$$?; set -e; \
	[ "$$unsupported_rc" = 126 ]; \
	echo "guard preload tests: ok"

clean:
	rm -f $(BUILD)/libjooan_guard.so $(BUILD)/libjooan_guard_test.so \
		$(BUILD)/libjooan_guard.mips-glibc.so $(BUILD)/test_guard \
		$(BUILD)/guard_probe $(BUILD)/guard_child_probe \
		$(BUILD)/guard_child_probe.mips
	@rmdir $(BUILD) 2>/dev/null || :
