SUPPORTED_FLASH_SIZES := 512KB 1MB 2MB 4MB
ifeq (,$(filter $(FLASH_SIZE),$(SUPPORTED_FLASH_SIZES)))
$(error Unsupported FLASH_SIZE $(FLASH_SIZE))
endif
ESP_FLASH_MAX := $(if $(filter 512KB,$(FLASH_SIZE)),479232,503808)

ifeq ("$(FLASH_SIZE)","512KB")
# Winbond 25Q40 512KB flash, typ for esp-01 thru esp-11
ESP_SPI_SIZE        ?= 0       # 0->512KB (256KB+256KB)
ESP_FLASH_MODE      ?= 2       # DIO
ESP_FLASH_FREQ_DIV  ?= 0       # 40 MHz

else ifeq ("$(FLASH_SIZE)","1MB")
# ESP-01E
ESP_SPI_SIZE        ?= 2       # 2->1MB (512KB+512KB)
ESP_FLASH_MODE      ?= 2       # DIO
ESP_FLASH_FREQ_DIV  ?= 0       # 40 MHz

else ifeq ("$(FLASH_SIZE)","2MB")
# Manuf 0xA1 Chip 0x4015 found on wroom-02 modules
# Here we're using two partitions of approx 0.5MB because that's what's easily available in terms
# of linker scripts in the SDK. Ideally we'd use two partitions of approx 1MB, the remaining 2MB
# cannot be used for code (esp8266 limitation).
ESP_SPI_SIZE        := 3       # 2MB, 512KB+512KB map
ESP_FLASH_MODE      ?= 2       # DIO
ESP_FLASH_FREQ_DIV  ?= 0       # 40 MHz

else
# Winbond 25Q32 4MB flash, typ for esp-12
# Here we're using two partitions of approx 0.5MB because that's what's easily available in terms
# of linker scripts in the SDK. Ideally we'd use two partitions of approx 1MB, the remaining 2MB
# cannot be used for code (esp8266 limitation).
ESP_SPI_SIZE        ?= 4       # 6->4MB (1MB+1MB) or 4->4MB (512KB+512KB)
ESP_FLASH_MODE      ?= 2       # DIO
ESP_FLASH_FREQ_DIV  ?= 0       # 40 MHz
endif

# Output directors to store intermediate compiled files
# relative to the project directory
BUILD_BASE	= build
FW_BASE		= firmware

# name for the target project
TARGET		= esp-serial2wifi

# espressif tool to concatenate sections for OTA upload using bootloader v1.2+

CFLAGS=

# set defines for optional modules
ifneq (,$(findstring mqtt,$(MODULES)))
	CFLAGS		+= -DMQTT
endif

ifneq (,$(findstring rest,$(MODULES)))
	CFLAGS		+= -DREST
endif

ifneq (,$(findstring syslog,$(MODULES)))
	CFLAGS		+= -DSYSLOG
endif

ifneq (,$(findstring web-server,$(MODULES)))
	CFLAGS		+= -DWEBSERVER
endif

ifneq (,$(findstring socket,$(MODULES)))
	CFLAGS		+= -DSOCKET
endif

# which modules (subdirectories) of the project to include in compiling
override MODULES += espfs httpd serial cmd core
EXTRA_INCDIR 	= include . build

# libraries used in this project, mainly provided by the SDK
LIBS = c gcc hal phy pp net80211 wpa main lwip_536 crypto driver

# compiler flags using during compilation of source files
CFLAGS	+= -MMD -MP -Os -ggdb -std=c99 -Werror -Wpointer-arith -Wundef -Wall -Wl,-EL -fno-inline-functions \
	-nostdlib -mlongcalls -mtext-section-literals -ffunction-sections -fdata-sections \
	-D__ets__ -DICACHE_FLASH -Wno-address -DFIRMWARE_SIZE=$(ESP_FLASH_MAX) \
	-DMCU_RESET_PIN=$(MCU_RESET_PIN) -DMCU_ISP_PIN=$(MCU_ISP_PIN) \
	-DLED_CONN_PIN=$(LED_CONN_PIN) -DLED_SERIAL_PIN=$(LED_SERIAL_PIN) \
	-DSERIAL2WIFI_DEBUG=$(SERIAL2WIFI_DEBUG) -DSERIAL2WIFI_OTA_ENABLED=$(SERIAL2WIFI_OTA_ENABLED) -DSERIAL2WIFI_OTA_TESTING=$(SERIAL2WIFI_OTA_TESTING) \
	-DVERSION="esp-serial2wifi $(VERSION)"

