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

#define CEIL_TO(x, n) (((x) + (n) - 1) / (n) * (n))
#define NUMBER_OF(items) (sizeof(items) / sizeof((items)[0]))
#define ms SDL_GetTicks

// --------------------------------------------
// logging
// --------------------------------------------

void LOG_init(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
}
#define LOG(...) printf(__VA_ARGS__); printf("\n"); fflush(stdout)

// --------------------------------------------
// support
// --------------------------------------------

static inline void touch(const char *path) {
	int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
	if (fd >= 0) close(fd);
}
static inline int getInt(int f) {
	if (f<0) return 0;

	char b[32];
	int n = pread(f, b, sizeof(b) - 1, 0);
	if (n<=0) return 0;

	b[n] = '\0';
	return atoi(b);
}
static inline void setInt(int f, int value) {
    char buffer[16];
    int len = sprintf(buffer, "%d\n", value);
	lseek(f, 0, SEEK_SET);
    write(f, buffer, len);
}
static inline void getString(void) {
	
}
static inline void putString(const char* path, const char* value) {
	int f = open(path, O_WRONLY);
	if (f<0) return;
	write(f, value, strlen(value));
	close(f);
}
static inline void putInt(const char* path, int value) {
	char buffer[16];
	sprintf(buffer, "%d", value);
	putString(path, buffer);
}
static inline int exists(const char* path) {
	return access(path, F_OK)==0;
}

// --------------------------------------------
// raw HMI controls
// --------------------------------------------

static struct {
	int bri;
	struct mixer *mix;
	struct mixer_ctl *ctl;
} raw;

static void raw_init(void) {
	// lcd
	int fd = open("/dev/mem", O_RDWR);
	int len = 0x1000;
	uint32_t* mem = (uint32_t*)mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01c20000); // CCU/INTC/PIO/TIMER
	uint32_t pe_cfg0 = mem[0x0890>>2]; // PE_CFG0
	if (pe_cfg0 & 1) mem[0x0890>>2] = (pe_cfg0 & 0xF0FFFFFF) | 0x03000000;
	munmap(mem, 0x1000);
	close(fd);
	
	raw.bri = open("/sys/class/disp/disp/attr/lcdbl", O_WRONLY | O_CLOEXEC);
	raw.mix = mixer_open(0);
	raw.ctl = mixer_get_ctl(raw.mix, 22);
}
static void raw_quit(void) {
	close(raw.bri);
	mixer_close(raw.mix);
}

static void raw_vol(int value) { // 0-100 
	mixer_ctl_set_percent(raw.ctl,0,value);
}
static void raw_bri(int value) { // 70-120 
	setInt(raw.bri, value);
}

// --------------------------------------------

static int PWR_getBatteryLevel(void) {
	// returns the average of the last 10 readings
	#define MAX_VALUES 10
	static int values[MAX_VALUES];
	static int total;
	static int i = 0;
	static int ready = 0;
	
	// get the current value
	int value = -1;
	FILE* file = fopen("/sys/devices/soc/1c23400.battery/adc", "r");
	if (file!=NULL) {
		fscanf(file, "%i", &value);
		fclose(file);
	}
	
	// first run, fill up the buffer
	if (!ready) {
		for (int i=0; i<MAX_VALUES; i++) {
			values[i] = value;
		}
		total = value * MAX_VALUES;
		ready = 1;
	}
	// subsequent calls, update average
	else {
		total -= values[i];
		values[i] = value;
		total += value;
		i += 1;
		if (i>=MAX_VALUES) i -= MAX_VALUES;
		value = total / MAX_VALUES;
	}
	return value;
}
static int PWR_getBatteryPercent(void) {
	int charge = PWR_getBatteryLevel();
	if (charge<41) return  0;
	if (charge<43) return 25;
	if (charge<44) return 50;
	if (charge<46) return 75;
	return 100;
}
static int PWR_isConnected(void) {
	// TODO: this is unreliable, don't use
    char state[32] = {0};
    FILE *f = fopen("/sys/class/android_usb/android0/state", "r");
    if (!f) return false;
    fgets(state, sizeof(state), f);
    fclose(f);
    return strstr(state, "CONFIGURED") || strstr(state, "CONNECTED");
}

// --------------------------------------------

#define CPU_SPEED_SLEEP		0x00000112 // 16MHz
#define CPU_SPEED_MIN		0x00c00532 // 192MHz
#define CPU_SPEED_DEFAULT	0x02d01d22 // 720MHz
#define CPU_SPEED_MAX		0x03601a32 // 864MHz

static void CPU_setSpeed(uint32_t mhz) {
	volatile uint32_t* mem;
	volatile uint8_t memdev = 0;
	memdev = open("/dev/mem", O_RDWR);
	if (memdev>0) {
		mem = (uint32_t*)mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, memdev, 0x01c20000);
		if (mem==MAP_FAILED) {
			LOG("Could not mmap CPU hardware registers!");
			close(memdev);
			return;
		}
	}
	else LOG("Could not open /dev/mem");
	
	uint32_t v = mem[0];
	v &= 0xffff0000;
	v |= (mhz & 0x0000ffff);
	mem[0] = v;
	
	if (memdev>0) close(memdev);
}

// --------------------------------------------

#define SDCARD_PATH		"/mnt/SDCARD"
#define BIOS_PATH		SDCARD_PATH "/bios"
#define GAMES_PATH 		SDCARD_PATH "/games"
#define ARCHIVE_PATH	GAMES_PATH "/archive"
#define SYSTEM_PATH 	SDCARD_PATH "/system"
#define ASSETS_PATH		SYSTEM_PATH "/assets"
#define USERDATA_PATH	SDCARD_PATH "/userdata"

#define MAX_LINE 1024
#define MAX_PATH 512
#define MAX_FILE 256

// --------------------------------------------

#define BLACK_TRIAD 	0x00,0x00,0x00
#define WHITE_TRIAD 	0xff,0xff,0xff
#define LIGHT_TRIAD 	0x99,0x99,0x99
#define DARK_TRIAD 		0x33,0x33,0x33
#define MID_TRIAD 		0x44,0x44,0x44
#define RED_TRIAD		0xff,0x33,0x33
#define GREEN_TRIAD		0x33,0xcc,0x33
#define YELLOW_TRIAD	0xff,0xcc,0x00

#define TRIAD_ALPHA(t,a) (SDL_Color){t,a}

#define BLACK_COLOR		TRIAD_ALPHA(BLACK_TRIAD,0xff)
#define WHITE_COLOR		TRIAD_ALPHA(WHITE_TRIAD,0xff)
#define LIGHT_COLOR		TRIAD_ALPHA(LIGHT_TRIAD,0xff)
#define DARK_COLOR		TRIAD_ALPHA(DARK_TRIAD,0xff)
#define RED_COLOR		TRIAD_ALPHA(RED_TRIAD,0xff)
#define GREEN_COLOR		TRIAD_ALPHA(GREEN_TRIAD,0xff)
#define YELLOW_COLOR	TRIAD_ALPHA(YELLOW_TRIAD,0xff)

