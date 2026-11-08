CC      ?= clang
PREFIX  ?= /usr/local

# Universal 2 (Intel + Apple silicon); the arm64 slice is always 11.0 or later.
ARCHS        ?= x86_64 arm64
MACOSX_MIN   ?= 10.11
ARCHFLAGS     = $(foreach a,$(ARCHS),-arch $(a)) -mmacosx-version-min=$(MACOSX_MIN)

V        = vendor/libusb
CFLAGS  += -O2 -Wall -Wextra -std=gnu11 -Isrc -I$(V) $(ARCHFLAGS)
# libusb is vendored and linked statically, so nothing needs installing
LUFLAGS  = -O2 -std=gnu11 $(ARCHFLAGS) -w
LUSRC    = $(V)/core.c $(V)/descriptor.c $(V)/hotplug.c $(V)/io.c $(V)/strerror.c $(V)/sync.c \
           $(V)/os/events_posix.c $(V)/os/threads_posix.c $(V)/os/darwin_usb.c
SYSLIBS  = -framework IOKit -framework CoreFoundation -framework Security

BUILD   = build
LIBUSB  = $(BUILD)/libusb-1.0.a

all: $(BUILD)/hdpvr-capture

$(BUILD):
	mkdir -p $(BUILD)/libusb

$(LIBUSB): $(LUSRC) $(V)/config/macos/config.h | $(BUILD)
	cd $(BUILD)/libusb && for f in $(LUSRC); do \
	    $(CC) $(LUFLAGS) -I$(CURDIR)/$(V) -I$(CURDIR)/$(V)/config/macos -c $(CURDIR)/$$f || exit 1; done
	rm -f $@ && ar rcs $@ $(BUILD)/libusb/*.o 2>/dev/null

$(BUILD)/hdpvr.o: src/hdpvr.c src/hdpvr.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/hdpvr-capture: src/capture.c $(BUILD)/hdpvr.o $(LIBUSB)
	$(CC) $(CFLAGS) $^ $(SYSLIBS) -o $@

install: all
	install -d $(PREFIX)/bin
	install -m 755 $(BUILD)/hdpvr-capture $(PREFIX)/bin/

clean:
	rm -rf $(BUILD)

.PHONY: all install clean
