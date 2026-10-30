# Rack SDK 位置：默认使用项目内的 Rack-SDK，也可以 make RACK_DIR=/path/to/Rack-SDK
RACK_DIR ?= Rack-SDK

FLAGS +=
CFLAGS +=
CXXFLAGS +=
LDFLAGS +=

# 这台机器的 CommandLineTools 里有一个残缺的 usr/include/c++/v1，会遮住 SDK 里完整的 libc++ 头文件，
# 这里显式改用 SDK 自带的 libc++ 头文件。
ifeq ($(shell uname -s),Darwin)
	MAC_SDK := $(shell xcrun --show-sdk-path 2>/dev/null)
	ifneq ($(wildcard $(MAC_SDK)/usr/include/c++/v1/string),)
		CXXFLAGS += -nostdinc++ -isystem $(MAC_SDK)/usr/include/c++/v1
	endif
endif

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/plugin.mk