// --------------------------------------------
// settings
// --------------------------------------------

static struct {
	int version;
	int volume;		// 0-20
	int brightness;	// 0 - 10
	int unused[5];
	char game[MAX_PATH];
} settings = {
	.version = 1,
	.volume = 10,
	.brightness = 4,
};

#define SETTINGS_PATH USERDATA_PATH "/settings.bin"
static void Settings_load(void) {
	raw_vol(0);
	
	if (access(SETTINGS_PATH, F_OK)!=0) return;
	
	FILE* file = fopen(SETTINGS_PATH, "rb");
	if (!file) return;
	fread(&settings, sizeof(settings), 1, file);
	fclose(file);
}
static void Settings_save(void) {
	FILE* file = fopen(SETTINGS_PATH, "wb");
	if (!file) return;
	fwrite(&settings, sizeof(settings), 1, file);
	fclose(file);
}
static void Settings_setVolume(int value) {
	raw_vol(value * 5);
	settings.volume = value;
}
static void Settings_setBrightness(int value) {
	raw_bri(70 + (value * 5));
	settings.brightness = value;
}

// --------------------------------------------
// IPU (courtesy of eggs)
// --------------------------------------------

#define	VIDEO_WIDTH		(320)
#define	VIDEO_HEIGHT	(240)

static int	SCALER_WIDTH	= VIDEO_WIDTH; // (240)
static int	SCALER_HEIGHT	= VIDEO_HEIGHT; // (180)

#define	SCALER_X 	((VIDEO_WIDTH - SCALER_WIDTH)/2)
#define	SCALER_Y 	((VIDEO_HEIGHT - SCALER_HEIGHT)/2)
#define	SCALER_W 	(SCALER_WIDTH)
#define	SCALER_H 	(SCALER_HEIGHT)

#define	ION_WRITE_CACHE	// probably faster but needs flush

#define DEFE			(0x01E00000)
#define DEFE_SIZE		(0x1000)
#define	BUF_ADDR0_REG	(0x20)
#define DEBE			(0x01E60000)
#define DEBE_SIZE		(0x1000)
#define	LAY1_ADDR_REG_L	(0x854)
#define	LAY1_ADDR_REG_H	(0x861)
#define	TCON			(0x01C0C000)
#define	TIMING_REG1		(0x4C)
#define	TIMING_REG2		(0x50)
#define	DEBUG_INFO_REG	(0xFC)
#define	LAYER0			(0)
#define	LAYER1			(1)
#define	LAYER2			(2)
#define	LAYER3			(3)

typedef struct ion_alloc_info {
	uint32_t			size;
	struct ion_handle*	handle;
	int					fd;
	void*				padd;
	void*				vadd;
} ion_alloc_info_t;

//	global variables
int					disp_fd, fb_fd, ion_fd, mem_fd;
ion_alloc_info_t	sc_meminfo, ov_meminfo;
disp_layer_info		fb_info, sc_info, ov_info;
SDL_Surface			*scaler, *overlay;
uint32_t			*debe_map, *defe_map, sleepns_mul;
uint16_t			vbp, vt;

uint32_t 			sc_flip_offset = UINT32_MAX;
uint32_t 			ov_flip_offset = UINT32_MAX;
int					sc_dirty = 0;
int					ov_dirty = 0;
float 				scale = 1.0;

#define NEW_IOCTL	// Model S seems to be NEW_IOCTL

static void ion_alloc(ion_alloc_info_t* info) {
	struct ion_allocation_data	iad;
	struct ion_fd_data			ifd;
	struct ion_custom_data		icd;
	sunxi_phys_data				spd;

	iad.len = info->size;
	iad.align = sysconf(_SC_PAGESIZE);
	iad.heap_id_mask = ION_HEAP_TYPE_DMA_MASK;
#ifdef	ION_WRITE_CACHE
	iad.flags = ION_FLAG_CACHED;
#else
	iad.flags = ION_FLAG_CACHED_NEEDS_SYNC;
#endif
	if (ioctl(ion_fd, ION_IOC_ALLOC, &iad)<0) fprintf(stderr, "ION_ALLOC failed %s\n",strerror(errno));
	icd.cmd = ION_IOC_SUNXI_PHYS_ADDR;
	icd.arg = (uintptr_t)&spd;
	spd.handle = iad.handle;
	if (ioctl(ion_fd, ION_IOC_CUSTOM, &icd)<0) fprintf(stderr, "ION_GET_PHY failed %s\n",strerror(errno));
	ifd.handle = iad.handle;
	if (ioctl(ion_fd, ION_IOC_MAP, &ifd)<0) fprintf(stderr, "ION_MAP failed %s\n",strerror(errno));

	info->handle = iad.handle;
	info->fd = ifd.fd;
	info->padd = (void*)spd.phys_addr;
	info->vadd = mmap(0, info->size, PROT_READ|PROT_WRITE, MAP_SHARED, info->fd, 0);
	fprintf(stderr, "allocated padd: 0x%x vadd: 0x%x size: 0x%x\n", (uintptr_t)info->padd, (uintptr_t)info->vadd, info->size);
}
static void ion_free(ion_alloc_info_t* info) {
	struct ion_handle_data	 ihd;

	munmap(info->vadd, info->size);
	ihd.handle = info->handle;
	if (ioctl(ion_fd, ION_IOC_FREE, &ihd)<0) fprintf(stderr, "ION_FREE failed %s\n",strerror(errno));
}
static void ion_flush(void* ofs, uint32_t len) {
#ifdef	ION_WRITE_CACHE
	sunxi_cache_range	range;
	range.start = (uintptr_t)ofs;
	range.end = (uintptr_t)ofs + len;
#ifdef	NEW_IOCTL
	if (ioctl(ion_fd, ION_IOC_SUNXI_FLUSH_RANGE, &range)<0) fprintf(stderr, "ION_FLUSH_RANGE failed %s\n",strerror(errno));
#else
	struct ion_custom_data	icd;
	icd.cmd = ION_IOC_SUNXI_FLUSH_RANGE;
	icd.arg = (uintptr_t)&range;
	if (ioctl(ion_fd, ION_IOC_CUSTOM, &icd)<0) fprintf(stderr, "ION_FLUSH_RANGE failed %s\n",strerror(errno));
#endif
#endif
}

