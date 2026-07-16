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

// old globals
static int fastforward = 0; // TODO: move to app?

// --------------------------------------------

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

#define SDCARD_PATH			"/mnt/SDCARD"
#define BIOS_PATH			SDCARD_PATH "/bios"
#define GAMES_PATH 			SDCARD_PATH "/games"
#define ARCHIVE_PATH		GAMES_PATH "/archive"
#define SYSTEM_PATH 		SDCARD_PATH "/system"
#define ASSETS_PATH			SYSTEM_PATH "/assets"
#define CORES_PATH			SYSTEM_PATH "/cores"
#define USERDATA_PATH		SDCARD_PATH "/userdata"
#define SAVES_PATH			USERDATA_PATH "/saves"
#define STATES_PATH			USERDATA_PATH "/states"
#define SCREENSHOTS_PATH	USERDATA_PATH "/screenshots"

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
	int frameskip;
	int unused[4];
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

#define	SCREEN_WIDTH		(320)
#define	SCREEN_HEIGHT	(240)

static int	SCALER_WIDTH	= SCREEN_WIDTH; // (240)
static int	SCALER_HEIGHT	= SCREEN_HEIGHT; // (180)

#define	SCALER_X 	((SCREEN_WIDTH - SCALER_WIDTH)/2)
#define	SCALER_Y 	((SCREEN_HEIGHT - SCALER_HEIGHT)/2)
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
#define	LAY2_ADDR_REG_L	(0x858)
#define	LAY2_ADDR_REG_H	(0x862)
#define	DEBE_ADDR(addr)	(((uint32_t)(addr) - 0x80000000) << 3)
// Probe also showed 0x8c4 changing when layer 2 addr changed via ioctl.
// Keep an eye on that if direct address writes do not latch reliably.
// #define PROBE_OVERLAY_REGS
#define	TCON			(0x01C0C000)
#define	TIMING_REG1		(0x4C)
#define	TIMING_REG2		(0x50)
#define	DEBUG_INFO_REG	(0xFC)
#define	LAYER0			(0)
#define	LAYER1			(1)
#define	LAYER2			(2)
#define	LAYER3			(3)
#define	OVERLAY_BUFFERS	(3)

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
int					ov_buffer = 1;
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
static void waitforvblankstart(void) {
	struct timespec sleeptime = {0,0};
	uint16_t *tcon_map, line;

	while (1) {
		tcon_map = (uint16_t*)mmap(0, DEBUG_INFO_REG+4, PROT_READ, MAP_PRIVATE, mem_fd, TCON);
		line = (tcon_map[(DEBUG_INFO_REG+2)/2]) & 0x3ff;
		munmap(tcon_map, DEBUG_INFO_REG+4);

		if (line < vbp) break;

		if (line < 240 + vbp) {
			sleeptime.tv_nsec = (240 + vbp - line) * sleepns_mul;
		}
		else {
			sleeptime.tv_nsec = (vt - line) * sleepns_mul;
		}
		nanosleep(&sleeptime, NULL);
	}
}
static void waitfornextvblankstart(void) {
	waitfordisplayperiod();
	waitforvblankstart();
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
	
	if (sc_dirty) ion_flush(scaler->pixels, scaler->pitch*scaler->h);
	if (ov_dirty) ion_flush(overlay->pixels, overlay->pitch*overlay->h);
	
	if (vsync) waitforvsync();
	// if (vsync) waitforvblankstart(); // waits for too long
	// if (vsync) waitfornextvblankstart(); // waits waaay too long
	
	if (sc_dirty) {
		sc_info.fb.addr[0] = (uintptr_t)sc_meminfo.padd + sc_flip_offset;
		defe_map[BUF_ADDR0_REG/4] = sc_info.fb.addr[0];
		sc_flip_offset ^= scaler->pitch*scaler->h;
		scaler->pixels = (void*)((uintptr_t)sc_meminfo.vadd + sc_flip_offset);
		sc_dirty = 0;
	}
	
	if (ov_dirty) {
		ov_info.fb.addr[0] = (uintptr_t)ov_meminfo.padd + ov_flip_offset;
		debe_map[LAY2_ADDR_REG_L/4] = DEBE_ADDR(ov_info.fb.addr[0]);
		// debe_map[0x8c4/4] = 0x40;
		ov_buffer = (ov_buffer + 1) % OVERLAY_BUFFERS;
		ov_flip_offset = overlay->pitch*overlay->h * ov_buffer;
		overlay->pixels = (void*)((uintptr_t)ov_meminfo.vadd + ov_flip_offset);
		ov_dirty = 0;
	}
	
	if (vsync>1) waitfordisplayperiod();
}

