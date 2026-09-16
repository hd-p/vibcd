# Baby monitor for RV1106 (Luckfox SDK)
#
# The SDK include tree is at $(SDK_PATH)/media/out, not media/output. That typo
# made an earlier version of this Makefile fail on the very first header.

SDK_PATH ?= /media/demon/hdd1/arepos/linux/SDK/rv1106/rv1106_SDK_luckfox
RK_MEDIA_OUTPUT ?= $(SDK_PATH)/media/out
TOOLCHAIN_DIR ?= $(SDK_PATH)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf

CROSS_COMPILE ?= $(TOOLCHAIN_DIR)/bin/arm-rockchip830-linux-uclibcgnueabihf-
CXX := $(CROSS_COMPILE)g++

SRC_DIR := src
BUILD_DIR := build
BIN_DIR := bin
TARGET := $(BIN_DIR)/baby_monitor

INCLUDES := -I$(SRC_DIR)
INCLUDES += -I$(RK_MEDIA_OUTPUT)/include
INCLUDES += -I$(RK_MEDIA_OUTPUT)/include/rkaiq
INCLUDES += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/uAPI2
INCLUDES += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/common
# The rkaiq headers include each other by bare filename, so every directory
# holding a referenced header has to be on the search path. Reached in this
# order from rk_aiq_user_api2_sysctl.h: xcore for "base/xcam_common.h", algos
# for "adebayer/...", then iq_parser and iq_parser_v2 for "RkAiqCalibDbTypes.h".
INCLUDES += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/xcore
INCLUDES += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/algos
INCLUDES += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/iq_parser
INCLUDES += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/iq_parser_v2

# c++17 is required, not a preference: std::atomic<>::is_always_lock_free and
# inline static data members are both C++17, and the earlier -std=c++11 setting
# could not compile either.
# CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -
CXXFLAGS := -std=c++17 -Wall -Wextra -g
CXXFLAGS += -DRV1106 -DISP_HW_V30
CXXFLAGS += $(INCLUDES)

LDFLAGS := -L$(RK_MEDIA_OUTPUT)/lib
LDFLAGS += -Wl,-rpath-link,$(RK_MEDIA_OUTPUT)/lib

# librtsp is a static archive, so it has to follow the objects that reference it.
LDLIBS := -lrockit -lrockchip_mpp -lrtsp -lrkaiq -lpthread -lrt -ldl

SOURCES := \
	$(SRC_DIR)/main.cpp \
	$(SRC_DIR)/base/child_process.cpp \
	$(SRC_DIR)/base/robust_mutex.cpp \
	$(SRC_DIR)/media/isp_controller.cpp \
	$(SRC_DIR)/media/media_pipeline.cpp \
	$(SRC_DIR)/audio/audio_pipeline.cpp \
	$(SRC_DIR)/app/media_service.cpp \
	$(SRC_DIR)/app/audio_service.cpp \
	$(SRC_DIR)/app/supervisor.cpp \
	$(SRC_DIR)/app/hardware_watchdog.cpp

OBJECTS := $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
DEPENDENCIES := $(OBJECTS:.o=.d)

.PHONY: all clean install help

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@
	@echo "Built $@"

# -MMD -MP emit header dependencies so editing a header rebuilds its users.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPENDENCIES)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

install: $(TARGET)
	@if [ -z "$(INSTALL_DIR)" ]; then \
		echo "Set INSTALL_DIR, e.g. make install INSTALL_DIR=/tmp/rootfs/usr/bin"; \
		exit 1; \
	fi
	@mkdir -p $(INSTALL_DIR)
	cp $(TARGET) $(INSTALL_DIR)/

help:
	@echo "Targets: all, clean, install, help"
	@echo ""
	@echo "  SDK_PATH     path to the Luckfox SDK"
	@echo "               (current: $(SDK_PATH))"
	@echo "  INSTALL_DIR  destination for 'make install'"
	@echo ""
	@echo "Deploy over adb:"
	@echo "  make"
	@echo "  adb push $(TARGET) /root/"
	@echo "  adb shell chmod +x /root/baby_monitor"