// Model S cannot use VSYNC_EVENT_EN / WAITFORVSYNC, so wait on its own based on TCON info
static void setup_vsyncwait_eggs(void) {
	uint16_t	*tcon_map, ht;

	tcon_map = (uint16_t*)mmap(0, TIMING_REG2+4, PROT_READ, MAP_PRIVATE, mem_fd, TCON);
	ht = (tcon_map[(TIMING_REG1+2)/2])+1;
	vt = (tcon_map[(TIMING_REG2+2)/2])/2;
	vbp = (tcon_map[(TIMING_REG2)/2])+1;
	munmap(tcon_map,TIMING_REG2+4);
	sleepns_mul = 1000000000 / (70000 / ht) / vt;
	
	fprintf(stderr, "ht: %i vt: %i vbp: %i sleepns_mul: %i\n", ht,vt,vbp,sleepns_mul);
}
static void setup_vsyncwait(void) { // ChatGPT mod for setting TCON ht/vt/vbp
	uint16_t *tcon_map;
	uint16_t ht;

	// Use read/write access, and MAP_SHARED so writes go through
	tcon_map = (uint16_t*)mmap(0, TIMING_REG2+4, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, TCON);
	if (tcon_map == MAP_FAILED) {
		perror("mmap");
		return;
	}

	// TIMING_REG2 layout:
	// offset 0: vbp (vertical back porch)
	// offset 2: unknown
	// offset 4: vt (vertical total), stored as raw = vt * 2

	// 1125/286/6 == default 61.5fps
	// 1122/294/6 == perfect 60fps
	
	ht = 1122;
	vt = 294; // 60
	// vt = 295; // 59.8
	// vt = 296; // 59.6

	// --- OVERRIDE values here before they're read ---
	// Set horizontal total (ht = 1125) stored as raw = ht - 1
	tcon_map[(TIMING_REG1 + 2)/2] = ht - 1;

	// Set vertical total (vt = 286), stored as raw = vt * 2
	tcon_map[(TIMING_REG2+2)/2] = vt * 2;

	// Set vertical back porch (vbp = 6), stored as raw = vbp - 1
	// tcon_map[(TIMING_REG2+0)/2] = 6 - 1;

	// Now read values
	ht  = (tcon_map[(TIMING_REG1+2)/2]) + 1;
	vt  = (tcon_map[(TIMING_REG2+2)/2]) / 2;
	vbp = (tcon_map[(TIMING_REG2+0)/2]) + 1;

	munmap(tcon_map, TIMING_REG2+4);

	sleepns_mul = 1000000000 / (70000 / ht) / vt;

	fprintf(stderr, "ht: %i vt: %i vbp: %i sleepns_mul: %i\n", ht, vt, vbp, sleepns_mul);
}
static void waitforvsync(void) {
	struct timespec	sleeptime = {0,0};
	uint16_t	*tcon_map, line;

	while(1) {
		tcon_map = (uint16_t*)mmap(0, DEBUG_INFO_REG+4, PROT_READ, MAP_PRIVATE, mem_fd, TCON);
		line = (tcon_map[(DEBUG_INFO_REG+2)/2]) & 0x3ff;
		munmap(tcon_map, DEBUG_INFO_REG+4);
		if ((line<vbp)||(line>=240+vbp)) break;
		sleeptime.tv_nsec = (240+vbp-line) * sleepns_mul;
		nanosleep(&sleeptime, NULL);
	}
}
static void waitfordisplayperiod(void) {
	struct timespec	sleeptime = {0,0};
	uint16_t	*tcon_map, line;

	while(1) {
		tcon_map = (uint16_t*)mmap(0, DEBUG_INFO_REG+4, PROT_READ, MAP_PRIVATE, mem_fd, TCON);
		line = (tcon_map[(DEBUG_INFO_REG+2)/2]) & 0x3ff;
		munmap(tcon_map, DEBUG_INFO_REG+4);
		if ((line>=vbp)&&(line<240+vbp)) break;
		sleeptime.tv_nsec = ((line<vbp)?(vbp-line):(vt-line+vbp)) * sleepns_mul;
		nanosleep(&sleeptime, NULL);
	}
}


static void dirty_scaler(void) {
	sc_dirty = 1;
}
static void dirty_overlay(void) {
	ov_dirty = 1;
}

// vsync: 0 = no wait, 1 = vsync wait, 2 = vsync & display period wait
enum {
	VSYNC_NONE = 0,
	VSYNC_WAIT,
	VSYNC_BLOCK,
	
	VSYNC_COUNT,
};
static void present_layers(int vsync) {
	// if (!sc_dirty && !ov_dirty) return;
	
	if (sc_dirty) {
		ion_flush(scaler->pixels, scaler->pitch*scaler->h);
	}
	if (ov_dirty) {
		ion_flush(overlay->pixels, overlay->pitch*overlay->h);
	}
	
	if (vsync) waitforvsync();
	
	if (sc_dirty) {
		sc_info.fb.addr[0] = (uintptr_t)sc_meminfo.padd + sc_flip_offset;
		defe_map[BUF_ADDR0_REG/4] = sc_info.fb.addr[0];
		sc_flip_offset ^= scaler->pitch*scaler->h;
		scaler->pixels = (void*)((uintptr_t)sc_meminfo.vadd + sc_flip_offset);
		sc_dirty = 0;
	}
	if (ov_dirty) {
		ov_info.fb.addr[0] = (uintptr_t)ov_meminfo.padd + ov_flip_offset;
		uint32_t args[4] = {0, LAYER2, (uintptr_t)&ov_info, 0};
		if (ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args)<0) fprintf(stderr, "LAYER_SET_INFO failed %s\n",strerror(errno));
		ov_flip_offset ^= overlay->pitch*overlay->h;
		overlay->pixels = (void*)((uintptr_t)ov_meminfo.vadd + ov_flip_offset);
		ov_dirty = 0;
	}
	
	if (vsync>1) waitfordisplayperiod();
}