#ifdef PROBE_OVERLAY_REGS
static void probe_overlay_regs(void) {
	uint32_t before[DEBE_SIZE / 4];
	uint32_t after[DEBE_SIZE / 4];
	uint8_t* before8 = (uint8_t*)before;
	uint8_t* after8 = (uint8_t*)after;
	uint32_t original = ov_info.fb.addr[0];
	uint32_t probe = (uintptr_t)ov_meminfo.padd + ov_flip_offset;
	uint32_t args[4] = {0, LAYER2, (uintptr_t)&ov_info, 0};
	
	LOG("probe overlay addr: 0x%08x -> 0x%08x", original, probe);
	memcpy(before, debe_map, sizeof(before));
	
	ov_info.fb.addr[0] = probe;
	if (ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args)<0) fprintf(stderr, "LAYER_SET_INFO failed %s\n",strerror(errno));
	memcpy(after, debe_map, sizeof(after));
	
	for (int i=0; i<DEBE_SIZE/4; i++) {
		if (before[i]!=after[i]) {
			LOG("DEBE word 0x%03x: 0x%08x -> 0x%08x", i*4, before[i], after[i]);
		}
	}
	for (int i=0; i<DEBE_SIZE; i++) {
		if (before8[i]!=after8[i]) {
			LOG("DEBE byte 0x%03x: 0x%02x -> 0x%02x", i, before8[i], after8[i]);
		}
	}
	
	ov_info.fb.addr[0] = original;
	if (ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args)<0) fprintf(stderr, "LAYER_SET_INFO failed %s\n",strerror(errno));
}
#endif

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
	sc_meminfo.size = ((SCREEN_WIDTH*SCREEN_HEIGHT*4)*2 + 4095) & (~4095); // doublebuf
	ov_meminfo.size = ((SCREEN_WIDTH*SCREEN_HEIGHT*4)*OVERLAY_BUFFERS + 4095) & (~4095);
	ion_alloc(&sc_meminfo);
	ion_alloc(&ov_meminfo);
	memset(ov_meminfo.vadd, 0, ov_meminfo.size);
	ion_flush(ov_meminfo.vadd, ov_meminfo.size);
	
	scaler = SDL_CreateRGBSurfaceFrom(sc_meminfo.vadd + SCALER_WIDTH*SCALER_HEIGHT*4, SCALER_WIDTH, SCALER_HEIGHT,
			32, SCALER_WIDTH*4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	overlay = SDL_CreateRGBSurfaceFrom(ov_meminfo.vadd + SCREEN_WIDTH*SCREEN_HEIGHT*4, SCREEN_WIDTH, SCREEN_HEIGHT,
			32, SCREEN_WIDTH*4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	
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
	ov_info.screen_win.width = SCREEN_WIDTH;			// frame.w
	ov_info.screen_win.height = SCREEN_HEIGHT;		// frame.h
	ov_info.fb.addr[0] = (uintptr_t)ov_meminfo.padd;// address[0]
	ov_info.fb.size.width = SCREEN_WIDTH;			// framebuffer.w
	ov_info.fb.size.height = SCREEN_HEIGHT;			// framebuffer.h
	ov_info.fb.format = DISP_FORMAT_ARGB_8888;		// fmt
	ov_info.fb.pre_multiply = 0;					// pre_mult
	ov_info.fb.src_win.x = 0;						// source crop.x
	ov_info.fb.src_win.y = 0;						// source crop.y
	ov_info.fb.src_win.width = SCREEN_WIDTH;			// source crop.w
	ov_info.fb.src_win.height = SCREEN_HEIGHT;		// source crop.h
	
	args[1] = LAYER2; args[2] = (uintptr_t)&ov_info;
	if (ioctl(disp_fd, DISP_CMD_LAYER_SET_INFO, &args)<0) fprintf(stderr, "LAYER_SET_INFO failed %s\n",strerror(errno));
	
	ov_flip_offset = overlay->pitch*overlay->h;
	ov_buffer = 1;
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
	// TODO: w,h must be <= SCREEN_*
	
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
	
	float scale_x = (float)SCREEN_WIDTH / SCALER_WIDTH;
	float scale_y = (float)SCREEN_HEIGHT / SCALER_HEIGHT;
	scale = scale_x < scale_y ? scale_x : scale_y;
	
	if (scale_mode==SCALE_NONE) {
		scale = (int)scale; // integer
	}
	// scale = (float)((int)(scale*2))/2; // nearest 50% scale
	fprintf(stderr,"reinit_layer %ix%i (%0.2f%%)\n",w,h,scale); fflush(stderr);
	
	int sw = round_to_nearest((int)(SCALER_WIDTH * scale + 0.5f), 2);
    int sh = round_to_nearest((int)(SCALER_HEIGHT * scale + 0.5f), 2);
	
	// if (scale_mode==SCALE_FULL) {
	// 	sw = SCREEN_WIDTH;
	// 	sh = SCREEN_HEIGHT;
	// }

    int ox = round_to_nearest((SCREEN_WIDTH - sw) / 2, 2);
    int oy = round_to_nearest((SCREEN_HEIGHT - sh) / 2, 2);
	
	resize_scaler(ox,oy,sw,sh);
	// resize_scaler(0,0,SCREEN_WIDTH,SCREEN_HEIGHT); // fullscreen
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

static Font* font6 = &(Font){
	.name = "font-dedicated-6.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%,: ",
	.tile_width = 6,
	.tile_height = 6,
	.tracking = 1,
	.char_width = 5,
	.char_widths = {
		['I'] = 2,
		['1'] = 4,
		['.'] = 2,
		[','] = 2,
		['-'] = 4,
		['\''] = 2,
		['!'] = 2,
		['1'] = 4,
		[':'] = 2,
		[' '] = 2,
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
	Font_init(font6);
}
static void Fonts_quit(void) {
	Font_quit(font18);
	Font_quit(font12);
	Font_quit(font6);
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

#define RATE 48000
#define SAMPLES 512
#define CHANNELS 2
#define BUFFER_DURATION 2 // in seconds

typedef struct {
	int16_t left, right;
} SND_Frame;

static struct SND_Context {
	double frame_rate;
	double resample_cursor;
	double resample_ratio;
	
	int ready;
	int sample_rate_in;
	int sample_rate_out;
	int frame_in;
	int frame_out;
	int frame_count;
	SND_Frame* buffer;
} snd = {0};
void SND_consumeCallback(void* userdata, uint8_t* stream, int len);

static inline int SND_availableToRead(void) {
	if (snd.frame_count <= 0) return 0;
	return (snd.frame_in - snd.frame_out + snd.frame_count) % snd.frame_count;
}
static inline int SND_availableToWrite(void) {
	if (snd.frame_count <= 0) return 0;
	return (snd.frame_out - snd.frame_in - 1 + snd.frame_count) % snd.frame_count;
}

int SDN_underrunLikely(double rate_in, int samples_in, double rate_out, int samples_out) {
	LOG("test %f %i against %f %i", rate_in, samples_in, rate_out, samples_out);
	int total = 0;
	double elapsed = 0;
	double next1 = 0;
	double next2 = 0;
	double dur = 0.5;
	
	while (elapsed<dur && total>=0) {
		LOG("tick %f", elapsed);
		while (elapsed>=next1) {
			total += samples_in;
			LOG("- in: %i", total);
			next1 += 1 / rate_in;
		}
		while (elapsed>=next2) {
			total -= samples_out;
			LOG("- out: %i", total);
			next2 += 1 / rate_out;
		}
		
		if (total<0) return 1;

		elapsed += fmin(next1,next2) - elapsed;
	}
	
	return 0;
}

void SND_init(double sample_rate, double frame_rate) {
	// LOG("gambatte underrun likely: %i", 	SDN_underrunLikely(59.727501,  548, 93.75, 512));
	// LOG("gpsp underrun likely: %i", 		SDN_underrunLikely(59.727501, 1092, 93.75, 512));
	// LOG("quicknes underrun likely: %i", 	SDN_underrunLikely(60.000000,  735, 93.75, 512));
	
	SDL_InitSubSystem(SDL_INIT_AUDIO);
	memset(&snd, 0, sizeof(snd));
	
	SDL_AudioSpec want = {
		.freq = RATE,
		.format = AUDIO_S16,
		.channels = CHANNELS,
		.samples = SAMPLES,
		.callback = SND_consumeCallback,
	};
	SDL_AudioSpec have;
	
	if (SDL_OpenAudio(&want, &have) < 0) {
		LOG("SDL_OpenAudio error: %s", SDL_GetError());
		return;
	}
	
	snd.frame_rate = frame_rate;
	snd.sample_rate_in = sample_rate;
	snd.sample_rate_out = have.freq;
	LOG("sample_rate_in: %i sample_rate_out: %i", snd.sample_rate_in, snd.sample_rate_out);
	// unpaused in the first produce callback
	
	snd.resample_ratio = (double)snd.sample_rate_in / (double)snd.sample_rate_out;
	
	int sample_count_in = (int)(sample_rate / frame_rate);
	// int sample_count_out = have.samples;
	// detecting underrun chance is more complicated because of resampling
	
	int samples_per_frame = (sample_count_in + (SAMPLES-1)) & ~(SAMPLES-1); // 
	snd.frame_in = 0;
	snd.frame_out = 0;
	snd.frame_count = samples_per_frame * BUFFER_DURATION;

	int buffer_size = sizeof(SND_Frame) * snd.frame_count;
	snd.buffer = malloc(buffer_size);
	memset(snd.buffer, 0, buffer_size);
}
void SND_quit(void) {
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	free(snd.buffer);
	memset(&snd, 0, sizeof(snd));
}
void SND_pause(void) {
	SDL_PauseAudio(1);
}
void SND_resume(void) {
	if (!snd.ready) return;
	SDL_PauseAudio(0);
}

size_t SND_produceCallback(const SND_Frame* frames, size_t count) {
	// LOG("SND_produceCallback(%i)", count);
	if (!snd.buffer || snd.frame_count <= 0) return 0;
	if (count >= (size_t)snd.frame_count) {
		frames += count - (snd.frame_count - 1);
		count = snd.frame_count - 1;
	}
	
	SDL_LockAudio();
	// LOG("producer waiting? count (%i) > available (%i))...", count, SND_availableToWrite());
	uint32_t wait_start = SDL_GetTicks();
	while (count > SND_availableToWrite()) {
		// LOG("producer waiting (%i)...", SND_availableToWrite());
		SDL_UnlockAudio();
		SDL_Delay(1);
		SDL_LockAudio();
		if (SDL_GetTicks() - wait_start > 250) {
			SDL_UnlockAudio();
			LOG("SND_produceCallback timeout; dropping %u frames", (unsigned)count);
			return 0;
		}
	}
	for (int i=0; i<count; ++i) {
		snd.buffer[snd.frame_in] = frames[i];
		snd.frame_in = (snd.frame_in + 1) % snd.frame_count;
	}
	SDL_UnlockAudio();
	
	// LOG_debug("enqueued %i\n", count);
	if (!snd.ready) {
		LOG("SND ready! anticipated: %i received: %i", (int)(snd.sample_rate_in / snd.frame_rate), count);
		snd.ready = 1;
		SDL_PauseAudio(0);
	}
	return count;
}

void SND_consumeCallback(void* userdata, uint8_t* stream, int len) {
	int16_t* out = (int16_t*)stream;
	int count = len / sizeof(SND_Frame);
	// LOG("SND_consumeCallback(%i)", count);
	if (!snd.buffer || snd.frame_count <= 0) {
		memset(stream, 0, len);
		return;
	}

	double ratio = snd.resample_ratio; // * snd.resample_drift;
	double fill = (double)SND_availableToRead() / (double)snd.frame_count;
	if (fill<0.5) ratio *= 0.99; // simple attempt to get ahead of underrun
	
	for (int i=0; i<count; ++i) {
		int available = SND_availableToRead();
		if (available < 2) { // need 2 frames to resample
			// LOG("consumer waiting (%i)...\n", available);
			*out++ = 0;
			*out++ = 0;
			continue;
		}

		int index_a = snd.frame_out;
		int index_b = (index_a + 1) % snd.frame_count;

		SND_Frame a = snd.buffer[index_a];
		SND_Frame b = snd.buffer[index_b];

		double t = snd.resample_cursor;
		int16_t left  = (int16_t)((1.0 - t) * a.left  + t * b.left);
		int16_t right = (int16_t)((1.0 - t) * a.right + t * b.right);

		*out++ = left;
		*out++ = right;

		snd.resample_cursor += ratio;

		while (snd.resample_cursor >= 1.0) {
			snd.frame_out = (snd.frame_out + 1) % snd.frame_count;
			snd.resample_cursor -= 1.0;
		}
	}
}

// --------------------------------------------

static SDL_Surface *framebuffer = NULL;
static void Framebuffer_quit(void) {
	if (framebuffer) SDL_FreeSurface(framebuffer);
}

// --------------------------------------------

typedef enum {
	OSD_NONE,
	OSD_VOLUME,
	OSD_BRIGHTNESS,
} OSDMode;

static struct {
	SDL_Surface *icons;

	OSDMode osd;
	uint32_t osd_at;
	
	int menu;
} ui;

static void UI_init(void) {
	raw_init();
	
	Settings_load();
	Settings_setBrightness(settings.brightness);
	Settings_setVolume(settings.volume);
	
	CPU_setSpeed(CPU_SPEED_DEFAULT);
	
	SDL_Init(SDL_INIT_VIDEO);
	SDL_ShowCursor(0);
	SDL_EnableKeyRepeat(0, 0);
	SDL_Surface* video = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 16, SDL_SWSURFACE);
	SDL_FillRect(video, NULL, 0x0);
	SDL_Flip(video);
	
	DE_init();
	enable_layers();
	disable_overlay();
	
	reinit_layer(SCREEN_WIDTH, SCREEN_HEIGHT); // TODO: remove
	
	Fonts_init(); // TODO: can this be deferred to extras?
	Pad_reset();
}
static void UI_quit(void) {
	if (ui.icons) SDL_FreeSurface(ui.icons);
	
	Fonts_quit();
	Framebuffer_quit();
	Settings_save();
	
	DE_quit();
	SDL_Quit();
	raw_quit();
}
static void UI_extras(void) {
	if (!ui.icons) ui.icons = IMG_Load(ASSETS_PATH "/icons.png");
}
static void UI_setOSD(OSDMode osd) {
	if (ui.osd==OSD_NONE && osd!=OSD_NONE) {
		if (!ui.menu) {
			SDL_FillRect(overlay, NULL, 0);
			dirty_overlay();
			present_layers(VSYNC_NONE);
			enable_overlay();
		}
	}
	ui.osd = osd;
	ui.osd_at = SDL_GetTicks();
}
static void UI_update(void) {
	if (ui.osd!=OSD_NONE && SDL_GetTicks()>ui.osd_at+1000) {
		ui.osd = OSD_NONE;
		if (!ui.menu) disable_overlay();
	}
}

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
static void UI_OSD(char* label, int value, int max, int bottom) {
	int nh = 14;
	int x,y,w,h;
	Font_getTextSize(font6, label, &w, &h);
	w = 106;
	h = 31;
	x = (SCREEN_WIDTH - w) / 2;
	y = (bottom - h) - 4;
	
	UI_rect(overlay, x,y,w,h, 0, TRIAD_ALPHA(BLACK_TRIAD,0x60));

	Font_shadowText(overlay, font6, label, x+4, y+4, LIGHT_COLOR);
	
	int nw = max==10?8:3;
	int no = max==10?10:5;
	for (int i=0; i<max; i++) {
		int nx = x + 4;
		int ny = y + 13;
		UI_rect(overlay, nx+i*no+1,ny+1,nw,nh, 0, BLACK_COLOR);
		if (i<value) {
			UI_rect(overlay, nx+i*no,ny,nw,nh, 0, WHITE_COLOR);
		}
	}
}

// --------------------------------------------

static void log_callback(enum retro_log_level level, const char *fmt, ...) {
	(void)level;
	
	fprintf(stdout, "[dos] core: ");

	va_list args;
	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);
}
// TODO: move to settings
static int core_options_dirty = 0;
static bool environment_callback(unsigned cmd, void *data) {
	switch (cmd) {
		case RETRO_ENVIRONMENT_SET_MESSAGE: {
			const struct retro_message *message = (const struct retro_message *)data;
			if (message && message->msg) LOG("core: message: %s\n", message->msg);
			return true;
		}
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
			struct retro_log_callback *log = (struct retro_log_callback *)data;
			if (log) log->log = log_callback;
			return true;
		}
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
			const enum retro_pixel_format *format = (const enum retro_pixel_format *)data;
			if (!format || *format!=RETRO_PIXEL_FORMAT_RGB565) LOG("unsupported pixel format!");
			return format && *format==RETRO_PIXEL_FORMAT_RGB565;
		}
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
		    bool *out = data;
		    *out = core_options_dirty;
		    core_options_dirty = 0;
		    return true;
		}
		case RETRO_ENVIRONMENT_GET_VARIABLE: {
			struct retro_variable *var = (struct retro_variable *)data;
			if (!var || !var->key) return false;
			var->value = NULL;
			
			// gambatte
			if (strcmp(var->key, "gambatte_gb_bootloader") == 0) {
				var->value = "enabled";
			}
			else if (strcmp(var->key, "gambatte_gb_colorization") == 0) {
				var->value = "internal";
			}
			else if (strcmp(var->key, "gambatte_gb_internal_palette") == 0) {
				var->value = "PixelShift - Pack 1";
			}
			else if (strcmp(var->key, "gambatte_gb_palette_pixelshift_1") == 0) {
				var->value = "PixelShift 27 - GBP Bivert";
			}
			
			// gpsp
			else if (strcmp(var->key, "gpsp_boot_mode") == 0) {
				var->value = "bios"; // TODO: only if present
			}
			else if (strcmp(var->key, "gpsp_frameskip") == 0) {
				var->value = settings.frameskip ? "fixed_interval" : "disabled";
			}
			else if (strcmp(var->key, "gpsp_frameskip_interval") == 0) {
				var->value = "1";
			}
			
			// pokemini
			else if (strcmp(var->key, "pokemini_video_scale") == 0) {
				var->value = "2x";
			}
			else if (strcmp(var->key, "pokemini_palette") == 0) {
				var->value = "Old";
			}
			else if (strcmp(var->key, "pokemini_60hz_mode") == 0) {
				var->value = "enabled";
			}
			else if (strcmp(var->key, "pokemini_lowpass_filter") == 0) {
				var->value = "enabled";
			}
			
			return var->value != NULL;
		}
		case RETRO_ENVIRONMENT_GET_OVERSCAN:
		case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
			if (data) *(bool *)data = true;
			return true;
		}
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
			const char **out = (const char **)data;
			if (out) *out = BIOS_PATH;
			return true;
		}
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
			if (data) *(const char **)data = SAVES_PATH;
			return true;
		}
		
		default: return false;
	}
}
static void video_refresh_callback(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!data) return;
	
	if (framebuffer && (framebuffer->w!=width || framebuffer->h!=height || framebuffer->pitch!=pitch)) {
		SDL_FreeSurface(framebuffer);
		framebuffer = NULL;
	}
	
	if (!framebuffer) {
		framebuffer = SDL_CreateRGBSurfaceFrom(NULL, width, height, 16, pitch, 0xF800, 0x07E0, 0x001F, 0x0000);
		reinit_layer(width, height);
	}
	
	framebuffer->pixels = (void*)data;
	SDL_BlitSurface(framebuffer, &(SDL_Rect){0,0,width,height}, scaler, &(SDL_Rect){(SCALER_WIDTH-width)/2,(SCALER_HEIGHT-height)/2});
	
	dirty_scaler();
}
static void audio_sample_callback(int16_t left, int16_t right) {
	if (fastforward) return;
	SND_produceCallback(&(const SND_Frame){left,right}, 1);
}
static size_t audio_sample_batch_callback(const int16_t *data, size_t frames) {
	if (fastforward) return frames;
	return SND_produceCallback((const SND_Frame*)data, frames);
}

