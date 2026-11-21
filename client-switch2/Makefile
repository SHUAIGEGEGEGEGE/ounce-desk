#---------------------------------------------------------------------------------
# OunceDesk — Nintendo Switch 2 (kaiHEN) Native Windows RDP (3389) + macOS VNC (5900)
# Built with devkitPro + ouncebrew/ounce-devkit (libounce + OunceVK)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libounce/switch2_rules

TARGET		:= OunceDesk
BUILD		:= build
SOURCES		:= source
DATA		:= data
INCLUDES	:= source
ICON		:= icon.jpg

APP_TITLE	:= OunceDesk
APP_AUTHOR	:= OunceDesk
APP_VERSION	:= 1.0.0

ARCH		:= -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS		:= -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS		+= $(INCLUDE) -D__SWITCH__ -DOUNCEDESK_HAS_TTF
CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

PORTLIBS	?= $(DEVKITPRO)/portlibs/switch
SWITCH_SPECS	:= $(firstword $(wildcard $(DEVKITPRO)/libounce/switch2.specs $(DEVKITPRO)/libounce/switch.specs $(DEVKITPRO)/libnx/switch.specs))

ASFLAGS		:= -g $(ARCH)
LDFLAGS		= $(if $(SWITCH_SPECS),-specs=$(SWITCH_SPECS)) -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# Official devkitPro Switch portlibs (compatible unchanged with libounce + OunceVK):
#   switch-sdl2, switch-sdl2_ttf, switch-mbedtls, switch-zlib
LIBS		:= -lSDL2_ttf -lfreetype -lbz2 -lpng -lSDL2 \
			   -lmbedtls -lmbedx509 -lmbedcrypto \
			   -lz -lEGL -lnx -lm

LIBDIRS		:= $(DEVKITPRO)/portlibs/switch2 $(PORTLIBS) $(DEVKITPRO)/libounce

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:= $(CURDIR)/$(TARGET)
export TOPDIR	:= $(CURDIR)
export VPATH	:= $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
				   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:= $(CURDIR)/$(BUILD)

CFILES		:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD	:= $(CXX)
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_SOURCES)

export INCLUDE	:= $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
				   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
				   -I$(PORTLIBS)/include/SDL2 \
				   -I$(CURDIR)/$(BUILD)

export LIBPATHS	:= $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

.PHONY: all

DEPENDS	:= $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
