CC ?= cc
CFLAGS ?= -Os -Wall -Wextra
CPPFLAGS += -I.
LDFLAGS ?=
CRYPT_LIBS ?= -lcrypt
TLS ?= 1
SOURCES = main.c util.c crypto.c auth.c integrations.c mqtt_bridge.c mdns.c rtsp_proxy.c tls.c onvif.c http.c
OBJECTS = $(SOURCES:.c=.o)

ifeq ($(TLS),1)
LDLIBS += -lmbedtls -lmbedx509 -lmbedcrypto -lpthread
else
CPPFLAGS += -DJOAN_NO_TLS
LDLIBS += -lpthread
endif

all: joan-daemon

joan-daemon: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS) $(CRYPT_LIBS)

clean:
	rm -f $(OBJECTS) joan-daemon

.PHONY: all clean