static int App_listen(void);
static void input_poll_callback(void) {
	Pad_update();
	App_listen();
}
static int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void)index;
	
	if (port > 0 || device != RETRO_DEVICE_JOYPAD || Pad_isPressed(PAD_MENU)) return 0;

	switch (id) {
		case RETRO_DEVICE_ID_JOYPAD_UP:			return Pad_isPressed(PAD_UP);
		case RETRO_DEVICE_ID_JOYPAD_DOWN:		return Pad_isPressed(PAD_DOWN);
		case RETRO_DEVICE_ID_JOYPAD_LEFT:		return Pad_isPressed(PAD_LEFT);
		case RETRO_DEVICE_ID_JOYPAD_RIGHT:		return Pad_isPressed(PAD_RIGHT);
		case RETRO_DEVICE_ID_JOYPAD_A:			return Pad_isPressed(PAD_A);
		case RETRO_DEVICE_ID_JOYPAD_B:			return Pad_isPressed(PAD_B);
		case RETRO_DEVICE_ID_JOYPAD_X:			return Pad_isPressed(PAD_X);
		case RETRO_DEVICE_ID_JOYPAD_Y:			return Pad_isPressed(PAD_Y);
		case RETRO_DEVICE_ID_JOYPAD_L:			return Pad_isPressed(PAD_L1);
		case RETRO_DEVICE_ID_JOYPAD_R:			return Pad_isPressed(PAD_R1);
		case RETRO_DEVICE_ID_JOYPAD_SELECT:		return Pad_isPressed(PAD_SELECT);
		case RETRO_DEVICE_ID_JOYPAD_START:		return Pad_isPressed(PAD_START);
		default:								return 0;
	}
}