static void setup_layers(void) {
	// move FB from layer3 to layer0 (lowest priority)
	uint32_t args[4] = {0, LAYER3, (uintptr_t)&fb_info, 0};
	if (ioctl(disp_fd, DISP_CMD_LAYER_GET_INFO, &args)<0) fprintf(stderr, "LAYER_GET_INFO failed %s\n",strerror(errno));
	if (ioctl(disp_fd, DISP_CMD_LAYER_DISABLE, &args)<0) fprintf(stderr, "LAYER_DISABLE failed %s\n",strerror(errno));
	
	args[1] = LAYER0;
	fb_info.zorder = 0; // z (not work, just in case)
	if (ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args)<0) fprintf(stderr, "LAYER_SET_INFO failed %s\n",strerror(errno));
	if (ioctl(disp_fd, DISP_CMD_LAYER_ENABLE, &args)<0) fprintf(stderr, "LAYER_ENABLE failed %s\n",strerror(errno));

	// allocate memory for scaler/overlay layers
	sc_meminfo.size = ((VIDEO_WIDTH*VIDEO_HEIGHT*4)*2 + 4095) & (~4095); // doublebuf
	ov_meminfo.size = ((VIDEO_WIDTH*VIDEO_HEIGHT*4)*2 + 4095) & (~4095); // doublebuf
	ion_alloc(&sc_meminfo);
	ion_alloc(&ov_meminfo);
	memset(ov_meminfo.vadd, 0, ov_meminfo.size);
	ion_flush(ov_meminfo.vadd, ov_meminfo.size);
	
	scaler = SDL_CreateRGBSurfaceFrom(sc_meminfo.vadd + SCALER_WIDTH*SCALER_HEIGHT*4, SCALER_WIDTH, SCALER_HEIGHT,
			32, SCALER_WIDTH*4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	overlay = SDL_CreateRGBSurfaceFrom(ov_meminfo.vadd + VIDEO_WIDTH*VIDEO_HEIGHT*4, VIDEO_WIDTH, VIDEO_HEIGHT,
			32, VIDEO_WIDTH*4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	
	// setup layer for scaler: use layer 1
	memset(&sc_info, 0, sizeof(sc_info));
	sc_info.mode = DISP_LAYER_WORK_MODE_SCALER;
	sc_info.pipe = 1;								// pipe
	sc_info.zorder = 1;								// z (not work)
	sc_info.alpha_mode = 0;							// alpha
	sc_info.alpha_value = 0;						// alpha(val)
	sc_info.ck_enable = 0;							// colorkey
	sc_info.screen_win.x = SCALER_X;				// frame.x
	sc_info.screen_win.y = SCALER_Y;				// frame.y
	sc_info.screen_win.width = SCALER_W;			// frame.w stretch 1.5x 192 > 288
	sc_info.screen_win.height = SCALER_H;			// frame.h stretch 1.5x 144 > 216
	sc_info.fb.addr[0] = (uintptr_t)sc_meminfo.padd;// address[0]
	sc_info.fb.size.width = SCALER_WIDTH;			// framebuffer.w
	sc_info.fb.size.height = SCALER_HEIGHT;			// framebuffer.h
	sc_info.fb.format = DISP_FORMAT_ARGB_8888;		// fmt
	sc_info.fb.pre_multiply = 0;					// pre_mult
	sc_info.fb.src_win.x = 0;						// source crop.x
	sc_info.fb.src_win.y = 0;						// source crop.y
	sc_info.fb.src_win.width = SCALER_WIDTH;		// source crop.w
	sc_info.fb.src_win.height = SCALER_HEIGHT;		// source crop.h
	
	args[1] = LAYER1; args[2] = (uintptr_t)&sc_info;
	if (ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args)<0) fprintf(stderr, "LAYER_SET_INFO failed %s\n",strerror(errno));

	// setup layer for overlay: use layer 2
	memset(&ov_info, 0, sizeof(ov_info));
	ov_info.mode = DISP_LAYER_WORK_MODE_NORMAL;
	ov_info.pipe = 0;								// pipe
	ov_info.zorder = 2;								// z (not work)
	ov_info.alpha_mode = 0;							// alpha
	ov_info.alpha_value = 0;						// alpha(val)
	ov_info.ck_enable = 0;							// colorkey
	ov_info.screen_win.x = 0;						// frame.x
	ov_info.screen_win.y = 0;						// frame.y
	ov_info.screen_win.width = VIDEO_WIDTH;			// frame.w
	ov_info.screen_win.height = VIDEO_HEIGHT;		// frame.h
	ov_info.fb.addr[0] = (uintptr_t)ov_meminfo.padd;// address[0]
	ov_info.fb.size.width = VIDEO_WIDTH;			// framebuffer.w
	ov_info.fb.size.height = VIDEO_HEIGHT;			// framebuffer.h
	ov_info.fb.format = DISP_FORMAT_ARGB_8888;		// fmt
	ov_info.fb.pre_multiply = 0;					// pre_mult
	ov_info.fb.src_win.x = 0;						// source crop.x
	ov_info.fb.src_win.y = 0;						// source crop.y
	ov_info.fb.src_win.width = VIDEO_WIDTH;			// source crop.w
	ov_info.fb.src_win.height = VIDEO_HEIGHT;		// source crop.h
	
	args[1] = LAYER2; args[2] = (uintptr_t)&ov_info;
	if (ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args)<0) fprintf(stderr, "LAYER_SET_INFO failed %s\n",strerror(errno));
	
	ov_flip_offset = overlay->pitch*overlay->h;
}
static void enable_layers(void) {
	uint32_t args[4] = {0, LAYER1, 0, 0};
	if (ioctl(disp_fd, DISP_CMD_LAYER_ENABLE, &args)<0) fprintf(stderr, "LAYER_ENABLE failed %s\n",strerror(errno));
	args[1] = LAYER2;
	if (ioctl(disp_fd, DISP_CMD_LAYER_ENABLE, &args)<0) fprintf(stderr, "LAYER_ENABLE failed %s\n",strerror(errno));
}
static void disable_layers(void) {
	uint32_t args[4] = {0, LAYER1, 0, 0};
	if (ioctl(disp_fd, DISP_CMD_LAYER_DISABLE, &args)<0) fprintf(stderr, "LAYER_DISABLE failed %s\n",strerror(errno));
	args[1] = LAYER2;
	if (ioctl(disp_fd, DISP_CMD_LAYER_DISABLE, &args)<0) fprintf(stderr, "LAYER_DISABLE failed %s\n",strerror(errno));
}
static void enable_overlay(void) {
	uint32_t args[4] = {0, LAYER2, 0, 0};
	ioctl(disp_fd, DISP_CMD_LAYER_ENABLE, &args);
}
static void disable_overlay(void) {
	uint32_t args[4] = {0, LAYER2, 0, 0};
	ioctl(disp_fd, DISP_CMD_LAYER_DISABLE, &args);
}

static void free_layers(void) {
	// disable and free memory for scaler/overlay layers
	disable_layers();
	ion_free(&ov_meminfo);
	ion_free(&sc_meminfo);
	// move FB from layer0 to layer3
	uint32_t args[4] = {0, LAYER0, (uintptr_t)&fb_info, 0};
	if (ioctl(disp_fd, DISP_CMD_LAYER_DISABLE, &args)<0) fprintf(stderr, "LAYER_DISABLE failed %s\n",strerror(errno));
	args[1] = LAYER3;
	fb_info.zorder = 3; // z (not work, just in case)
	if (ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args)<0) fprintf(stderr, "LAYER_SET_INFO failed %s\n",strerror(errno));
	if (ioctl(disp_fd, DISP_CMD_LAYER_ENABLE, &args)<0) fprintf(stderr, "LAYER_ENABLE failed %s\n",strerror(errno));
}

static void DE_init(void) {
	disp_fd = open("/dev/disp",O_RDWR);
	fb_fd = open("/dev/fb0",O_RDWR);
	ion_fd = open("/dev/ion", O_RDWR);
	mem_fd = open("/dev/mem",O_RDWR);
	defe_map = (uint32_t*)mmap(0, DEFE_SIZE, PROT_WRITE, MAP_SHARED, mem_fd, DEFE);
	debe_map = (uint32_t*)mmap(0, DEBE_SIZE, PROT_WRITE, MAP_SHARED, mem_fd, DEBE);
	setup_vsyncwait();
	setup_layers();
}
static void DE_quit(void) {
	free_layers();
	munmap(defe_map, DEFE_SIZE);
	munmap(debe_map, DEBE_SIZE);
	close(mem_fd);
	close(ion_fd);
	close(fb_fd);
	close(disp_fd);
}

