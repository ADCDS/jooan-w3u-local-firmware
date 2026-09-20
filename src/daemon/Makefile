CC ?= cc
CFLAGS ?= -Os -Wall -Wextra
CPPFLAGS += -I.
LDFLAGS ?=
CRYPT_LIBS ?= -lcrypt
TLS ?= 1
SOURCES = main.c util.c crypto.c auth.c integrations.c mqtt_bridge.c mdns.c fmp4.c tls.c http.c
OBJECTS = $(SOURCES:.c=.o)
AUDIO_WIRE_OBJECT = audio_wire_shared.o

ifeq ($(TLS),1)
LDLIBS += -lmbedtls -lmbedx509 -lmbedcrypto -lpthread
else
CPPFLAGS += -DJOAN_NO_TLS
LDLIBS += -lpthread
endif

all: joan-daemon

joan-daemon: $(OBJECTS) $(AUDIO_WIRE_OBJECT)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(AUDIO_WIRE_OBJECT) $(LDLIBS) $(CRYPT_LIBS)

$(AUDIO_WIRE_OBJECT): ../audio/audio_wire.c ../audio/audio_wire.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ ../audio/audio_wire.c

clean:
	rm -f $(OBJECTS) $(AUDIO_WIRE_OBJECT) joan-daemon

.PHONY: all clean