// --------------------------------------------

static struct {
	char path[MAX_PATH];
	char name[MAX_FILE];
	
	double fps;
	double sample_rate;
	double aspect_ratio;
	
	void *handle;
	void (*init)(void);
	void (*deinit)(void);
	
	void (*get_system_info)(struct retro_system_info *info);
	void (*get_system_av_info)(struct retro_system_av_info *info);
	void (*set_controller_port_device)(unsigned port, unsigned device);
	
	void (*reset)(void);
	void (*run)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *data, size_t size);
	bool (*unserialize)(const void *data, size_t size);
	bool (*load_game)(const struct retro_game_info *game);
	bool (*load_game_special)(unsigned game_type, const struct retro_game_info *info, size_t num_info);
	void (*unload_game)(void);
	unsigned (*get_region)(void);
	void *(*get_memory_data)(unsigned id);
	size_t (*get_memory_size)(unsigned id);
} core;
static struct {
	char path[MAX_PATH];
	char name[MAX_FILE];	
	uint8_t *data;
	size_t size;
} game;

// --------------------------------------------

typedef enum {
	CONSOLE_UNKNOWN = -1,
	CONSOLE_GAME_BOY_ADVANCE,
	CONSOLE_GAME_BOY_COLOR,
	CONSOLE_GAME_BOY,
	CONSOLE_POKEMON_MINI,
	CONSOLE_COUNT,
} ConsoleId;
typedef struct {
	const char *core;
	const char *name;
	const char *slug;
} Console;
static Console consoles[CONSOLE_COUNT] = {
	[CONSOLE_GAME_BOY_ADVANCE] = {"gpsp","Game Boy Advance", "gba"},
	[CONSOLE_GAME_BOY_COLOR] = {"gambatte", "Game Boy Color", "gbc"},
	[CONSOLE_GAME_BOY] = {"gambatte", "Game Boy", "gb"},
	[CONSOLE_POKEMON_MINI] = {"pokemini", "Pokemon Mini", "pkm"},
};
static Console *Console_for(const char *path) {
	const char *ext = strrchr(path, '.');
	if (!ext) return NULL;
	
		 if (strcasecmp(ext, ".gba")==0) return &consoles[CONSOLE_GAME_BOY_ADVANCE];
	else if (strcasecmp(ext, ".min")==0) return &consoles[CONSOLE_POKEMON_MINI];
	else if (strcasecmp(ext, ".gbc")==0) return &consoles[CONSOLE_GAME_BOY_COLOR];
	else if (strcasecmp(ext, ".gb")==0)	 return &consoles[CONSOLE_GAME_BOY];
	else if (strcasecmp(ext, ".dmg")==0) return &consoles[CONSOLE_GAME_BOY];

	return NULL;
}

static void SRAM_getPath(char* path) {
	sprintf(path, "%s/%s.srm", SAVES_PATH, game.name);
}
static void SRAM_read(void) {
	size_t sram_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) return;
	
	char path[MAX_PATH];
	SRAM_getPath(path);
	LOG("sav path (read): %s", path);
	
	FILE *sram_file = fopen(path, "r");
	if (!sram_file) return;

	void* sram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!sram || !fread(sram, 1, sram_size, sram_file)) {
		LOG("Error reading SRAM data");
	}

	fclose(sram_file);
}
static void SRAM_write(void) {
	size_t sram_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) return;
	
	char path[MAX_PATH];
	SRAM_getPath(path);
	LOG("sav path (write): %s", path);
		
	FILE *sram_file = fopen(path, "w");
	if (!sram_file) {
		LOG("Error opening SRAM file: %s", strerror(errno));
		return;
	}

	void *sram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!sram || sram_size != fwrite(sram, 1, sram_size, sram_file)) {
		LOG("Error writing SRAM data to file");
	}

	fclose(sram_file);
	sync();
}

// --------------------------------------------

static int state_slot = 0;
static void State_getPath(char* path, const char *name) {
	sprintf(path, "%s/%s.st%i", STATES_PATH, name, state_slot);
}
static void State_getPreviewPath(char* path, const char *name) {
	sprintf(path, "%s/%s.%i.bmp", SCREENSHOTS_PATH, name, state_slot);
}
static void State_read(void) {
	size_t state_size = core.serialize_size();
	if (!state_size) return;

	void *state = calloc(1, state_size);
	if (!state) {
		LOG("Couldn't allocate memory for state");
		return;
	}

	char path[MAX_PATH];
	State_getPath(path, game.name);
	
	FILE *state_file = fopen(path, "rb");
	if (!state_file) {
		LOG("Error opening state file: %s (%s)", path, strerror(errno));
	}
	else {
		size_t read_size = fread(state, 1, state_size, state_file);
		int read_error = ferror(state_file);
		fclose(state_file);
		
		if (read_error) {
			LOG("Error reading state file: %s (%s)", path, strerror(errno));
		}
		else if (!read_size) {
			LOG("State file is empty: %s", path);
		}
		else if (!core.unserialize(state, state_size)) {
			LOG("Error restoring save state: %s (%s)", path, strerror(errno));
		}
	}
	
	free(state);
}
static void State_write(void) {
	size_t state_size = core.serialize_size();
	if (!state_size) return;

	void *state = malloc(state_size);
	if (!state) {
		LOG("Couldn't allocate memory for state");
		return;
	}

	char path[MAX_PATH];
	State_getPath(path, game.name);
	
	if (!core.serialize(state, state_size)) {
		LOG("Error creating save state: %s", path);
		free(state);
		return;
	}
	
	FILE *state_file = fopen(path, "wb");
	if (!state_file) {
		LOG("Error opening state file: %s (%s)", path, strerror(errno));
	}
	else {
		if (fwrite(state, 1, state_size, state_file)!=state_size) {
			LOG("Error writing state data to file: %s (%s)", path, strerror(errno));
		}
		fclose(state_file);
		sync();
	}

	free(state);
	
	State_getPreviewPath(path, game.name);
	SDL_SaveBMP(framebuffer, path);
}
static void State_autosave(void) {
	State_write();
}
static void State_resume(void) {
	State_read();
}

// --------------------------------------------