static void resize_scaler(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	uint32_t args[4] = {0, LAYER1, (uintptr_t)&sc_info, 0};
	sc_info.screen_win.x = x; 		// source crop.x
	sc_info.screen_win.y = y; 		// source crop.y
	sc_info.screen_win.width  = w;	// source crop.w
	sc_info.screen_win.height = h;	// source crop.h
	ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args);
}

enum {
	SCALE_NONE = 0,
	SCALE_ASPECT,
	// SCALE_FULL,
	
	SCALE_COUNT,
};
static int scale_mode = SCALE_ASPECT;

static inline int round_to_nearest(int value, int n) {
    return (value + n / 2) / n * n;
}
static void reinit_layer(int w, int h) {
	// TODO: w,h must be <= VIDEO_*
	
	// TODO: don't resize scaler just resize scale?
	
	if (w!=SCALER_WIDTH || h!=SCALER_HEIGHT) {
		memset(sc_meminfo.vadd, 0, sc_meminfo.size);
		ion_flush(sc_meminfo.vadd, sc_meminfo.size);
	}
	
	SCALER_WIDTH = w;
	SCALER_HEIGHT = h;
	SDL_FreeSurface(scaler);
	scaler = SDL_CreateRGBSurfaceFrom(sc_meminfo.vadd + SCALER_WIDTH*SCALER_HEIGHT*4, SCALER_WIDTH, SCALER_HEIGHT,
			32, SCALER_WIDTH*4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	
	uint32_t args[4] = {0, LAYER1, (uintptr_t)&sc_info, 0};
	sc_info.fb.size.width = SCALER_WIDTH;		// framebuffer.w
	sc_info.fb.size.height = SCALER_HEIGHT;		// framebuffer.h
	sc_info.fb.src_win.x = 0;					// source crop.x
	sc_info.fb.src_win.y = 0;					// source crop.y
	sc_info.fb.src_win.width = SCALER_WIDTH;	// source crop.w
	sc_info.fb.src_win.height = SCALER_HEIGHT;	// source crop.h
	if (ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args)<0) fprintf(stderr, "LAYER_SET_INFO failed %s\n",strerror(errno));

	sc_flip_offset = scaler->pitch*scaler->h;
	
	float scale_x = (float)VIDEO_WIDTH / SCALER_WIDTH;
	float scale_y = (float)VIDEO_HEIGHT / SCALER_HEIGHT;
	scale = scale_x < scale_y ? scale_x : scale_y;
	
	if (scale_mode==SCALE_NONE) {
		scale = (int)scale; // integer
	}
	// scale = (float)((int)(scale*2))/2; // nearest 50% scale
	fprintf(stderr,"reinit_layer %ix%i (%0.2f%%)\n",w,h,scale); fflush(stderr);
	
	int sw = round_to_nearest((int)(SCALER_WIDTH * scale + 0.5f), 2);
    int sh = round_to_nearest((int)(SCALER_HEIGHT * scale + 0.5f), 2);
	
	// if (scale_mode==SCALE_FULL) {
	// 	sw = VIDEO_WIDTH;
	// 	sh = VIDEO_HEIGHT;
	// }

    int ox = round_to_nearest((VIDEO_WIDTH - sw) / 2, 2);
    int oy = round_to_nearest((VIDEO_HEIGHT - sh) / 2, 2);
	
	resize_scaler(ox,oy,sw,sh);
	// resize_scaler(0,0,VIDEO_WIDTH,VIDEO_HEIGHT); // fullscreen
}

// --------------------------------------------
// custom fonts
// --------------------------------------------

typedef struct {
	uint8_t map[128];
	uint8_t char_widths[128];
	int8_t kern_pairs[128][128];

	SDL_Surface* bitmap;
	const char* name;
	const char* charset;
	int8_t offset_x;
	int8_t offset_y;
	int8_t tracking;
	uint8_t tile_width;
	uint8_t tile_height;
	uint8_t tiles_wide;
	uint8_t tiles_high;
	uint8_t missing;

	uint8_t char_width;
	uint8_t low_width;
} Font;

static Font* font18 = &(Font){
	.name = "font-dedicated-18.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%,: ",
	.tile_width = 18,
	.tile_height = 18,
	.offset_x = -1,
	.tracking = 3,
	.char_width = 15,
	.char_widths = {
		['I'] = 3,
		['1'] = 9,
		['.'] = 3,
		[','] = 3,
		['-'] = 12,
		['\''] = 3,
		['!'] = 3,
		[':'] = 3,
		[' '] = 6,
	},
	.kern_pairs = {
		['A']['T'] = -3,
		['T']['A'] = -3,
		['A']['V'] = -3,
		['V']['A'] = -3,
		['A']['W'] = -1,
		['W']['A'] = -1,
		['T']['-'] = -3,
		['-']['T'] = -3,
	},
};

static Font* font12 = &(Font){
	.name = "font-dedicated-12.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%,: ",
	.tile_width = 12,
	.tile_height = 12,
	.tracking = 2,
	.char_width = 10,
	.char_widths = {
		['I'] = 2,
		['1'] = 8,
		['.'] = 2,
		[','] = 2,
		['-'] = 8,
		['\''] = 2,
		['!'] = 2,
		[':'] = 2,
		[' '] = 4,
	},
	.kern_pairs = {
		['A']['T'] = -1,
		['T']['A'] = -1,
		['A']['V'] = -1,
		['V']['A'] = -1,
		['A']['W'] = -1,
		['W']['A'] = -1,
		['T']['-'] = -1,
		['-']['T'] = -1,
	},
};

static void Font_init(Font* font) {
	char path[MAX_FILE];
	sprintf(path, "%s/%s", ASSETS_PATH, font->name);
	font->bitmap = IMG_Load(path);
	
	font->tiles_wide = font->bitmap->w / font->tile_width;
	font->tiles_high = font->bitmap->h / font->tile_height;
	font->missing = (font->tiles_wide * font->tiles_high) - 1;
	
	memset(font->map, font->missing, sizeof(font->map));
	for (uint8_t i=0; font->charset[i]; i++) {
		unsigned char c = font->charset[i];
		font->map[c] = (uint8_t)i;
		if (!font->char_widths[c]) {
			if (font->low_width && c>='a' && c<='z') {
				font->char_widths[c] = font->low_width;
			}
			else {
				font->char_widths[c] = font->char_width;
			}
		}
	}
}
static void Font_quit(Font* font) {
	SDL_FreeSurface(font->bitmap);
}

