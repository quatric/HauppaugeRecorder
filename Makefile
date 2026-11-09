CC      ?= clang
PREFIX  ?= /usr/local

# Universal 2 (Intel + Apple silicon). 10.9 is the oldest macOS current
# toolchains can still target (libarclite is gone from the SDK, so the ARC file is
# compiled with -fobjc-arc but linked without it; libobjc has the ARC calls since 10.7); the arm64 slice is always 11.0 or later.
VERSION      ?= 0.3.1
ARCHS        ?= x86_64 arm64
MACOSX_MIN   ?= 10.9
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
APP     = $(BUILD)/HauppaugeRecorder.app
# A small LGPL ffmpeg (scripts/build-ffmpeg.sh) goes into the app for the
# preview and the sound. BUNDLE_FFMPEG=0 skips it (then ffmpeg comes from PATH).
BUNDLE_FFMPEG ?= 1
FFMPEG   = $(BUILD)/ffmpeg
ENGINE   = $(BUILD)/hdpvr.o $(BUILD)/app_core.o $(BUILD)/decoder.o $(BUILD)/sink.o

all: $(BUILD)/hdpvr-capture app

$(FFMPEG): scripts/build-ffmpeg.sh
	for a in $(ARCHS); do scripts/build-ffmpeg.sh macos-$$a $(BUILD)/ffmpeg-build/out/ffmpeg-$$a || exit 1; done
	lipo -create $(foreach a,$(ARCHS),$(BUILD)/ffmpeg-build/out/ffmpeg-$(a)) -output $@
	codesign --force --sign - $@

ffmpeg: $(FFMPEG)

$(BUILD):
	mkdir -p $(BUILD)/libusb

$(LIBUSB): $(LUSRC) $(V)/config/macos/config.h | $(BUILD)
	cd $(BUILD)/libusb && for f in $(LUSRC); do \
	    $(CC) $(LUFLAGS) -I$(CURDIR)/$(V) -I$(CURDIR)/$(V)/config/macos -c $(CURDIR)/$$f || exit 1; done
	rm -f $@ && ar rcs $@ $(BUILD)/libusb/*.o 2>/dev/null

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/hdpvr-capture: src/capture.c $(BUILD)/hdpvr.o $(LIBUSB)
	$(CC) $(CFLAGS) $^ $(SYSLIBS) -o $@

$(BUILD)/hdpvr-capture-app: src/viewer.m $(ENGINE) $(LIBUSB)
	$(CC) $(CFLAGS) -fobjc-arc -c src/viewer.m -o $(BUILD)/viewer.o
	$(CC) $(CFLAGS) $(BUILD)/viewer.o $(filter-out src/viewer.m,$^) $(SYSLIBS) \
	    -framework Cocoa -framework AVFoundation -framework QuartzCore -framework AudioUnit \
	    -framework AudioToolbox -framework CoreAudio -o $@

app: $(BUILD)/hdpvr-capture-app resources/Info.plist resources/AppIcon.icns $(if $(filter 1,$(BUNDLE_FFMPEG)),$(FFMPEG))
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	sed -e "s/@MACOSX_MIN@/$(MACOSX_MIN)/" -e "s/@VERSION@/$(VERSION)/" resources/Info.plist > "$(APP)/Contents/Info.plist"
	cp resources/AppIcon.icns "$(APP)/Contents/Resources/AppIcon.icns"
	cp $(BUILD)/hdpvr-capture-app "$(APP)/Contents/MacOS/HauppaugeRecorder"
	if [ "$(BUNDLE_FFMPEG)" = 1 ]; then \
	    cp $(FFMPEG) "$(APP)/Contents/MacOS/ffmpeg" && \
	    mkdir -p "$(APP)/Contents/Resources/licenses" && cp resources/licenses/* "$(APP)/Contents/Resources/licenses/"; fi
	codesign --force --sign - "$(APP)"

# engine test without a device: plays a recorded transport stream through it
$(BUILD)/test_engine: tests/test_engine.c $(ENGINE) $(LIBUSB)
	$(CC) $(CFLAGS) $^ $(SYSLIBS) -o $@

test: $(BUILD)/test_engine
	@[ -n "$(SAMPLE)" ] || { echo "make test SAMPLE=recording.ts"; exit 1; }
	mkdir -p $(BUILD)/test-out
	HDPVR_FFMPEG=$${HDPVR_FFMPEG:-$(CURDIR)/$(FFMPEG)} HDPVR_REPLAY="$(SAMPLE)" $(BUILD)/test_engine $(BUILD)/test-out

install: all
	install -d $(PREFIX)/bin
	install -m 755 $(BUILD)/hdpvr-capture $(PREFIX)/bin/
	cp -R "$(APP)" /Applications/

clean:
	rm -rf $(BUILD)

.PHONY: all app ffmpeg test install clean
