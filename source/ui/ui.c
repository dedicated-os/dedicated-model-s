#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <tinyalsa/asoundlib.h>
#include <unistd.h>

#include "drv_display.h"	// https://github.com/linux-sunxi/libvdpau-sunxi/blob/master/kernel-headers/drv_display.h
#include "ion.h" 			// https://github.com/armbian/linux/blob/sun8i/include/linux/ion.h
#include "ion_sunxi.h"		// https://github.com/armbian/linux/blob/sun8i/include/linux/ion_sunxi.h
#include "libretro.h"

int main(void) {
	return 0;
}