static void __Font_blitChar(Font* font, SDL_Surface* dst, unsigned char c, int x, int y) {
	int i = c < 128 ? font->map[c] : font->missing;
	int tx = i % font->tiles_wide;
	int ty = i / font->tiles_wide;
	int tw = font->tile_width;
	int th = font->tile_height;

	SDL_Surface* src = font->bitmap;

	int sx = tx * tw;
	int sy = ty * th;
	int dx = x;
	int dy = y;

	if (dx < 0) {
		sx -= dx;
		tw += dx;
		dx = 0;
	}
	if (dy < 0) {
		sy -= dy;
		th += dy;
		dy = 0;
	}
	if (dx + tw > dst->w) tw = dst->w - dx;
	if (dy + th > dst->h) th = dst->h - dy;

	if (tw <= 0 || th <= 0) return;

	uint32_t* src_pixels = (uint32_t*)src->pixels;
	uint32_t* dst_pixels = (uint32_t*)dst->pixels;

	int src_pitch = src->pitch / 4;
	int dst_pitch = dst->pitch / 4;

	for (int row = 0; row < th; row++) {
		uint32_t* src_row = src_pixels + (sy + row) * src_pitch + sx;
		uint32_t* dst_row = dst_pixels + (dy + row) * dst_pitch + dx;

		for (int col = 0; col < tw; col++) {
			uint32_t px = src_row[col];
			if (px & 0xFF000000) {
				dst_row[col] = px;
			}
		}
	}
}

static void __Font_layoutText(Font* font, const char* text, int* out_width, int* out_height, SDL_Surface** out_surface) {
	SDL_Surface* dst = NULL;
	if (out_surface) {
		int w = 0;
		int h = 0;
		__Font_layoutText(font, text, &w, &h, NULL);
		dst = SDL_CreateRGBSurface(SDL_SWSURFACE, w,h, 32, 0x00FF0000,0x0000FF00,0x000000FF,0xFF000000);
		memset(dst->pixels, 0, dst->pitch * dst->h);
	}
	
	const char* tmp = text;
	int ow = 0;
	int oh = font->tile_height;
	while (*tmp) {
		unsigned char c = *tmp++;
		unsigned char n = *tmp;
		if (!font->char_widths[c]) c = toupper(c);
		if (n && !font->char_widths[n]) n = toupper(n);
		
		if (out_surface) __Font_blitChar(font, dst, c, ow,0);

		ow += font->char_widths[c] ? font->char_widths[c] : font->char_width;
		if (n) {
			ow += font->tracking;
			if (font->kern_pairs[c][n]) {
				ow += font->kern_pairs[c][n];
			}
			else {
				ow += font->kern_pairs[0][n];
			}
		}
	}
	ow -= font->offset_x * 2; // reverse pad?
	
	if (out_width) *out_width = ow;
	if (out_height) *out_height = oh;
	if (out_surface) *out_surface = dst;
}

static void Font_getTextSize(Font* font, const char* text, int* out_width, int* out_height) {
	__Font_layoutText(font, text, out_width, out_height, NULL);
}

static SDL_Surface* Font_drawText(Font* font, const char* text) {
	SDL_Surface* surface = NULL;
	__Font_layoutText(font, text, NULL, NULL, &surface);
	return surface;
}

typedef void (*Font_renderFunc)(SDL_Surface* dst, Font* font, const char* text, int x, int y, SDL_Color color);

static void __Font_blitText(SDL_Surface *src, SDL_Surface* dst, int dx, int dy, SDL_Color color) {
	uint32_t argb = 0xFF000000 | ((uint32_t)color.r << 16) | ((uint32_t)color.g << 8) | color.b;

	int sx = 0;
	int sy = 0;
	int w = src->w;
	int h = src->h;

	if (dx < 0) {
		sx -= dx;
		w += dx;
		dx = 0;
	}
	if (dy < 0) {
		sy -= dy;
		h += dy;
		dy = 0;
	}
	if (dx + w > dst->w) w = dst->w - dx;
	if (dy + h > dst->h) h = dst->h - dy;
	if (w <= 0 || h <= 0) return;

	uint32_t* src_pixels = (uint32_t*)src->pixels;
	uint32_t* dst_pixels = (uint32_t*)dst->pixels;

	int src_pitch = src->pitch / 4;
	int dst_pitch = dst->pitch / 4;

	for (int row = 0; row < h; row++) {
		uint32_t* src_row = src_pixels + (sy + row) * src_pitch + sx;
		uint32_t* dst_row = dst_pixels + (dy + row) * dst_pitch + dx;

		for (int col = 0; col < w; col++) {
			if (src_row[col] & 0xFF000000) {
				dst_row[col] = argb;
			}
		}
	}
}

static void __Font_renderText(SDL_Surface* dst, Font* font, const char* text, int x, int y, SDL_Color color, int s) {	
	x += font->offset_x;
	y += font->offset_y;

	SDL_Surface* surface = Font_drawText(font, text);

	if (s) __Font_blitText(surface, dst, x+1,y+1, BLACK_COLOR);
	__Font_blitText(surface, dst, x,y, color);

	SDL_FreeSurface(surface);
}

static void Font_renderText(SDL_Surface* dst, Font* font, const char* text, int x, int y, SDL_Color color) {
	__Font_renderText(dst, font, text, x,y, color, 0);
}
static void Font_shadowText(SDL_Surface* dst, Font* font, const char* text, int x, int y, SDL_Color color) {
	__Font_renderText(dst, font, text, x,y, color, 1);
}

// --------------------------------------------

static void Fonts_init(void) {
	Font_init(font18);
	Font_init(font12);
}
static void Fonts_quit(void) {
	Font_quit(font18);
	Font_quit(font12);
}

// --------------------------------------------

typedef enum {
	PAD_ID_NONE = -1,
	PAD_ID_UP,
	PAD_ID_DOWN,
	PAD_ID_LEFT,
	PAD_ID_RIGHT,
	PAD_ID_A,
	PAD_ID_B,
	PAD_ID_X,
	PAD_ID_Y,
	PAD_ID_START,
	PAD_ID_SELECT,
	PAD_ID_L1,
	PAD_ID_R1,
	PAD_ID_MENU,

	PAD_ID_COUNT,
} PadButtonId;
typedef enum {
	PAD_NONE	= 0,
	PAD_UP 		= 1u << PAD_ID_UP,
	PAD_DOWN	= 1u << PAD_ID_DOWN,
	PAD_LEFT	= 1u << PAD_ID_LEFT,
	PAD_RIGHT	= 1u << PAD_ID_RIGHT,
	PAD_A		= 1u << PAD_ID_A,
	PAD_B		= 1u << PAD_ID_B,
	PAD_X		= 1u << PAD_ID_X,
	PAD_Y		= 1u << PAD_ID_Y,
	PAD_START	= 1u << PAD_ID_START,
	PAD_SELECT	= 1u << PAD_ID_SELECT,
	PAD_L1		= 1u << PAD_ID_L1,
	PAD_R1		= 1u << PAD_ID_R1,
	PAD_MENU	= 1u << PAD_ID_MENU,
} PadButton;

