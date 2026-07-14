ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE, please run from the union-trimui-toolchain docker image)
endif

TARGET=ui.elf
OUTPUT=build/$(TARGET)

ARCH = -marm -mtune=arm926ej-s -march=armv5te
CC 	= $(CROSS_COMPILE)gcc
SRC = ui.c
CFLAGS = -Ofast $(ARCH) -flto -Wall -ffunction-sections -fdata-sections -Wall
CFLAGS += -I./libretro-common/include/
LDFLAGS = -ldl -lm -lSDL -lSDL_image -lSDL_ttf -ltinyalsa -Wl,--gc-sections -s

OBJ = $(addprefix build/,$(SRC:.c=.o))

all: $(OUTPUT)
	
build/%.o: %.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(OUTPUT): libretro-common $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

clean:
	rm -rf build
	
libretro-common:
	git clone https://github.com/libretro/libretro-common