static void Core_open(void) {
	LOG("core: open");
	
	core.handle = dlopen(core.path, RTLD_LAZY);
	if (!core.handle) {
		LOG("ui: core dlopen failed: %s: %s", core.path, dlerror());
		return;
	}
	
	#define CORE_MAP(field, name) ((field) = (__typeof__(field))dlsym(core.handle, (name)))
	
	CORE_MAP(core.init, "retro_init");
	CORE_MAP(core.deinit, "retro_deinit");
	CORE_MAP(core.get_system_info, "retro_get_system_info");
	CORE_MAP(core.get_system_av_info, "retro_get_system_av_info");
	CORE_MAP(core.set_controller_port_device, "retro_set_controller_port_device");
	CORE_MAP(core.reset, "retro_reset");
	CORE_MAP(core.run, "retro_run");
	CORE_MAP(core.serialize_size, "retro_serialize_size");
	CORE_MAP(core.serialize, "retro_serialize");
	CORE_MAP(core.unserialize, "retro_unserialize");
	CORE_MAP(core.load_game, "retro_load_game");
	CORE_MAP(core.load_game_special, "retro_load_game_special");
	CORE_MAP(core.unload_game, "retro_unload_game");
	CORE_MAP(core.get_region, "retro_get_region");
	CORE_MAP(core.get_memory_data, "retro_get_memory_data");
	CORE_MAP(core.get_memory_size, "retro_get_memory_size");
	
	void (*set_environment_callback)(retro_environment_t);
	void (*set_video_refresh_callback)(retro_video_refresh_t);
	void (*set_audio_sample_callback)(retro_audio_sample_t);
	void (*set_audio_sample_batch_callback)(retro_audio_sample_batch_t);
	void (*set_input_poll_callback)(retro_input_poll_t);
	void (*set_input_state_callback)(retro_input_state_t);
	
	CORE_MAP(set_environment_callback, "retro_set_environment");
	CORE_MAP(set_video_refresh_callback, "retro_set_video_refresh");
	CORE_MAP(set_audio_sample_callback, "retro_set_audio_sample");
	CORE_MAP(set_audio_sample_batch_callback, "retro_set_audio_sample_batch");
	CORE_MAP(set_input_poll_callback, "retro_set_input_poll");
	CORE_MAP(set_input_state_callback, "retro_set_input_state");
	
	#undef CORE_MAP
	
	set_environment_callback(environment_callback);
	set_video_refresh_callback(video_refresh_callback);
	set_audio_sample_callback(audio_sample_callback);
	set_audio_sample_batch_callback(audio_sample_batch_callback);
	set_input_poll_callback(input_poll_callback);
	set_input_state_callback(input_state_callback);
	
	LOG("core: init");
	core.init();
}
static void Core_configure(void) {
	LOG("core: configure");
	
	struct retro_system_av_info av_info = {0};
	core.get_system_av_info(&av_info);
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

	core.fps = av_info.timing.fps;
	core.sample_rate = av_info.timing.sample_rate;
	double a = av_info.geometry.aspect_ratio;
	int w = av_info.geometry.base_width;
	int h = av_info.geometry.base_height;
	if (a<=0) a = (double)w / h;
	core.aspect_ratio = a;

	LOG("ratio: %f (%ix%i) fps: %f sample_rate: %f", a, w, h, core.fps, core.sample_rate);

	SND_init(core.sample_rate, core.fps);
	
	// DOS_SetSourceSampleRate(core.sample_rate);
	// Settings_setVolume(settings.volume);
}
static void Core_reset(void) {
	core.reset();
}
static void Core_close(void) {
	LOG("core: close");
	SND_quit();
	core.deinit();
	dlclose(core.handle);
}

static void Game_open(void) {
	LOG("game: open");
	
	FILE *file = fopen(game.path, "r");
	if (!file) return;
	
	fseek(file, 0, SEEK_END);
	game.size = ftell(file);
	rewind(file);
	game.data = (uint8_t *)malloc(game.size);
	fread(game.data, sizeof(uint8_t), game.size, file);
	fclose(file);
	
	LOG("game.path: %s", game.path);
	LOG("game.name: %s", game.name);
	
	struct retro_game_info game_info;
	game_info.path = game.path;
	game_info.data = game.data;
	game_info.size = game.size;
	core.load_game(&game_info);
	
	SRAM_read();
	
	Core_configure();
	
	State_resume();
}
static void Game_close(void) {
	LOG("game: close");
	LOG("name: %s", game.name);
	
	State_autosave();
	SRAM_write();
	core.unload_game();
	free(game.data);
}

// --------------------------------------------

typedef struct {
	char name[MAX_FILE];
	int hidden;
} Entry;

static struct {
	int count;
	int current;
	int capacity;
	Entry* items;
	
	int next;
	int reload;
	int quit;
} app;

// --------------------------------------------

typedef enum {
	MODE_MENU,
	MODE_ARCHIVE,
} MenuMode;
typedef enum {
	ITEM_SAVE,
	ITEM_LOAD,
	ITEM_ARCHIVE,
	ITEM_RESET,
} MenuItem;
typedef enum {
	PREVIEW_CURRENT,
	PREVIEW_STATE,
	PREVIEW_RESET,
} PreviewType;

static MenuItem current_items[] = {
	ITEM_SAVE,
	ITEM_LOAD,
	ITEM_ARCHIVE,
	ITEM_RESET,
};
static MenuItem other_items[] = {
	ITEM_LOAD,
	ITEM_ARCHIVE,
};

static struct {
	MenuMode mode;
	MenuItem *items;
	int count;
	int current;
	int selected;
	bool dirty;
	bool quit;
} menu;

static const char *Menu_getItemName(MenuItem item) {
	switch (item) {
		case ITEM_SAVE:		return "SAVE";
		case ITEM_LOAD:		return "LOAD";
		case ITEM_ARCHIVE:	return "ARCHIVE";
		case ITEM_RESET:	return "RESET";
	}
	return NULL;
}
static void Menu_select(int selected) {
	if (selected>=menu.count) selected -= menu.count;
	if (selected<0) selected += menu.count;
	
	menu.selected = selected;
	menu.dirty = 1;
}
static void Menu_switch(int current) {
	menu.current = current;
	menu.dirty = 1;

	if (menu.current==app.current) {
		menu.items = current_items;
		menu.count = NUMBER_OF(current_items);
	}
	else {
		menu.items = other_items;
		menu.count = NUMBER_OF(other_items);
	}

	Menu_select(0);
}

static void Menu_init(void) {
	SND_pause();
	ui.menu = 1;
	menu.current = app.current;
	menu.selected = 0;
	menu.mode = MODE_MENU;
	menu.quit = 0;
	menu.items = current_items;
	menu.count = NUMBER_OF(current_items);
	menu.dirty = 1;
	UI_extras();
}
static void Menu_quit(void) {
	ui.menu = 0;
	menu.quit = 1;
	if (!app.reload && !app.quit) SND_resume();
}

// --------------------------------------------

static int compareNatural(const char *a, const char *b) {
	while (*a && *b) {
		// ensure "Game 10.ext" sorts after "Game 2.ext"
		if (isdigit(*a) && isdigit(*b)) {
			char *ea, *eb;
			long na = strtol(a, &ea, 10);
			long nb = strtol(b, &eb, 10);
			
			if (na!=nb) return (na<nb) ? -1 : 1;

			a = ea;
			b = eb;
		} else {
			int ca = tolower(*a);
			int cb = tolower(*b);
			
			if (ca!=cb) {
				// special case: treat '.' as less than ' '
				// ensure "Game.ext" sorts before "Game 2.ext"
				if (ca=='.' && cb==' ') return -1;
				if (ca==' ' && cb=='.') return  1;
				return ca - cb;
			}
			
			a++;
			b++;
		}
	}
	return tolower(*a) - tolower(*b);
}
static int App_sort(const void* a, const void* b) {
	const Entry *i = (const Entry *)a;
	const Entry *j = (const Entry *)b;
	return compareNatural(i->name, j->name);
}

static void App_empty(void) {
	LOG("TODO: empty");
}

static void App_set(int i) {
	if (!app.count) App_empty();
	
	if (i>=app.count) i -= app.count;
	if (i<0) i += app.count;
	app.current = i;
	
	Entry* item = &app.items[app.current];
	strcpy(settings.game, item->name);

	sprintf(game.path, "%s/%s", item->hidden ? ARCHIVE_PATH : GAMES_PATH, settings.game);
	
	strcpy(game.name, settings.game);
	char *dot = strrchr(game.name, '.');
	if (dot && dot!=game.name) *dot = '\0';
	
	LOG("settings.game: %s", settings.game);
}
static int App_next(int start, int dir) {
	int i = start;
	int count = app.count;
	for (int _=0; _<count; _++) {
		i += dir;
		if (i>=count) i -= count;
		else if (i<0) i += count;
		if (!app.items[i].hidden || i==app.current) return i;
	}
	return start;
}