#define KEYSYM_UP		SDLK_UP
#define KEYSYM_RIGHT	SDLK_RIGHT
#define KEYSYM_DOWN		SDLK_DOWN
#define KEYSYM_LEFT		SDLK_LEFT
#define KEYSYM_A		SDLK_SPACE
#define KEYSYM_B		SDLK_LCTRL
#define KEYSYM_X		SDLK_LSHIFT
#define KEYSYM_Y		SDLK_LALT
#define KEYSYM_L		SDLK_TAB
#define KEYSYM_R		SDLK_BACKSPACE
#define KEYSYM_SELECT	SDLK_RCTRL
#define KEYSYM_START	SDLK_RETURN
#define KEYSYM_MENU		SDLK_ESCAPE

#define PAD_REPEAT_DELAY	300
#define PAD_REPEAT_INTERVAL 100
static struct {
	uint32_t is_pressed;
	uint32_t just_pressed;
	uint32_t just_released;
	uint32_t just_repeated;
	uint32_t repeat_at[PAD_ID_COUNT];
} pad;

static void Pad_reset(void) {
	pad.just_pressed = PAD_NONE;
	pad.is_pressed = PAD_NONE;
	pad.just_released = PAD_NONE;
	pad.just_repeated = PAD_NONE;
}
static void Pad_consume(PadButton btn) {
	// pad.is_pressed is intentionally omitted
	pad.just_pressed  &= ~btn;
	pad.just_released &= ~btn;
	pad.just_repeated &= ~btn;
}
static void Pad_update(void) {
	// reset transient state
	pad.just_pressed = PAD_NONE;
	pad.just_released = PAD_NONE;
	pad.just_repeated = PAD_NONE;
	
	// update repeat states
	uint32_t tick = ms();
	for (int i=0; i<PAD_ID_COUNT; i++) {
		int btn = 1 << i;
		if ((pad.is_pressed & btn) && (tick>=pad.repeat_at[i])) {
			pad.just_repeated |= btn; // set
			pad.repeat_at[i] += PAD_REPEAT_INTERVAL;
		}
	}
	
	// the actual poll
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		int btn = PAD_NONE;
		int pressed = 0;
		int id = -1;
		if (event.type==SDL_KEYDOWN || event.type==SDL_KEYUP) {
			SDLKey key = event.key.keysym.sym;
			pressed = event.type==SDL_KEYDOWN;
				 if (key==KEYSYM_UP) 		{ btn = PAD_UP; 	id = PAD_ID_UP; }
 			else if (key==KEYSYM_DOWN)		{ btn = PAD_DOWN; 	id = PAD_ID_DOWN; }
			else if (key==KEYSYM_LEFT)		{ btn = PAD_LEFT; 	id = PAD_ID_LEFT; }
			else if (key==KEYSYM_RIGHT)		{ btn = PAD_RIGHT; 	id = PAD_ID_RIGHT; }
			else if (key==KEYSYM_A)			{ btn = PAD_A; 		id = PAD_ID_A; }
			else if (key==KEYSYM_B)			{ btn = PAD_B; 		id = PAD_ID_B; }
			else if (key==KEYSYM_X)			{ btn = PAD_X; 		id = PAD_ID_X; }
			else if (key==KEYSYM_Y)			{ btn = PAD_Y; 		id = PAD_ID_Y; }
			else if (key==KEYSYM_START)		{ btn = PAD_START; 	id = PAD_ID_START; }
			else if (key==KEYSYM_SELECT)	{ btn = PAD_SELECT; id = PAD_ID_SELECT; }
			else if (key==KEYSYM_L)			{ btn = PAD_L1; 	id = PAD_ID_L1; }
			else if (key==KEYSYM_R)			{ btn = PAD_R1; 	id = PAD_ID_R1; }
			else if (key==KEYSYM_MENU)		{ btn = PAD_MENU; 	id = PAD_ID_MENU; }
		}
		
		if (btn==PAD_NONE) continue;
	
		if (!pressed) {
			pad.is_pressed		&= ~btn; // unset
			pad.just_repeated	&= ~btn; // unset
			pad.just_released	|= btn; // set
		}
		else if ((pad.is_pressed & btn)==PAD_NONE) {
			pad.just_pressed	|= btn; // set
			pad.just_repeated	|= btn; // set
			pad.is_pressed		|= btn; // set
			pad.repeat_at[id]	= tick + PAD_REPEAT_DELAY;
		}
	}
}

static inline int Pad_anyJustPressed(void)	{ return pad.just_pressed!=PAD_NONE; }
static inline int Pad_anyPressed(void)		{ return pad.is_pressed!=PAD_NONE; }
static inline int Pad_anyJustReleased(void)	{ return pad.just_released!=PAD_NONE; }

static inline int Pad_justPressed(PadButton btn)	{ return pad.just_pressed & btn; }
static inline int Pad_isPressed(PadButton btn)	{ return pad.is_pressed & btn; }
static inline int Pad_justReleased(PadButton btn)	{ return pad.just_released & btn; }
static inline int Pad_justRepeated(PadButton btn)	{ return pad.just_repeated & btn; }

// --------------------------------------------

