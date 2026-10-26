# Convenience wrappers around the CMake builds.
#
# The default Unix Makefiles tree in build/ stays the primary one. The Xcode
# tree exists so Metal GPU frame capture and Instruments can drive the sample,
# which the Makefiles generator cannot do. Both trees can coexist; they share no
# state, so switching between them never forces a reconfigure of the other.

XCODE_DIR ?= build-xcode
BUILD_DIR ?= build
# Release matters more than it looks: terrain package expansion and Tree64
# construction are substantially slower at -O0, and Xcode's default scheme
# otherwise attaches a debugger on top of that. Capture and profiling should
# use this tree.
RELEASE_DIR ?= build-release
DEVELOPER_DIR ?= /Applications/Xcode.app/Contents/Developer
SAMPLE ?= SparseVoxelRendererSample
JOBS ?= 8

# Xcode is a multi-config generator, so one generate serves Debug and Release
# and the configuration is chosen at build time rather than at configure time.
XCODE_CONFIGS ?= Debug;Release

# Launch arguments baked into the generated Xcode scheme, so opening the project
# needs no manual setup. Semicolons are CMake's list separator: one element per
# argument row. Override the whole list with SCHEME_ARGS=... , for example
#   make xcode SCHEME_ARGS="--terrain;--terrain-lod-levels;4"
SPONZA ?= $(CURDIR)/assets/meshes/obj/sponza.obj
TERRAIN_PACKAGE ?= $(CURDIR)/$(RELEASE_DIR)/tests/polygon-terrain-default/planet.planetpkg
SCHEME_ARGS ?= --terrain-package;$(TERRAIN_PACKAGE);--terrain-lod-levels;5;--disable-vsync;--skip-output-signature

# ENGINE_BUILD_SAMPLES defaults to OFF, so the sample target simply does not
# exist in a tree configured without it.
XCODE_FLAGS ?= -DENGINE_BUILD_SAMPLES=ON -DCMAKE_CONFIGURATION_TYPES="$(XCODE_CONFIGS)" \
               -DSPARSE_VOXEL_SCHEME_ARGUMENTS="$(SCHEME_ARGS)"

# Capture settings for the headless MoltenVK path. Scope 2 captures the first
# frame; see docs/xcode-gpu-profiling.md for the other scopes.
# The full pipeline, not --depth-only. Depth-only runs at roughly 8.5ms GPU and
# omits the whole shading and ray chain, and the deferred cost lands on a
# different pass there than it does in a full frame - so it profiles a
# configuration nobody ships.
TRACE ?= $(CURDIR)/$(BUILD_DIR)/tests/frame.gputrace
TRACE_ARGS ?= --test-camera-roof-down --rays-per-pixel 8 --ray-resolution-divisor 2 \
              --disable-vsync --skip-output-signature --frames 3

.PHONY: help release xcode xcode-debug xcode-release xcode-open xcode-clean gputrace gputrace-sponza bench benchmark bench-sponza

help:
	@echo "Optimised build (use this for profiling and capture):"
	@echo "  make release         build $(SAMPLE) into $(RELEASE_DIR)/tests"
	@echo ""
	@echo "Xcode project (for GPU frame capture / Instruments):"
	@echo "  make xcode           generate $(XCODE_DIR) with samples enabled"
	@echo "  make xcode-debug     build $(SAMPLE), Debug"
	@echo "  make xcode-release   build $(SAMPLE), Release"
	@echo "  make xcode-open      open the project in Xcode"
	@echo "  make xcode-clean     remove $(XCODE_DIR)"
	@echo ""
	@echo "Headless capture (no Xcode UI needed, only to view the result):"
	@echo "  make gputrace        write a .gputrace to $(TRACE)"
	@echo "  make gputrace-sponza same, loading the bundled Sponza scene"
	@echo ""
	@echo "Terrain lighting benchmark (camera walks and rotates):"
	@echo "  make bench           write $(BENCH)"
	@echo "  make benchmark       alias for make bench"
	@echo "  make bench-sponza    retain the optional Sponza comparison"
	@echo ""
	@echo "Overridable: XCODE_DIR BUILD_DIR RELEASE_DIR SAMPLE JOBS TRACE TRACE_ARGS SCHEME_ARGS"

release:
	cmake -B $(RELEASE_DIR) -DCMAKE_BUILD_TYPE=Release -DENGINE_BUILD_SAMPLES=ON
	cmake --build $(RELEASE_DIR) --target $(SAMPLE) -j $(JOBS)