static void App_getDisplayName(const char* in_name, char* out_name) {
	char* tmp;
	char work_name[MAX_FILE];
	strcpy(work_name, in_name);
	strcpy(out_name, in_name);
	
	// extract just the filename if necessary
	tmp = strrchr(work_name, '/');
	if (tmp) strcpy(out_name, tmp+1);
	
	// remove extension(s), eg. .p8.png
	while ((tmp = strrchr(out_name, '.'))!=NULL) {
		int len = strlen(tmp);
		if (len>2 && len<=5) {
			if (tmp[1]==' ') tmp[1] = '\0'; // excempt . followed by whitespace
			else tmp[0] = '\0'; // 1-4 letter extension plus dot (was 1-3, extended for .doom files)
		}
		else break;
	}
	
	// remove trailing parens (round and square)
	strcpy(work_name, out_name);
	while ((tmp=strrchr(out_name, '('))!=NULL || (tmp=strrchr(out_name, '['))!=NULL) {
		if (tmp==out_name) break;
		tmp[0] = '\0';
		tmp = out_name;
	}
	
	// make sure we haven't nuked the entire name
	if (out_name[0]=='\0') strcpy(out_name, work_name);
	
	// remove trailing whitespace
	tmp = out_name + strlen(out_name) - 1;
	while(tmp>out_name && isspace((unsigned char)*tmp)) tmp--;
	tmp[1] = '\0';
}
static int App_wrap(Font* font, char* text, int max_lines, char** lines, int* splits) {
	int line_count = 0;

	char line[1024] = {0};
	char word[256] = {0};
	char* p = text;
	int line_width = 0;
	
	while (*p) {
		char* start = p;
	
		// get next word
		while (*p && !isspace(*p)) p++;
		int word_len = p - start;
		strncpy(word, start, word_len);
		word[word_len] = '\0';
		
		// wrap on hyphen
		if (strcmp(word, "-")==0) {
			if (strlen(line)>0 && line_count<max_lines) {
				lines[line_count++] = strdup(line);
				splits[line_count] = 1;
				line[0] = '\0';
			}
			while (isspace(*p)) p++;
			continue;
		}
	
		// append to line
		char test_line[1024] = {0};
		if (strlen(line)>0) sprintf(test_line,"%s %s", line, word);
		else sprintf(test_line,"%s", word);
	
		Font_getTextSize(font, test_line, &line_width, NULL);
		line_width += font->offset_x * 2;
		
		int ow = 4 + 4;
		if (line_width<=SCREEN_WIDTH-ow) {
			strcpy(line, test_line);
		}
		else {
			if (line_count<max_lines) {
				lines[line_count++] = strdup(line);
			}
			else {
				line[0] = '\0';
				break;
			}
			strcpy(line, word);
		}
	
		// advance to next word
		while (isspace(*p)) p++;
	}

	// add trailing line
	if (strlen(line)>0 && line_count<max_lines) {
		lines[line_count++] = strdup(line);
	}
	return line_count;
}
static void App_trunc(Font* font, const char* text, int max_width, char* out_text) {
	int fw = 0;
	Font_getTextSize(font, text, &fw, NULL);
	
	if (fw<=max_width) {
		strcpy(out_text, text);
		return;
	}
	
	const char* sep = "...";
	int slen = strlen(sep);
	int sw = 0;
	Font_getTextSize(font, sep, &sw, NULL);
	// width(head) + width(sep) + width(tail) != width(head + sep + tail)
	// because it doesn't account for tracking/kerning against sep and head/tail
	// so we have to fudge it here or out_text could be wider than max_width
	sw += font->tracking * 2;
	
	fw = max_width - sw;
	int hw = fw / 2;
	int len = strlen(text);
	
	char tmp[MAX_FILE];
	int head = 0;
	int tail = 0;

	#define WORDS_ONLY 1 // looks much better to me
	
	// head
	for (int i=0; i<len; i++) {
		if (WORDS_ONLY && i!=0 && text[i]!=' ') continue;
		
		memcpy(tmp, text, i);
		tmp[i] = '\0';
		int w = 0;
		Font_getTextSize(font, tmp, &w, NULL);
		if (w>hw) {
			if (head) break; // found at least one word
			if (w>fw) {
				int j = i;
				while (j>0) {
					j -= 1;
					memcpy(tmp,text,j);
					tmp[j] = '\0';
					Font_getTextSize(font, tmp, &w, NULL);
					if (w<=fw) {
						head = j;
						break;
					}
				}
				break;
			}
			// else continue searching for a word break past the halfway
		}
		head = i;
	}
	
	// trim trailing space or dash
	while (WORDS_ONLY && head>0 && (text[head-1]==' ' || text[head-1]=='-')) head -= 1;
	
	memcpy(tmp, text, head);
	tmp[head] = '\0';
	int w = 0;
	Font_getTextSize(font, tmp, &w, NULL);

	hw = max_width - sw - w;
	if (hw<0) hw = 0;
	
	// tail
	for (int i=1; i<len-head; i++) {
		int start = len - i;
		if (WORDS_ONLY && start>0 && text[start-1]!=' ') continue;
		
		const char* trail = text + start;
		int w = 0;
		Font_getTextSize(font, trail, &w, NULL);
		if (w>hw) break;
		tail = i;
	}
	
	while (WORDS_ONLY && tail>0 && (text[len-tail]==' ' || text[len-tail]=='-')) tail -= 1;
	
	int i = 0;
	memcpy(out_text+i, text, head);
	i += head;
	memcpy(out_text+i, sep, slen);
	i += slen;
	if (tail) {
		memcpy(out_text+i, text+(len-tail), tail);
		i += tail;
	}
	out_text[i] = '\0';
}

static void App_selectCore(void) {
	Console *console = Console_for(game.path);
	if (!console) return;
	
	strcpy(core.name, console->core);
	sprintf(core.path, "%s/%s_libretro.so", CORES_PATH, core.name);
	
	LOG("core.path: %s", core.path);
	LOG("core.name: %s", core.name);
}

static void App_init(void) {
	LOG("App_init");
	
	Menu_quit();
	
	app.count = 0;
	app.current = 0;
	app.capacity = 16;
	app.items = malloc(sizeof(Entry) * app.capacity);
	
	char* dirs[] = {
		GAMES_PATH,
		ARCHIVE_PATH,
	};
	
	for (int i=0; i<(int)NUMBER_OF(dirs); i++) {
		DIR* dir = opendir(dirs[i]);
		if (dir) {
			struct dirent* entry;
			while ((entry=readdir(dir))!=NULL) {
				if (entry->d_name[0]=='.') continue;
				if (entry->d_type==DT_DIR) continue;
			
				if (app.count>=app.capacity) {
					app.capacity *= 2;
					app.items = realloc(app.items, sizeof(Entry) * app.capacity);
				}
				Entry* item = &app.items[app.count++];
				item->hidden = i;
				snprintf(item->name, MAX_FILE, "%s", entry->d_name);
			}
			closedir(dir);
		}
	}
	
	if (app.count) qsort(app.items, app.count, sizeof(Entry), App_sort);
	
	// get index of last played game
	if (*settings.game) {
		for (int i=0; i<app.count; i++) {
			if (strcmp(app.items[i].name, settings.game)==0) {
				app.current = i;
				break;
			}
		}
	}
	
	// if there is no current and the first game is hidden
	if (app.count>0 && app.items[app.current].hidden) {
		// try to find a non-hidden game
		app.current = App_next(app.current, 1);
	}
	// TODO: tmp cycle
	// app.current = App_next(app.current, 1);
	
	App_set(app.current);
}
static void App_quit(void) {
	if (app.reload) {
		app.reload = 0;
		App_set(app.next);
	}
	free(app.items);
}

static void App_save(void) {
	State_write();
}
static void App_load(void) {
	State_read();
}