static void UI_gradient(SDL_Surface *dst) {
	// quantized 8x8 Bayer matrix
	static const int8_t dither[8][8] = {
		{ -4,  2, -3,  3, -4,  2, -3,  3 },
		{  0, -2,  1, -1,  0, -2,  1, -1 },
		{ -3,  3, -4,  2, -3,  3, -4,  2 },
		{  1, -1,  0, -2,  1, -1,  0, -2 },
		{ -4,  2, -3,  3, -4,  2, -3,  3 },
		{  0, -2,  1, -1,  0, -2,  1, -1 },
		{ -3,  3, -4,  2, -3,  3, -4,  2 },
		{  1, -1,  0, -2,  1, -1,  0, -2 },
	};

	uint32_t* pixels = dst->pixels;
	int w = dst->w;
	int h = dst->h;
	for (int y = 0; y < h; y++) {
		int base = 64 + ((h - y) * 160 / h);
		for (int x = 0; x < w; x++, pixels++) {
			*pixels = (uint32_t)(base + dither[y & 7][x & 7]) << 24;
		}
	}
}
static void UI_fillRect(SDL_Surface* dst, int x, int y, int w, int h, SDL_Color c) {
	if (w <= 0 || h <= 0) return;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > dst->w) w = dst->w - x;
	if (y + h > dst->h) h = dst->h - y;

	if (w <= 0 || h <= 0) return;

	uint32_t color = 0xFF000000 |
		((uint32_t)c.r << 16) |
		((uint32_t)c.g << 8) |
		(uint32_t)c.b;

	uint32_t* pixels = (uint32_t*)dst->pixels;
	int pitch = dst->pitch / 4;
	uint32_t* row0 = pixels + y * pitch + x;

	if (w < 16) {
		for (int row = 0; row < h; row++) {
			uint32_t* d = row0 + row * pitch;
			for (int col = 0; col < w; col++) {
				d[col] = color;
			}
		}
	}
	else {
		for (int col = 0; col < w; col++) {
			row0[col] = color;
		}

		size_t row_bytes = (size_t)w * sizeof(uint32_t);
		for (int row = 1; row < h; row++) {
			memcpy(row0 + row * pitch, row0, row_bytes);
		}
	}
}
static void UI_rect(SDL_Surface* dst, int x, int y, int w, int h, int s, SDL_Color c) {
	if (w <= 0 || h <= 0) return;

	int x0 = x;
	int y0 = y;
	int x1 = x + w;
	int y1 = y + h;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > dst->w) x1 = dst->w;
	if (y1 > dst->h) y1 = dst->h;
	if (x0 >= x1 || y0 >= y1) return;

	uint32_t color = 0xFF000000 |
		((uint32_t)c.r << 16) |
		((uint32_t)c.g << 8) |
		(uint32_t)c.b;

	uint32_t* pixels = (uint32_t*)dst->pixels;
	int pitch = dst->pitch / 4;
	
	// fill
	if (s <= 0) {
		for (int yy = y0; yy < y1; yy++) {
			int lx = x0 - x;
			int rx = x1 - x;

			if (yy == y || yy == y + h - 1) {
				if (lx == 0) lx = 1;
				if (rx == w) rx = w - 1;
			}

			uint32_t* d = pixels + yy * pitch + x + lx;
			for (int xx = lx; xx < rx; xx++) *d++ = color;
		}
		return;
	}
	
	// outline
	if (s > w / 2) s = w / 2;
	if (s > h / 2) s = h / 2;

	for (int yy = y0; yy < y1; yy++) {
		int ly = yy - y;
		int top_or_bottom = ly < s || ly >= h - s;
		int corner_row = ly == 0 || ly == h - 1;

		int lx0 = x0 - x;
		int lx1 = x1 - x;

		if (top_or_bottom) {
			if (corner_row) {
				if (lx0 == 0) lx0 = 1;
				if (lx1 == w) lx1 = w - 1;
			}

			uint32_t* d = pixels + yy * pitch + x + lx0;
			for (int xx = lx0; xx < lx1; xx++) *d++ = color;
		}
		else {
			int left0 = lx0;
			int left1 = lx1 < s ? lx1 : s;
			int right0 = lx0 > w - s ? lx0 : w - s;
			int right1 = lx1;

			uint32_t* d = pixels + yy * pitch + x + left0;
			for (int xx = left0; xx < left1; xx++) *d++ = color;

			d = pixels + yy * pitch + x + right0;
			for (int xx = right0; xx < right1; xx++) *d++ = color;
		}
	}
}
static void UI_bolt(SDL_Surface* dst, int x, int y, SDL_Color c) {
	UI_fillRect(dst, x+3,y+0, 4,2, c);
	UI_fillRect(dst, x+2,y+2, 4,2, c);
	UI_fillRect(dst, x+1,y+4, 9,2, c);
	UI_fillRect(dst, x+0,y+6, 9,2, c);
	UI_fillRect(dst, x+4,y+8, 4,2, c);
	UI_fillRect(dst, x+3,y+10, 4,2, c);
}
static void UI_bat(SDL_Surface* dst, int x, int y, int battery, SDL_Color c) {
	UI_rect(dst, x,y, 20,14, 2, c);
	UI_rect(dst, x+20,y+4, 2,6, 2, c);
	
	int w = CEIL_TO(battery,20) * 12 / 100;
	if (w>0) UI_fillRect(dst, x+4,y+4,w,6, c);
}
static void UI_battery(int battery, int is_charging, int shadowed) {
	int x = 296;
	int y = 2;
	
	SDL_Color c = LIGHT_COLOR;
	
	if (shadowed) UI_bat(overlay, x+1,y+1, battery, BLACK_COLOR);
	UI_bat(overlay, x,y, battery, c);
	
	if (is_charging) { // && battery<100) {	
		x -= 12;
		y += 1;
		if (shadowed) UI_bolt(overlay, x+1,y+1, BLACK_COLOR);
		UI_bolt(overlay, x,y, c);
	}
}

// --------------------------------------------

int main(void) {
	LOG_init();
	raw_init();
	
	Settings_load();
	Settings_setBrightness(settings.brightness);
	Settings_setVolume(settings.volume);
	
	CPU_setSpeed(CPU_SPEED_DEFAULT);
		
	SDL_Init(SDL_INIT_VIDEO);
	SDL_ShowCursor(0);
	SDL_EnableKeyRepeat(0, 0);
	SDL_Surface* video = SDL_SetVideoMode(VIDEO_WIDTH, VIDEO_HEIGHT, 16, SDL_SWSURFACE);
	SDL_FillRect(video, NULL, 0x0);
	SDL_Flip(video);
	
	Fonts_init();

	DE_init();
	enable_layers();
	// disable_overlay();
	
	reinit_layer(VIDEO_WIDTH, VIDEO_HEIGHT);
	
	UI_gradient(overlay);
	UI_battery(PWR_getBatteryPercent(), 0, 1);
	
	Font_shadowText(overlay, font12, "Dedicated OS", 4,4, LIGHT_COLOR);
	Font_shadowText(overlay, font18, "Game Boy Micro", 4,4+18, WHITE_COLOR);
	dirty_overlay();
	
	SDL_FillRect(scaler, NULL, 0xff282828);
	dirty_scaler();
	
	Pad_reset();
	int menu_combo = 0;
	int quit = 0;
	while (!quit) {
		Pad_update();
		
		if (Pad_justPressed(PAD_MENU)) menu_combo = 0;
		if (menu_combo && Pad_justReleased(PAD_MENU)) Pad_consume(PAD_MENU);
			
		if (Pad_isPressed(PAD_MENU)) {
			if (Pad_justRepeated(PAD_UP)) {
				Pad_consume(PAD_UP);
				menu_combo = 1;
				if (settings.brightness<10) {
					Settings_setBrightness(settings.brightness+1);
				}
			}
			else if (Pad_justRepeated(PAD_DOWN)) {
				Pad_consume(PAD_DOWN);
				menu_combo = 1;
				if (settings.brightness>0) {
					Settings_setBrightness(settings.brightness-1);
				}
			}
			else if (Pad_justRepeated(PAD_LEFT)) {
				Pad_consume(PAD_LEFT);
				menu_combo = 1;
				if (settings.volume>0) {
					Settings_setVolume(settings.volume-1);
				}
			}
			else if (Pad_justRepeated(PAD_RIGHT)) {
				Pad_consume(PAD_RIGHT);
				menu_combo = 1;
				if (settings.volume<20) {
					Settings_setVolume(settings.volume+1);
				}
			}
		}
		
		if (Pad_justPressed(PAD_START)) quit = 1;
		
		// SDL_FillRect(scaler, NULL, 0xff282828);
		// dirty_scaler();
		present_layers(VSYNC_WAIT);
	}
	
	Fonts_quit();
	Settings_save();
	
	DE_quit();
	SDL_Quit();
	raw_quit();
	
	return 0;
}