# linker flags used to generate the main object file
LDFLAGS		= -nostdlib -Wl,--no-check-sections -u call_user_start1 -Wl,-static -Wl,--gc-sections

# linker script used for the above linker step
LD_SCRIPT1	:= build/app1.ld
LD_SCRIPT2	:= build/app2.ld

# various paths from the SDK used in this project
SDK_LIBDIR	= lib
SDK_LDDIR	= ld
SDK_INCDIR	= include include/json third_party/include

# select which tools to use as compiler, librarian and linker
CC		:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-gcc
AR		:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-ar
LD		:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-gcc
OBJCP		:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-objcopy
OBJDP		:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-objdump
ELF_SIZE	:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-size

####
SRC_DIR		:= $(addprefix src/,$(MODULES))
BUILD_DIR	:= $(addprefix $(BUILD_BASE)/,$(SRC_DIR))

SDK_LIBDIR	:= $(addprefix $(SDK_BASE)/,$(SDK_LIBDIR))
SDK_LDDIR 	:= $(addprefix $(SDK_BASE)/,$(SDK_LDDIR))
SDK_INCDIR	:= $(addprefix -I$(SDK_BASE)/,$(SDK_INCDIR))

SRC		:= $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.c))
OBJ		:= $(patsubst %.c,$(BUILD_BASE)/%.o,$(SRC)) $(BUILD_BASE)/espfs_img.o
LIBS		:= $(addprefix -l,$(LIBS))
STARTUP_OBJ := $(BUILD_BASE)/src/core/startup.o
APP_AR		:= $(addprefix $(BUILD_BASE)/,$(TARGET)_app.a)
SDK_TRANSPORT_LIB := $(BUILD_BASE)/sdk-transport/liblwip_536.a
USER1_OUT 	:= $(addprefix $(BUILD_BASE)/,$(TARGET).user1.elf)
USER2_OUT 	:= $(addprefix $(BUILD_BASE)/,$(TARGET).user2.elf)

INCDIR		:= $(addprefix -I,$(SRC_DIR))
EXTRA_INCDIR	:= $(addprefix -I,$(EXTRA_INCDIR))

V ?= $(VERBOSE)
ifeq ("$(V)","1")
Q :=
vecho := @true
else
Q := @
vecho := @echo
endif

ifneq ($(CHIP_IN_HOSTNAME),0)
CFLAGS += -DCHIP_IN_HOSTNAME
endif

ifeq ("$(GZIP_COMPRESSION)","yes")
CFLAGS		+= -DGZIP_COMPRESSION
endif

ifeq ("$(CHANGE_TO_STA)","yes")
CFLAGS		+= -DCHANGE_TO_STA
endif

export SERIAL2WIFI_BUILD_CFLAGS := $(CFLAGS)
UI_INPUTS := $(wildcard web/app/*.html web/app/*.css web/app/*.js)
HTML_INPUTS := $(shell find web/custom-pages -type f) $(UI_INPUTS) tools/build/build_ui.py tools/build/optimize_assets.mjs $(wildcard third_party/xterm-*/*) package-lock.json requirements-build.txt
BUILD_CONFIG := build/build-config.json

$(BUILD_BASE)/%.o: %.c
	$(Q) mkdir -p $(dir $@)
	$(vecho) "CC $<"
	$(Q) $(CC) $(INCDIR) $(EXTRA_INCDIR) $(SDK_INCDIR) $(CFLAGS) -c $< -o $@

.PHONY: all checkdirs clean wiflash flash factory-flash release

LAYOUT_HEADER := build/generated_flash_layout.h
FIRMWARE_IMAGES := $(FW_BASE)/user1.bin
ifneq ($(FLASH_SIZE),512KB)
FIRMWARE_IMAGES += $(FW_BASE)/user2.bin
endif
all: check-inputs checkdirs $(FIRMWARE_IMAGES)
	$(if $(filter 1,$(SERIAL2WIFI_EXTERNAL_UI)),$(Q) $(PYTHON) tools/lib/ui_package.py firmware --ui build/ui.espfs,$(Q) rm -f $(FW_BASE)/update-user1.elu $(FW_BASE)/update-user2.elu $(FW_BASE)/ui.espfs)
	$(Q) $(PYTHON) tools/lib/flash_layout.py $(FLASH_SIZE) $(UI_LAYOUT_ARG) $(foreach image,$(FIRMWARE_IMAGES),--image $(image)) > build/flash-layout.json
	$(Q) $(PYTHON) tools/build/write_manifest.py $(FLASH_SIZE) --sdk "$(SDK_BASE)" --version "$(VERSION)" --debug $(SERIAL2WIFI_DEBUG) --ota-enabled $(SERIAL2WIFI_OTA_ENABLED) --ota-testing $(SERIAL2WIFI_OTA_TESTING) $(UI_LAYOUT_ARG) --modules "$(MODULES)" --mode $(ESP_FLASH_MODE) --frequency $(ESP_FLASH_FREQ_DIV)