static void App_menu(void) {
	SDL_SaveBMP(framebuffer, SCREENSHOTS_PATH "/current.bmp");
	
	SDL_Surface *preview = NULL;
	
	if (ui.osd==OSD_NONE) {
		SDL_FillRect(overlay, NULL, 0);
		dirty_overlay();
		present_layers(VSYNC_NONE);
		enable_overlay();
	}
	
	Menu_init();
	int top = 0;
	int rows = 10;
	int dirty = 1;
	int back = menu.current;
	while (!menu.quit) {
		Pad_update();
		if (App_listen()) dirty = 1;;
		
		if (menu.mode==MODE_MENU) {
			if (Pad_justPressed(PAD_UP)) {
				Menu_select(menu.selected - 1);
			}
			else if (Pad_justPressed(PAD_DOWN)) {
				Menu_select(menu.selected + 1);
			}
		
			if (Pad_justPressed(PAD_LEFT)) {
				Menu_switch(App_next(menu.current, -1));
			}
			else if (Pad_justPressed(PAD_RIGHT)) {
				Menu_switch(App_next(menu.current, 1));
			}
		
			if (Pad_justPressed(PAD_B)) {
				if (menu.current!=app.current) {
					Menu_switch(app.current);
				}
				else {
					Menu_quit();
				}
			}
		
			if (Pad_justPressed(PAD_A)) {
				MenuItem item = menu.items[menu.selected];
				if (menu.current==app.current) {
					if (item==ITEM_SAVE) {
						App_save();
					}
					else if (item==ITEM_LOAD) {
						App_load();
					}
					else if (item==ITEM_ARCHIVE) {
						LOG("change to archive (from current)");
						back = menu.current;
						menu.selected = menu.current;
						menu.mode = MODE_ARCHIVE;
						menu.dirty = 1;
					}
					else if (item==ITEM_RESET) {
						Core_reset();
					}
					if (item!=ITEM_ARCHIVE) Menu_quit();
				}
				else {
					if (item==ITEM_LOAD) {
						if (menu.current!=app.current) {
							app.next = menu.current;
							app.reload = 1;
							Menu_quit();
						}
					}
					else if (item==ITEM_ARCHIVE) {
						LOG("change to archive (from other)");
						back = menu.current;
						menu.selected = menu.current;
						menu.mode = MODE_ARCHIVE;
						menu.dirty = 1;
					}
				}
			}
		}
		else if (menu.mode==MODE_ARCHIVE) {
			if (Pad_justRepeated(PAD_UP)) { // ROW UP
				menu.current -= 1;
				if (menu.current<0) menu.current += app.count;
				menu.dirty = 1;
			}
			else if (Pad_justRepeated(PAD_DOWN)) { // ROW DOWN
				menu.current += 1;
				if (menu.current>=app.count) menu.current -= app.count;
				menu.dirty = 1;
			}
			
			if (Pad_justRepeated(PAD_RIGHT)) { // PAGE FORWARD
				if (menu.current==app.count-1) menu.current = 0;
				else {
					menu.current += rows;
					if (menu.current>=app.count) menu.current = app.count-1;
				}
				menu.dirty = 1;
			}
			else if (Pad_justRepeated(PAD_LEFT)) { // PAGE BACK
				if (menu.current==0) menu.current = app.count-1;
				else {
					menu.current -= rows;
					if (menu.current<0) menu.current = 0;
				}
				menu.dirty = 1;
			}
			
			if (Pad_justPressed(PAD_A)) {
				Entry* item = &app.items[menu.current];
				
				char shown[MAX_PATH];
				sprintf(shown, "%s/%s", GAMES_PATH, item->name);
				char hidden[MAX_PATH];
				sprintf(hidden, "%s/%s", ARCHIVE_PATH, item->name);
				
				char *dst,*src;
				if (item->hidden) {
					src = hidden;
					dst = shown;
				}
				else {
					src = shown;
					dst = hidden;
				}
				
				if (!exists(dst) && rename(src, dst)==0) {
					item->hidden = !item->hidden;
					menu.dirty = 1;
				}
			}
			
			if (Pad_justPressed(PAD_B)) {
				Menu_switch(back);
				menu.selected = back==app.current ? 2 : 1; // ARCHIVE
				menu.mode = MODE_MENU;
				menu.dirty = 1;
			}
		}
		
		if (menu.quit) break;
		
		if (menu.dirty) {
			dirty = 1;
			menu.dirty = 0;
		}
		
		if (dirty) {
			if (preview) {
				SDL_FreeSurface(preview);
				preview = NULL;
			}
			
			char game_name[MAX_FILE];
			strcpy(game_name, app.items[menu.current].name);
		
			Console *console = Console_for(game_name); // requires extension
			char *dot = strrchr(game_name, '.');
			if (dot && dot!=game_name) *dot = '\0';

			if (!preview) {
				char path[MAX_PATH];
				if (menu.mode==MODE_MENU) {
					MenuItem item = menu.items[menu.selected]; // won't exist in archive!
			
					if (menu.current==app.current) {
						if (item==ITEM_LOAD) State_getPreviewPath(path, game_name);
						else if (item!=ITEM_RESET) strcpy(path, SCREENSHOTS_PATH "/current.bmp");
					}
					else State_getPreviewPath(path, game_name);
					if (item==ITEM_RESET || !exists(path)) sprintf(path, ASSETS_PATH "/default-%s.png", console->slug);
				}
				else if (menu.mode==MODE_ARCHIVE) {
					if (menu.current==app.current) strcpy(path, SCREENSHOTS_PATH "/current.bmp");
					else State_getPreviewPath(path, game_name);
					if (!exists(path)) sprintf(path, ASSETS_PATH "/default-%s.png", console->slug);
				}
			
				// LOG("load preview: %s", path);
			
				// int w,h;
				// preview = DOS_LoadTexture(ui.renderer, path);
				// DOS_QueryTexture(preview, NULL, NULL, &w, &h);
				// preview_rect = UI_center(w,h);
			}
			// if (preview) DOS_RenderCopy(ui.renderer, preview, NULL, &preview_rect);
			UI_gradient(overlay);
		
			Font_shadowText(overlay, font12, console->name, 4,4, LIGHT_COLOR);
			UI_battery(PWR_getBatteryPercent(), 0, 1);

			// game name
			char name[MAX_FILE];
			App_getDisplayName(game_name, name);

			#define MAX_LINES 8
			char* lines[MAX_LINES];
			int splits[MAX_LINES] = {0};
			int line_count = App_wrap(font18, name, MAX_LINES, lines, splits);
			SDL_Color color = WHITE_COLOR;
			for (int i=0; i<line_count; i++) {
				if (splits[i]) color = LIGHT_COLOR;
				Font_shadowText(overlay, font18, lines[i], 4, 4+18+(i*18), color);
				free(lines[i]);
			}
		
			int x,y,w,h;
			int bottom = SCREEN_HEIGHT;
			if (menu.mode==MODE_MENU) {
				y = 0;
				h = SCREEN_HEIGHT;
		
				int mw = 0;
				for (int i=0; i<menu.count; i++) {
					Font_getTextSize(font12, Menu_getItemName(menu.items[i]), &w, NULL);
					if (w>mw) mw = w;
				}

				w = 4 + mw + 4;
				x = (SCREEN_WIDTH - w) / 2;

				int oh = ((menu.count-1) * 20) + 18;
				y += (h - oh) / 2;
				y += 18; // TODO: tmp, quick fix for too high menu
				h = oh;
				oh = 20;

				UI_rect(overlay, x-4,y-4,4+w+4,4+h+4, 0, TRIAD_ALPHA(BLACK_TRIAD,0x40));

				for (int i=0; i<menu.count; i++) {
					Font_renderFunc font_renderer = Font_shadowText;
					SDL_Color color = WHITE_COLOR;

					if (i==menu.selected) {
						UI_rect(overlay, x+1,y+(i*oh)+1,w,18, 0, TRIAD_ALPHA(BLACK_TRIAD,0xff));
						UI_rect(overlay, x,y+(i*oh),w,18, 0, TRIAD_ALPHA(WHITE_TRIAD,0xff));
						font_renderer = Font_renderText;
						color = DARK_COLOR;
					}

					font_renderer(overlay, font12, Menu_getItemName(menu.items[i]), x+4, y+4+(i*oh), color);
				}
			}
			else if (menu.mode==MODE_ARCHIVE) {
				bottom /= 2;
				// calculate viewport
				if (app.count<=rows) {
					top = 0;
				}
				else {
					int max = app.count - rows;
					int bottom = top + rows - 1;
					if (menu.current<top) top = menu.current;
					else if (menu.current>bottom) top = menu.current - (rows - 1);
					if (top<0) top = 0;
					if (top>max) top = max;
				}
				int end = top + rows;
				if (end>app.count) end = app.count;
	
				x = 0;
				y = bottom;
				h = 16;
			
				// TODO: fill black?
			
				// draw viewport
				for (int i=top; i<end; i++) {
					int row = i - top;
					int oy = y + row * h;
		
					Entry* item = &app.items[i];
					SDL_Color c = item->hidden ? LIGHT_COLOR : WHITE_COLOR;
					if (i==menu.current) {
						UI_rect(overlay, x,oy,SCREEN_WIDTH,h, 0, c);
						c = BLACK_COLOR;
					}
		
					char fit[MAX_FILE];
					App_getDisplayName(item->name, name);
					App_trunc(font12, name, SCREEN_WIDTH-(16+8), fit);
		
					// DOS_RenderCopy(ui.renderer, ui.icons, &(DOS_Rect){item->hidden?24:0,0,24,24}, &(DOS_Rect){x+4,oy+4,24,24});
					Font_renderText(overlay, font12, fit, x+16,oy+3, c);
				}
			}
		
			if (menu.mode==MODE_MENU) {
				if (ui.osd==OSD_BRIGHTNESS)	UI_OSD("BRIGHTNESS", settings.brightness, 10, bottom);
				else if (ui.osd==OSD_VOLUME)UI_OSD("VOLUME", settings.volume, 20, bottom);
			}
		
			dirty_overlay();
		}

		present_layers(fastforward ? VSYNC_NONE : VSYNC_WAIT);
	}
	
	if (ui.osd==OSD_NONE) disable_overlay();
	Pad_reset();
}
static int App_listen(void) {
	UI_update();
	
	static int ignore_menu = 0;
	if (Pad_justPressed(PAD_MENU)) {
		ignore_menu = 0;
	}
	else if (Pad_isPressed(PAD_MENU)) {
		if (Pad_justRepeated(PAD_UP)) {
			Pad_consume(PAD_UP);
			ignore_menu = 1;
			UI_setOSD(OSD_BRIGHTNESS);
			if (settings.brightness<10) {
				Settings_setBrightness(settings.brightness+1);
			}
		}
		else if (Pad_justRepeated(PAD_DOWN)) {
			Pad_consume(PAD_DOWN);
			ignore_menu = 1;
			UI_setOSD(OSD_BRIGHTNESS);
			if (settings.brightness>0) {
				Settings_setBrightness(settings.brightness-1);
			}
		}
		else if (Pad_justRepeated(PAD_LEFT)) {
			Pad_consume(PAD_LEFT);
			ignore_menu = 1;
			UI_setOSD(OSD_VOLUME);
			if (settings.volume>0) {
				Settings_setVolume(settings.volume-1);
			}
		}
		else if (Pad_justRepeated(PAD_RIGHT)) {
			Pad_consume(PAD_RIGHT);
			ignore_menu = 1;
			UI_setOSD(OSD_VOLUME);
			if (settings.volume<20) {
				Settings_setVolume(settings.volume+1);
			}
		}
		
		if (Pad_justPressed(PAD_R1)) {
			Pad_consume(PAD_R1);
			ignore_menu = 1;
			settings.frameskip = !settings.frameskip;
			core_options_dirty =1;
		}
		
		if (Pad_justPressed(PAD_START)) {
			Pad_consume(PAD_START);
			ignore_menu = 1;
			app.quit = 1;
		}
		if (Pad_justPressed(PAD_SELECT)) {
			Pad_consume(PAD_SELECT);
			ignore_menu = 1;
			app.next = app.current;
			app.reload = 1;
		}
	}
	else if (Pad_justReleased(PAD_MENU)) {
		if (!ignore_menu) {
			if (!ui.menu) App_menu();
			else Menu_quit();
		}
		else ignore_menu = 0;
	}
	
	return ignore_menu;
}
static void App_render(void) {
	
	if (ui.osd!=OSD_NONE) {
		SDL_FillRect(overlay, NULL, 0);
		if (ui.osd==OSD_BRIGHTNESS)	UI_OSD("BRIGHTNESS", settings.brightness, 10, SCREEN_HEIGHT);
		else if (ui.osd==OSD_VOLUME)UI_OSD("VOLUME", settings.volume, 20, SCREEN_HEIGHT);
		dirty_overlay();
	}

	present_layers(fastforward ? VSYNC_NONE : VSYNC_WAIT);
}

