# ESP Serial2WiFi, derived from JeeLabs esp-link and esphttpd; see LICENSE.
.DEFAULT_GOAL := all
.NOTPARALLEL:
-include config.mk

# Explicit VERSION values win; builds otherwise identify their actual Git source.
ifeq ($(origin VERSION),undefined)
ifneq ($(filter-out setup help clean ui check-browser check-layout format check-format,$(or $(MAKECMDGOALS),all)),)
VERSION := $(shell python3 tools/build/write_config.py --print-version)
ifneq ($(.SHELLSTATUS),0)
$(error Cannot determine firmware version; supply VERSION=... when building without Git metadata)
endif
else
VERSION := unversioned
endif
endif
FLASH_SIZE ?= 4MB
TOOLS_DIR ?= $(abspath .cache/esp8266)
SDK_BASE ?= $(TOOLS_DIR)/ESP8266_NONOS_SDK-3.0.6
XTENSA_TOOLS_ROOT ?= $(TOOLS_DIR)/xtensa-lx106-elf/bin/
SDK_BASE := $(abspath $(SDK_BASE))
XTENSA_TOOLS_ROOT := $(abspath $(XTENSA_TOOLS_ROOT))/
PYTHON ?= $(abspath .venv/bin/python)
BOOTFILE := $(SDK_BASE)/bin/boot_v1.6.bin
ESPBAUD ?= 230400
SERIAL_SLOT ?= 1
ESP_HOSTNAME ?= esp-serial2wifi
PORT ?=

# Generic adapter defaults. Select pins in the UI for your actual wiring.
MCU_RESET_PIN ?= -1
MCU_ISP_PIN ?= -1
LED_CONN_PIN ?= -1
LED_SERIAL_PIN ?= -1
CHIP_IN_HOSTNAME ?= 1
MODULES ?= mqtt rest socket web-server syslog
CHANGE_TO_STA ?= yes
GZIP_COMPRESSION ?= yes
SERIAL2WIFI_DEBUG ?= 0
SERIAL2WIFI_OTA_ENABLED ?= $(if $(filter 4MB,$(FLASH_SIZE)),1,0)
SERIAL2WIFI_OTA_TESTING ?= 0
# Experimental, documented separately; public releases use embedded UI.
SERIAL2WIFI_EXTERNAL_UI ?= 0
ifeq ($(SERIAL2WIFI_EXTERNAL_UI),1)
ifneq ($(FLASH_SIZE),4MB)
$(error External UI requires FLASH_SIZE=4MB)
endif
endif
UI_LAYOUT_ARG := $(if $(filter 1,$(SERIAL2WIFI_EXTERNAL_UI)),--external-ui,)
ifneq ($(filter $(SERIAL_SLOT),1 2),$(SERIAL_SLOT))
$(error SERIAL_SLOT must be 1 or 2)
endif

include mk/firmware.mk

.PHONY: setup help ui check-browser check-layout
setup:
	python3 tools/setup.py $(if $(filter 1,$(OFFLINE)),--offline,) --base "$(TOOLS_DIR)"

ui:
	$(PYTHON) tools/build/build_ui.py web

check-browser:
	$(PYTHON) tests/run.py browser --browser "$(BROWSER)"

check-layout:
	$(PYTHON) tests/run.py layout --browser "$(BROWSER)"

help:
	@echo "make setup                         Install local build inputs"
	@echo "make [FLASH_SIZE=4MB|512KB]         Build firmware (default: 4MB)"
	@echo "make check                         Run native and model tests"
	@echo "make release FLASH_SIZE=...        Package a capacity-specific archive"
	@echo "make check-browser BROWSER=/path    Run isolated browser tests"
	@echo "make check-layout BROWSER=/path     Run all-screen layout checks"
	@echo "make factory-flash PORT=/dev/...    Erase and install; back up first"
	@echo "make flash PORT=/dev/... SERIAL_SLOT=1  Update only the selected app slot"
	@echo "make format / check-format         Apply or check source formatting"
	@echo "make clean                         Remove build output, keep dependencies"

.PHONY: format check-format
format:
	$(PYTHON) tools/build/format.py

check-format:
	$(PYTHON) tools/build/format.py --check
