# Gin corresponding-source module build for existing OP1R tooling.
PYTHON ?= python3
NODE ?= node
CHROMIUM ?=
STOCK ?=
BUILD ?= $(CURDIR)/build
IMAGE ?= gin-bfin:1.0.0
TOOLCHAIN ?= $(CURDIR)/.toolchain
ROOT := $(CURDIR)
CHROME_ARG = $(if $(CHROMIUM),--chromium "$(CHROMIUM)",)
.PHONY: inputs data module image toolchain
inputs: $(BUILD)/stock-inputs/ASSETS.private.json
$(BUILD)/stock-inputs/ASSETS.private.json:
	$(PYTHON) tools/tonic/release/prepare_stock_assets.py "$(STOCK)" "$(BUILD)/stock-inputs" --node "$(NODE)" $(CHROME_ARG)
data: $(BUILD)/builtin/builtin.kit.bin $(BUILD)/factory-data/FACTORY_PACK.json
$(BUILD)/builtin/builtin.kit.bin:
	$(PYTHON) tools/tonic/release/build_bootstrap_data.py bootstrap "$(BUILD)/builtin"
$(BUILD)/factory-data/FACTORY_PACK.json:
	$(PYTHON) tools/tonic/sdk-drum/build_factory_data.py --out "$(BUILD)/factory-data"
module: inputs data
	mkdir -p "$(BUILD)/module"
	docker run --rm --pull never --network none --read-only --platform linux/amd64 --entrypoint /bin/bash \
	 -e GIN_WORKING_SLOTS=1 -e GIN_PROFILE_ENABLED=0 -e GIN_PARAMS_ONLY=0 -e GIN_FW_OUTPUT_SHIFT=16 \
	 -e GIN_SINE_QUARTER=0 -e GIN_LEVEL_TRACE=0 -e GIN_SERVICE_TRACE=0 -e GIN_STOCK_PROTECTED_CONTROL=0 \
	 -e GIN_SYNTH_VOICE_LIMIT=4 -e GIN_RATE_2X=1 -e GIN_BLOCK_STATE=1 -e GIN_FACTORY_PACKED_INDEX=1 \
	 -v "$(TOOLCHAIN):/workspace/.toolchain:ro" -v "$(ROOT)/sdk:/source:ro" \
	 -v "$(ROOT)/src/tonic/sdk_drum:/owned:ro" -v "$(ROOT)/tools/tonic/sdk-drum:/tools:ro" \
	 -v "$(BUILD)/builtin:/data:ro" -v "$(ROOT)/src/tonic/ui:/ui:ro" \
	 -v "$(ROOT)/src/tonic/ui_native:/scene:ro" -v "$(ROOT)/src/tonic/integer:/integer:ro" \
	 -v "$(ROOT)/src/tonic:/tonic:ro" -v "$(ROOT)/src/tonic/presets:/presets:ro" \
	 -v "$(ROOT)/third_party/libpo32/core:/po:ro" -v "$(BUILD)/stock-inputs:/font:ro" \
	 -v "$(BUILD)/factory-data:/factory-data:ro" -v "$(ROOT)/tools/tonic/ui-native:/ui-tools:ro" \
	 -v "$(BUILD)/module:/out" "$(IMAGE)" /tools/build_ui_candidate.sh
image:
	docker build --platform linux/amd64 -t "$(IMAGE)" -f tools/tonic/release/toolchain.Dockerfile .
toolchain: image
	docker run --rm --platform linux/amd64 -v "$(ROOT):/workspace" -e BFIN_PREFIX=/workspace/.toolchain \
	 -e BFIN_CACHE=/workspace/.cache/bfin-src "$(IMAGE)" bash tools/tonic/release/build_toolchain.sh