int main_OLD(void) {
	LOG_init();
	UI_init();
		
	int x = 0; // tmp
	
	// UI_gradient(overlay);
	// UI_battery(PWR_getBatteryPercent(), 0, 1);
	// Font_shadowText(overlay, font12, "Dedicated OS", 4,4, LIGHT_COLOR);
	// Font_shadowText(overlay, font18, "Game Boy Micro", 4,4+18, WHITE_COLOR);
	// dirty_overlay();
	
	int menu_combo = 0;
	int quit = 0;
	while (!quit) {
		App_listen();
		
		if (Pad_justPressed(PAD_MENU)) menu_combo = 0;
		if (menu_combo && Pad_justReleased(PAD_MENU)) Pad_consume(PAD_MENU);
			
		if (Pad_isPressed(PAD_MENU)) {
			if (Pad_justRepeated(PAD_UP)) {
				Pad_consume(PAD_UP);
				menu_combo = 1;
				UI_setOSD(OSD_BRIGHTNESS);
				if (settings.brightness<10) {
					Settings_setBrightness(settings.brightness+1);
				}
			}
			else if (Pad_justRepeated(PAD_DOWN)) {
				Pad_consume(PAD_DOWN);
				menu_combo = 1;
				UI_setOSD(OSD_BRIGHTNESS);
				if (settings.brightness>0) {
					Settings_setBrightness(settings.brightness-1);
				}
			}
			else if (Pad_justRepeated(PAD_LEFT)) {
				Pad_consume(PAD_LEFT);
				menu_combo = 1;
				UI_setOSD(OSD_VOLUME);
				if (settings.volume>0) {
					Settings_setVolume(settings.volume-1);
				}
			}
			else if (Pad_justRepeated(PAD_RIGHT)) {
				Pad_consume(PAD_RIGHT);
				menu_combo = 1;
				UI_setOSD(OSD_VOLUME);
				if (settings.volume<20) {
					Settings_setVolume(settings.volume+1);
				}
			}
		}
		
		if (Pad_justPressed(PAD_START)) quit = 1;
		
		x += 4;
		if (x>SCREEN_WIDTH) x -= SCREEN_WIDTH;

		UI_gradient(overlay);
		UI_battery(PWR_getBatteryPercent(), 0, 1);

		Font_shadowText(overlay, font12, "Dedicated OS", 4,4, LIGHT_COLOR);
		Font_shadowText(overlay, font18, "Game Boy Micro", 4,4+18, WHITE_COLOR);

		UI_fillRect(overlay, x,0,16,SCREEN_HEIGHT, RED_COLOR);
		UI_fillRect(overlay, x-SCREEN_WIDTH,0,16,SCREEN_HEIGHT, RED_COLOR);

		int bottom = SCREEN_HEIGHT;
		if (ui.osd==OSD_BRIGHTNESS)	UI_OSD("BRIGHTNESS", settings.brightness, 10, bottom);
		else if (ui.osd==OSD_VOLUME)UI_OSD("VOLUME", settings.volume, 20, bottom);

		dirty_overlay();
		
		SDL_FillRect(scaler, NULL, 0xff282828);
		UI_fillRect(scaler, SCREEN_WIDTH-x,0,16,SCREEN_HEIGHT, YELLOW_COLOR);
		UI_fillRect(scaler, SCREEN_WIDTH-(x-SCREEN_WIDTH),0,16,SCREEN_HEIGHT, YELLOW_COLOR);
		
		dirty_scaler();
		
		present_layers(VSYNC_WAIT);
	}
	
	UI_quit();
	return 0;
}

int main(void) {
	LOG_init();
	UI_init();
	
	while (!app.quit) {
		App_init();
		App_selectCore();
		
		Core_open();
		Game_open();

		while (!app.quit && !app.reload) {
			core.run();
			if (app.reload) break;
			App_render();
		}

		Game_close();
		Core_close();

		App_quit();
	}
	UI_quit();
	return 0;
}
