-include local.mk

RACK_DIR ?= ../../Rack-SDK
export RACK_DIR

SOURCES += $(wildcard src/*.cpp)
SOURCES += $(wildcard src/*.c)
DISTRIBUTABLES += res
DISTRIBUTABLES += scl
DISTRIBUTABLES += $(wildcard LICENSE*)

.PHONY: photos
PHOTO_ZOOM ?= 2

photos:
	PHOTO_ZOOM="$(PHOTO_ZOOM)" bash photos/app/render.sh

include $(RACK_DIR)/plugin.mk
