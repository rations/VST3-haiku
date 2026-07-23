# Thin wrapper so `make` works the same here as in the sibling Haiku ports.
# The real build system is CMake + ninja (the SDK is CMake-based).
#
# Typical loop from the dev host:
#   rsync -a --delete --exclude build ./ haikulaptop:VST3-haiku/
#   ssh haikulaptop 'cd VST3-haiku && make'

BUILD_DIR ?= build
BUILD_TYPE ?= Release
VST3_INSTALL_DIR ?= $(HOME)/config/non-packaged/add-ons/media/VST3

all: build

configure:
	cmake -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

$(BUILD_DIR)/build.ninja:
	cmake -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: $(BUILD_DIR)/build.ninja
	ninja -C $(BUILD_DIR)

# Copy every built .vst3 bundle into the user's non-packaged add-ons dir.
install: build
	mkdir -p $(VST3_INSTALL_DIR)
	@found=0; \
	for bundle in $$(find $(BUILD_DIR) -type d -name "*.vst3" -not -path "*/CMakeFiles/*"); do \
		found=1; \
		echo "install $$bundle -> $(VST3_INSTALL_DIR)/$$(basename $$bundle)"; \
		rm -rf "$(VST3_INSTALL_DIR)/$$(basename $$bundle)"; \
		cp -r "$$bundle" "$(VST3_INSTALL_DIR)/"; \
	done; \
	if [ $$found -eq 0 ]; then echo "no .vst3 bundles found in $(BUILD_DIR)"; fi

# Run the validator against every installed bundle.
validate:
	@for bundle in $(VST3_INSTALL_DIR)/*.vst3; do \
		[ -d "$$bundle" ] || continue; \
		echo "== validator $$bundle =="; \
		$(BUILD_DIR)/bin/$(BUILD_TYPE)/validator "$$bundle" || exit 2; \
	done

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all configure build install validate clean