# Xcode serialises on one build database per project, so a build started from
# the IDE and one started here abort each other with "database is locked",
# which reads like a corrupt tree rather than a collision. Say so plainly.
.PHONY: xcode-check-idle
xcode-check-idle:
	@if pgrep -f "xcodebuild.*$(SAMPLE)" >/dev/null 2>&1; then \
	  echo "error: an xcodebuild for $(SAMPLE) is already running."; \
	  echo "       Xcode allows one build per project at a time. Wait for it,"; \
	  echo "       or stop it in Xcode (Product > Stop), then retry."; \
	  exit 1; \
	fi

xcode: xcode-check-idle
	DEVELOPER_DIR=$(DEVELOPER_DIR) cmake -G Xcode -B $(XCODE_DIR) $(XCODE_FLAGS)

xcode-debug: xcode
	DEVELOPER_DIR=$(DEVELOPER_DIR) cmake --build $(XCODE_DIR) --target $(SAMPLE) --config Debug -j $(JOBS)

xcode-release: xcode
	DEVELOPER_DIR=$(DEVELOPER_DIR) cmake --build $(XCODE_DIR) --target $(SAMPLE) --config Release -j $(JOBS)

xcode-open: xcode
	open $(XCODE_DIR)/Engine.xcodeproj

xcode-clean:
	rm -rf $(XCODE_DIR)

# MTL_CAPTURE_ENABLED is required or Metal refuses programmatic capture. The
# trace is only flushed on a clean exit, so TRACE_ARGS must bound the run.
gputrace:
	rm -rf $(TRACE)
	cd $(BUILD_DIR)/tests && \
	  MTL_CAPTURE_ENABLED=1 \
	  MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE=2 \
	  MVK_CONFIG_AUTO_GPU_CAPTURE_OUTPUT_FILE=$(TRACE) \
	  ./$(SAMPLE) $(TRACE_ARGS)
	@echo "wrote $(TRACE) — open it with: open $(TRACE)"

# Sponza needs a coarse voxel size: the OBJ is in centimetres and spans ~3721
# units, so a small value blows the cell budget before it ever renders.
#
# The LOD threshold and the camera both matter, and getting either wrong
# captures a frame nobody renders. Headless runs override the interactive LOD
# default with 64 pixels so the bounded smoke check exercises a coarse cut - at
# that threshold the whole of Sponza resolves to *eight* root bricks covering
# 25k pixels, and a capture of it measures nothing. --lod-threshold 1.5 is the
# interactive default and gives ~3200 visible bricks; --test-camera-interior
# puts the camera in the hall instead of outside the model, which is the
# difference between 18k covered pixels and a full 518k screen.
gputrace-sponza:
	$(MAKE) gputrace BUILD_DIR=$(RELEASE_DIR) \
	  TRACE_ARGS="--test-obj-preview $(SPONZA) --voxel-size-meters 5 --lod-threshold 1.5 \
	              --test-camera-interior --disable-vsync --skip-output-signature --frames 3"

# Default benchmark: the production terrain package, five clipmap levels, and
# the camera-walk sequence (forward motion plus a smooth yaw). Instruments/Xcode
# remains authoritative for Metal timings; this JSON records frame pacing and
# validation without enabling the renderer's less reliable pass timeline.
BENCH ?= $(CURDIR)/$(RELEASE_DIR)/tests/terrain-bench.json
bench: release
	cd $(RELEASE_DIR)/tests && ./$(SAMPLE) \
	  --terrain-package "$$PWD/polygon-terrain-default/planet.planetpkg" \
	  --terrain-lod-levels 5 --disable-vsync \
	  --benchmark-lighting $(BENCH) --benchmark-sequence camera-walk \
	  --benchmark-warmup 60 --benchmark-frames 240 \
	  --benchmark-resolution 1280x720
	@echo "wrote $(BENCH)"

benchmark: bench

SPONZA_BENCH ?= $(CURDIR)/$(RELEASE_DIR)/tests/sponza-bench.json
bench-sponza: release
	cd $(RELEASE_DIR)/tests && ./$(SAMPLE) \
	  --test-obj-preview $(SPONZA) --voxel-size-meters 5 --lod-threshold 1.5 \
	  --disable-vsync --benchmark-shadow $(SPONZA_BENCH) --benchmark-sequence interior-yaw \
	  --benchmark-warmup 20 --benchmark-frames 60 --frames 80 \
	  --benchmark-resolution 1920x1080
	@echo "wrote $(SPONZA_BENCH)"
