# BatteryBar - build, bundle and package.
#
#   make            build universal binaries + BatteryBar.app
#   make run        build and launch the app
#   make install    copy BatteryBar.app into /Applications
#   make dist       build the release zip in dist/
#   make clean      remove build artifacts

APP        := BatteryBar
HELPER     := wireless_battery_monitor
VERSION    := 1.0.0
MIN_MACOS  := 11.0

BUILD      := build
DIST       := dist
BUNDLE     := $(BUILD)/$(APP).app
MACOS_DIR  := $(BUNDLE)/Contents/MacOS

# Build for both architectures so the release runs on Apple Silicon and Intel.
ARCHS      := arm64 x86_64

CC         := clang
CFLAGS     := -O2 -Wall -Wextra -mmacosx-version-min=$(MIN_MACOS) \
              $(foreach a,$(ARCHS),-arch $(a))
LDFLAGS    := -framework IOKit -framework CoreFoundation

SWIFTC     := swiftc
# -g is deliberately omitted: debug info would bake absolute build paths into
# the shipped binary. Each arch is compiled separately, then lipo'd together.
SWIFTFLAGS := -O -framework Cocoa

.PHONY: all run install uninstall dist clean

all: $(BUNDLE)

# ---- helper (C, universal in one pass) ----

$(BUILD)/$(HELPER): src/$(HELPER).c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	strip -x $@

# ---- app binary (Swift, one slice per arch then merged) ----

SWIFT_SLICES := $(foreach a,$(ARCHS),$(BUILD)/$(APP)-$(a))

$(BUILD)/$(APP)-%: src/$(APP).swift | $(BUILD)
	$(SWIFTC) $(SWIFTFLAGS) -target $*-apple-macos$(MIN_MACOS) -o $@ $<

$(BUILD)/$(APP): $(SWIFT_SLICES)
	lipo -create $^ -output $@
	strip -x $@

# ---- .app bundle ----

$(BUNDLE): $(BUILD)/$(APP) $(BUILD)/$(HELPER) Info.plist
	rm -rf $(BUNDLE)
	mkdir -p $(MACOS_DIR)
	cp $(BUILD)/$(APP) $(BUILD)/$(HELPER) $(MACOS_DIR)/
	sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@MIN_MACOS@/$(MIN_MACOS)/g' \
	    Info.plist > $(BUNDLE)/Contents/Info.plist
	# Ad-hoc signature: without it Gatekeeper kills unsigned arm64 binaries.
	codesign --force --deep --sign - $(BUNDLE)
	@echo "Built $(BUNDLE)"

$(BUILD):
	mkdir -p $@

# ---- tasks ----

run: $(BUNDLE)
	pkill -x $(APP) || true
	open $(BUNDLE)

install: $(BUNDLE)
	rm -rf /Applications/$(APP).app
	cp -R $(BUNDLE) /Applications/
	@echo "Installed to /Applications/$(APP).app"

uninstall:
	pkill -x $(APP) || true
	rm -rf /Applications/$(APP).app

dist: $(BUNDLE)
	mkdir -p $(DIST)
	rm -f $(DIST)/$(APP)-$(VERSION)-universal.zip
	ditto -c -k --keepParent $(BUNDLE) $(DIST)/$(APP)-$(VERSION)-universal.zip
	cd $(DIST) && shasum -a 256 $(APP)-$(VERSION)-universal.zip > $(APP)-$(VERSION)-universal.zip.sha256
	@echo "Packaged $(DIST)/$(APP)-$(VERSION)-universal.zip"

clean:
	rm -rf $(BUILD) $(DIST)