.PHONY: check-inputs
check-inputs:
	@test -x "$(PYTHON)" || { echo "Run make setup first (or set PYTHON to the prepared environment)."; exit 1; }
	$(Q) $(PYTHON) tools/build/fetch_sdk.py --offline --verify-only --base "$(abspath $(SDK_BASE)/..)" --sdk "$(SDK_BASE)" --compiler-dir "$(XTENSA_TOOLS_ROOT)"

$(OBJ): | check-inputs

$(LAYOUT_HEADER): tools/lib/flash_layout.py FORCE
	$(Q) $(PYTHON) tools/lib/flash_layout.py $(FLASH_SIZE) $(UI_LAYOUT_ARG) --header $@

$(BUILD_CONFIG): tools/build/write_config.py FORCE
	$(Q) $(PYTHON) tools/build/write_config.py $@ --sdk "$(SDK_BASE)" --toolchain "$(XTENSA_TOOLS_ROOT)" --flash "$(FLASH_SIZE)" --modules "$(MODULES)" --version "$(VERSION)" --debug "$(SERIAL2WIFI_DEBUG)" --ota "$(SERIAL2WIFI_OTA_ENABLED)/$(SERIAL2WIFI_OTA_TESTING)" --compression "$(SERIAL2WIFI_EXTERNAL_UI)/$(GZIP_COMPRESSION)/$(ESP_SPI_SIZE)/$(ESP_FLASH_MODE)/$(ESP_FLASH_FREQ_DIV)"

$(OBJ): Makefile mk/firmware.mk $(LAYOUT_HEADER) $(BUILD_CONFIG) build/ui_identity.h

build/ui_identity.h build/espfs.img $(if $(filter 1,$(SERIAL2WIFI_EXTERNAL_UI)),build/ui.espfs) &: $(HTML_INPUTS) $(BUILD_CONFIG)
	$(Q) $(PYTHON) tools/build/build_ui.py firmware $(UI_LAYOUT_ARG) $(if $(filter yes,$(GZIP_COMPRESSION)),,--no-gzip)
.PHONY: FORCE
FORCE:

$(SDK_TRANSPORT_LIB): tools/build/prepare_sdk.py sdk-lock.json $(SDK_LIBDIR)/liblwip_536.a | check-inputs
	$(Q) $(PYTHON) tools/build/prepare_sdk.py --sdk "$(SDK_BASE)" --ar "$(AR)" --output "$@"

$(USER1_OUT): $(APP_AR) $(STARTUP_OBJ) $(LD_SCRIPT1) $(SDK_TRANSPORT_LIB)
	$(vecho) "LD $@"
	$(Q) $(LD) -L$(dir $(SDK_TRANSPORT_LIB)) -L$(SDK_LIBDIR) -T$(LD_SCRIPT1) $(LDFLAGS) $(STARTUP_OBJ) -Wl,--start-group $(LIBS) $(APP_AR) -Wl,--end-group -o $@
	@echo Dump  : $(OBJDP) -x $(USER1_OUT)
	@echo Disass: $(OBJDP) -d -l -x $(USER1_OUT)
#	$(Q) $(OBJDP) -x $(TARGET_OUT) | egrep espfs_img

$(USER2_OUT): $(APP_AR) $(STARTUP_OBJ) $(LD_SCRIPT2) $(SDK_TRANSPORT_LIB)
	$(vecho) "LD $@"
	$(Q) $(LD) -L$(dir $(SDK_TRANSPORT_LIB)) -L$(SDK_LIBDIR) -T$(LD_SCRIPT2) $(LDFLAGS) $(STARTUP_OBJ) -Wl,--start-group $(LIBS) $(APP_AR) -Wl,--end-group -o $@
