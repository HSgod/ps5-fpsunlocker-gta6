# Build the PS5 payload (ps5-payload-sdk)
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make                             -> "GTA6 60FPS_v<version>.elf"
#   make send PS5_HOST=192.168.x.x   -> push to elfldr (port 9021)
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifeq ($(PS5_PAYLOAD_SDK),)
    ifneq ($(MAKECMDGOALS),clean)
        $(error PS5_PAYLOAD_SDK is undefined)
    endif
else
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

TOOL_VERSION := $(shell sed -n 's/^\#define TOOL_VERSION "\(.*\)"/\1/p' gta6-60fps.c)
ELF := GTA6 60FPS_v$(TOOL_VERSION).elf

CFLAGS := -Wall -Wextra -Werror -Os

all: gta6

gta6:
	$(CC) $(CFLAGS) -o "$(ELF)" gta6-60fps.c ptwalk.c

send: gta6
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) "$(ELF)"

clean:
	rm -f "$(ELF)" gta6-60fps.elf

.PHONY: all gta6 send clean
