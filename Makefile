# Define paths.
PWD := $(shell pwd)
MISC_PATH := $(PWD)/misc
MK_PATH := $(MISC_PATH)/mk
SCRIPTS_PATH := $(MISC_PATH)/scripts
SPECS_PATH := $(MISC_PATH)/specs
NPDM_JSON_PATH := $(MISC_PATH)/npdm-json

# To configure exlaunch, edit config.mk.
include $(PWD)/config.mk

# Define common variables.
NAME := $(shell basename $(PWD))
OUT := $(PWD)/out-dev
SD_OUT := atmosphere/contents/$(PROGRAM_ID)/exefs

# Native gcc/ld create temp files while linking, and they fall back to
# C:\WINDOWS (unwritable) when TMP/TEMP are missing or empty.  Some shells and
# sandboxes hand the build an empty TMP, which used to fail the link step with
# "Cannot create temporary file in C:\WINDOWS".  Point it at a directory inside
# the project so the build does not depend on the caller's environment.
# cygpath -m gives E:/... which native binaries understand; a POSIX /e/... path
# does not.
SDV_TMP := $(shell cygpath -m "$(CURDIR)/.tmp/build" 2>/dev/null || echo $(CURDIR)/.tmp/build)
export TMPDIR := $(SDV_TMP)
export TMP    := $(SDV_TMP)
export TEMP   := $(SDV_TMP)

# Both configurations link to ./sdv_native.elf, and make cannot tell them
# apart: after `make`, that elf is newer than every build-release object, so
# `make RELEASE=1` would skip the link and hand back the dev binary -- and the
# other way round, which is worse, because a dev build that is really a release
# build still boots and just silently loses the diagnostics.  Drop it at parse
# time so each build links from its own objects.  Cost is one relink.
$(shell rm -f "$(CURDIR)/$(notdir $(CURDIR)).elf")


# Set load kind specific variables.
ifeq ($(LOAD_KIND), Module)
    LOAD_KIND_ENUM := 2
    BINARY_NAME := subsdk8 # subsdk9 is already taken at runtime (see docs)
    SPECS_NAME := module.specs
    MK_NAME := module.mk
else ifeq ($(LOAD_KIND), AsRtld)
    LOAD_KIND_ENUM := 1
    BINARY_NAME := rtld
    SPECS_NAME := as_rtld.specs
    MK_NAME := as_rtld.mk
else
    $(error LOAD_KIND is invalid, please check config.mk)
endif

.PHONY: clean all

# Built internal C flags variable.
EXL_CFLAGS   := $(C_FLAGS) -DEXL_LOAD_KIND=$(LOAD_KIND) -DEXL_LOAD_KIND_ENUM=$(LOAD_KIND_ENUM) -DEXL_PROGRAM_ID=0x$(PROGRAM_ID)
EXL_CXXFLAGS := $(CXX_FLAGS)

# Release build: same source, one define apart.
#
# `make` gives the local build, with the on-screen diagnostics on, which is what
# gets flashed while debugging a feature.  `make RELEASE=1` compiles them out
# (SDV_RELEASE in main.cpp) and is the only thing that may be packaged for
# users.  The object and output directories differ on purpose: make does not
# know that the flags changed, so a shared build/ would happily link stale
# objects and hand back a "release" binary with the diagnostics still in it.
ifeq ($(RELEASE),1)
    SDV_RELEASE  := 1
    BUILD        := build-release
    OUT          := $(PWD)/out-release
    EXL_CFLAGS   += -DSDV_RELEASE
    EXL_CXXFLAGS += -DSDV_RELEASE
endif

# Export all of our variables to sub-makes and sub-processes.
export

include $(MK_PATH)/$(MK_NAME)
include $(MK_PATH)/common.mk
include $(MK_PATH)/deploy.mk
include $(MK_PATH)/npdm.mk