#	$(Q) $(OBJDP) -x $(TARGET_OUT) | egrep espfs_img

$(FW_BASE):
	$(vecho) "FW $@"
	$(Q) mkdir -p $@

$(FW_BASE)/user1.bin: $(USER1_OUT) tools/build/build_image.py tools/build/generate_appbin.py | $(FW_BASE)
	$(Q) $(PYTHON) tools/build/build_image.py $< $@ --slot 0 --toolchain "$(XTENSA_TOOLS_ROOT)" --mode $(ESP_FLASH_MODE) --frequency $(ESP_FLASH_FREQ_DIV) --map $(ESP_SPI_SIZE) --limit $(ESP_FLASH_MAX)

$(FW_BASE)/user2.bin: $(USER2_OUT) tools/build/build_image.py tools/build/generate_appbin.py | $(FW_BASE)
	$(Q) $(PYTHON) tools/build/build_image.py $< $@ --slot 1 --toolchain "$(XTENSA_TOOLS_ROOT)" --mode $(ESP_FLASH_MODE) --frequency $(ESP_FLASH_FREQ_DIV) --map $(ESP_SPI_SIZE) --limit $(ESP_FLASH_MAX)

$(APP_AR): $(filter-out $(STARTUP_OBJ),$(OBJ))
	$(vecho) "AR $@"
	$(Q) $(RM) $@
	$(Q) $(AR) cru $@ $^

checkdirs: $(BUILD_DIR)

$(BUILD_DIR):
	$(Q) mkdir -p $@

wiflash: all
	$(Q) $(PYTHON) tools/flash_ota.py $(ESP_HOSTNAME) $(if $(filter 1,$(SERIAL2WIFI_EXTERNAL_UI)),$(FW_BASE)/update-user1.elu $(FW_BASE)/update-user2.elu,$(FW_BASE)/user1.bin $(FW_BASE)/user2.bin)

# Never select a connected device implicitly.
flash: all
	$(Q) $(PYTHON) tools/flash_serial.py --port "$(PORT)" --slot $(SERIAL_SLOT) --baud $(ESPBAUD) --sdk "$(SDK_BASE)"

factory-flash: all
	$(Q) $(PYTHON) tools/flash_serial.py --port "$(PORT)" --slot 1 --baud $(ESPBAUD) --sdk "$(SDK_BASE)" --factory

# Ship the replacement UI and only the compatibility assets used by user pages.
$(BUILD_BASE)/espfs_img.o: build/ui_identity.h build/espfs.img
	$(Q) ls -sl build/espfs.img
	$(Q) cd build; $(OBJCP) -I binary -O elf32-xtensa-le -B xtensa --rename-section .data=.espfs \
	  espfs.img espfs_img.o; cd ..

# edit the loader script to add the espfs section to the end of irom with a 4 byte alignment.
# we also adjust the sizes of the segments 'cause we need more irom0
build/app1.ld: $(SDK_LDDIR)/eagle.app.v6.new.1024.app1.ld
	$(Q) sed -e '/\.irom\.text/{' -e 'a . = ALIGN (4);' -e 'a *(.espfs)' -e '}'  \
		-e '/^  irom0_0_seg/ s/6B000/7C000/' \
		$(SDK_LDDIR)/eagle.app.v6.new.1024.app1.ld >$@
build/app2.ld: $(SDK_LDDIR)/eagle.app.v6.new.1024.app2.ld
	$(Q) sed -e '/\.irom\.text/{' -e 'a . = ALIGN (4);' -e 'a *(.espfs)' -e '}'  \
		-e '/^  irom0_0_seg/ s/6B000/7C000/' \
		$(SDK_LDDIR)/eagle.app.v6.new.1024.app2.ld >$@

release: all
	$(Q) $(PYTHON) tools/build/package_release.py --sdk "$(SDK_BASE)"

clean:
	$(Q) rm -rf build firmware

-include $(OBJ:.o=.d)

.PHONY: check check-experimental
check: all
	$(Q) $(PYTHON) tests/run.py host --flash "$(FLASH_SIZE)" --sdk "$(SDK_BASE)" --ar "$(AR)"

check-experimental: all
	@test "$(SERIAL2WIFI_EXTERNAL_UI)" = 1 || { echo 'Use make check-experimental SERIAL2WIFI_EXTERNAL_UI=1'; exit 1; }
	$(Q) $(PYTHON) tests/run.py experimental
