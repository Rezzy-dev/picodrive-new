/*
 * PicoDrive
 * (C) notaz, 2007,2008
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syslimits.h> // PATH_MAX

#include <pspthreadman.h>
#include <pspdisplay.h>
#include <psputils.h>
#include <pspgu.h>
#include <pspaudio.h>

#include "psp.h"
#include "emu.h"
#include "mp3.h"
#include "in_psp.h"
#include "asm_utils.h"
#include "../common/emu.h"
#include "../common/input_pico.h"
#include "platform/libpicofe/input.h"
#include "platform/libpicofe/menu.h"

#include <pico/pico_int.h>
#include <pico/cd/genplus_macros.h>
#include <pico/cd/cdd.h>
#include <pico/cd/cue.h>

#define OSD_FPS_X 432

int engineStateSuspend;

#define PICO_PEN_ADJUST_X 4
#define PICO_PEN_ADJUST_Y 2

struct Vertex
{
	short u,v;
	short x,y,z;
};

static struct Vertex __attribute__((aligned(4))) g_vertices[2];
static unsigned short __attribute__((aligned(16))) localPal[0x100];
static int need_pal_upload = 0;
static int fbimg_offs = 0;
static int h32_mode = 0;

static const struct in_default_bind in_psp_defbinds[] =
{
	{ PSP_CTRL_UP,          IN_BINDTYPE_PLAYER12, GBTN_UP },
	{ PSP_CTRL_DOWN,        IN_BINDTYPE_PLAYER12, GBTN_DOWN },
	{ PSP_CTRL_LEFT,        IN_BINDTYPE_PLAYER12, GBTN_LEFT },
	{ PSP_CTRL_RIGHT,       IN_BINDTYPE_PLAYER12, GBTN_RIGHT },
	{ PSP_CTRL_SQUARE,      IN_BINDTYPE_PLAYER12, GBTN_A },
	{ PSP_CTRL_CROSS,       IN_BINDTYPE_PLAYER12, GBTN_B },
	{ PSP_CTRL_CIRCLE,      IN_BINDTYPE_PLAYER12, GBTN_C },
	{ PSP_CTRL_START,       IN_BINDTYPE_PLAYER12, GBTN_START },
	{ PSP_CTRL_TRIANGLE,    IN_BINDTYPE_EMU, PEVB_SWITCH_RND },
	{ PSP_CTRL_LTRIGGER,    IN_BINDTYPE_EMU, PEVB_STATE_SAVE },
	{ PSP_CTRL_RTRIGGER,    IN_BINDTYPE_EMU, PEVB_STATE_LOAD },
	{ PSP_CTRL_SELECT,      IN_BINDTYPE_EMU, PEVB_MENU },
	{ 0, 0, 0 }
};


const char *renderer_names[] = { "16bit accurate", " 8bit accurate", " 8bit fast", NULL };
const char *renderer_names32x[] = { "accurate", "faster", "fastest", NULL };
enum renderer_types { RT_16BIT, RT_8BIT_ACC, RT_8BIT_FAST, RT_COUNT };

#define is_16bit_mode() \
	(get_renderer() == RT_16BIT || (PicoIn.AHW & PAHW_32X))

static int get_renderer(void)
{
	if (PicoIn.AHW & PAHW_32X)
		return currentConfig.renderer32x;
	else
		return currentConfig.renderer;
}

static void change_renderer(int diff)
{
	int *r;
	if (PicoIn.AHW & PAHW_32X)
		r = &currentConfig.renderer32x;
	else
		r = &currentConfig.renderer;
	*r += diff;

	if      (*r >= RT_COUNT)
		*r = 0;
	else if (*r < 0)
		*r = RT_COUNT - 1;
}

static void apply_renderer(void)
{
	PicoIn.opt &= ~POPT_ALT_RENDERER;
	PicoIn.opt |=  POPT_DIS_32C_BORDER;

	switch (currentConfig.renderer) {
	case RT_16BIT:
		PicoDrawSetOutFormat(PDF_RGB555, 0);
		break;
	case RT_8BIT_ACC:
		PicoDrawSetOutFormat(PDF_8BIT, 0);
		break;
	case RT_8BIT_FAST:
		PicoIn.opt |=  POPT_ALT_RENDERER;
		PicoDrawSetOutFormat(PDF_NONE, 0);
		break;
	}
}

static void osd_text(int x, const char *text, int is_active, int clear_all)
{
	unsigned short *screen = is_active ? psp_video_get_active_fb() : psp_screen;
	int len = clear_all ? (480 / 2) : (strlen(text) * 8 / 2);
	int *p, h;
	void *tmp = g_screen_ptr;
	for (h = 0; h < 8; h++) {
		p = (int *) (screen+x+512*(264+h));
		p = (int *) ((int)p & ~3); // align
		memset32_uncached(p, 0, len);
	}
	g_screen_ptr = screen; // nasty pointer tricks
	emu_text_out16(x, 264, text);
	g_screen_ptr = tmp;
}


static void set_scaling_params(void)
{
	int src_width, fbimg_width, fbimg_height, fbimg_xoffs, fbimg_yoffs, border_hack = 0;
	g_vertices[0].x = g_vertices[0].y =
	g_vertices[0].z = g_vertices[1].z = 0;

	fbimg_height = (int)(240.0 * currentConfig.scale + 0.5);
	if (!h32_mode) {
		fbimg_width = (int)(320.0 * currentConfig.scale * currentConfig.hscale40 + 0.5);
		src_width = 320;
	} else {
		fbimg_width = (int)(256.0 * currentConfig.scale * currentConfig.hscale32 + 0.5);
		src_width = 256;
	}

	if (fbimg_width  & 1) fbimg_width++;  // make even
	if (fbimg_height & 1) fbimg_height++;

	if (fbimg_width >= 480) {
		g_vertices[0].u = (fbimg_width-480)/2;
		g_vertices[1].u = src_width - (fbimg_width-480)/2 - 1;
		fbimg_width = 480;
		fbimg_xoffs = 0;
	} else {
		g_vertices[0].u = 0;
		g_vertices[1].u = src_width;
		fbimg_xoffs = 240 - fbimg_width/2;
	}
	if (fbimg_width > 320 && fbimg_width <= 480) border_hack = 1;

	if (fbimg_height >= 272) {
		g_vertices[0].v = (fbimg_height-272)/2;
		g_vertices[1].v = 240 - (fbimg_height-272)/2;
		fbimg_height = 272;
		fbimg_yoffs = 0;
	} else {
		g_vertices[0].v = 0;
		g_vertices[1].v = 240;
		fbimg_yoffs = 136 - fbimg_height/2;
	}

	g_vertices[1].x = fbimg_width;
	g_vertices[1].y = fbimg_height;
	if (fbimg_xoffs < 0) fbimg_xoffs = 0;
	if (fbimg_yoffs < 0) fbimg_yoffs = 0;
	if (border_hack) {
		g_vertices[0].u++;
		g_vertices[0].x++;
		g_vertices[1].u--;
		g_vertices[1].x--;
	}
	if (!is_16bit_mode()) {
		// 8-bit modes have an 8 px overlap area on the left
		g_vertices[0].u += 8;
		g_vertices[1].u += 8;
	}
	fbimg_offs = (fbimg_yoffs*512 + fbimg_xoffs) * 2; // dst is always 16bit

	/*
	lprintf("set_scaling_params:\n");
	lprintf("offs: %i, %i\n", fbimg_xoffs, fbimg_yoffs);
	lprintf("xy0, xy1: %i, %i; %i, %i\n", g_vertices[0].x, g_vertices[0].y, g_vertices[1].x, g_vertices[1].y);
	lprintf("uv0, uv1: %i, %i; %i, %i\n", g_vertices[0].u, g_vertices[0].v, g_vertices[1].u, g_vertices[1].v);
	*/
}

static void do_pal_update(void)
{
	unsigned int *dpal=(void *)localPal;
	int i;

	//for (i = 0x3f/2; i >= 0; i--)
	//	dpal[i] = ((spal[i]&0x000f000f)<< 1)|((spal[i]&0x00f000f0)<<3)|((spal[i]&0x0f000f00)<<4);
	if (PicoIn.opt & POPT_ALT_RENDERER) {
		do_pal_convert(localPal, PicoMem.cram, currentConfig.gamma, currentConfig.gamma2);
		Pico.m.dirtyPal = 0;
	}
	else if (Pico.est.rendstatus & PDRAW_SONIC_MODE)
	{
		switch (Pico.est.SonicPalCount) {
		case 3: do_pal_convert(localPal+0xc0/2, Pico.est.SonicPal+0xc0, currentConfig.gamma, currentConfig.gamma2);
		case 2: do_pal_convert(localPal+0x80/2, Pico.est.SonicPal+0x80, currentConfig.gamma, currentConfig.gamma2);
		case 1:	do_pal_convert(localPal+0x40/2, Pico.est.SonicPal+0x40, currentConfig.gamma, currentConfig.gamma2);
		default:do_pal_convert(localPal, Pico.est.SonicPal, currentConfig.gamma, currentConfig.gamma2);
		}
	}
	else if (Pico.video.reg[0xC]&8) // shadow/hilight?
	{
		do_pal_convert(localPal, Pico.est.SonicPal, currentConfig.gamma, currentConfig.gamma2);
		// shadowed pixels
		for (i = 0; i < 0x40 / 2; i++) {
			dpal[0xc0/2 + i] = dpal[i];
			dpal[0x80/2 + i] = (dpal[i] >> 1) & 0x738e738e;
		}
		// hilighted pixels
		for (i = 0; i < 0x40 / 2; i++) {
			u32 t = ((dpal[i] >> 1) & 0x738e738e) + 0x738e738e;
			t |= (t >> 4) & 0x08610861;
			dpal[0x40/2 + i] = t;
		}
	}
 	else
 	{
		do_pal_convert(localPal, Pico.est.SonicPal, currentConfig.gamma, currentConfig.gamma2);
		memcpy((int *)dpal+0x40/2, (void *)localPal, 0x40*2);
		memcpy((int *)dpal+0x80/2, (void *)localPal, 0x80*2);
	}
	localPal[0xe0] = 0;
	localPal[0xf0] = 0x001f;

	if (Pico.m.dirtyPal == 2)
		Pico.m.dirtyPal = 0;
	need_pal_upload = 1;
}

static void blitscreen_clut(void)
{
	int offs = fbimg_offs;
	offs += (psp_screen == VRAM_FB0) ? VRAMOFFS_FB0 : VRAMOFFS_FB1;

	sceGuSync(0,0); // sync with prev
	sceGuStart(GU_DIRECT, guCmdList);
	sceGuDrawBuffer(GU_PSM_5650, (void *)offs, 512); // point to back buffer
	sceGuTexMode(is_16bit_mode() ? GU_PSM_5650:GU_PSM_T8,0,0,0);
	sceGuTexImage(0,512,512,512,g_screen_ptr);

	if (Pico.m.dirtyPal)
		do_pal_update();

	sceKernelDcacheWritebackAll();

	if (need_pal_upload) {
		need_pal_upload = 0;
		sceGuClutLoad((256/8), localPal); // upload 32*8 entries (256)
	}

#if 1
	if (g_vertices[0].u == 0 && g_vertices[1].u == g_vertices[1].x)
	{
		struct Vertex* vertices;
		int x;

		#define SLICE_WIDTH 32
		for (x = 0; x < g_vertices[1].x; x += SLICE_WIDTH)
		{
			// render sprite
			vertices = (struct Vertex*)sceGuGetMemory(2 * sizeof(struct Vertex));
			memcpy(vertices, g_vertices, 2 * sizeof(struct Vertex));
			vertices[0].u = vertices[0].x = x;
			vertices[1].u = vertices[1].x = x + SLICE_WIDTH;
			sceGuDrawArray(GU_SPRITES,GU_TEXTURE_16BIT|GU_VERTEX_16BIT|GU_TRANSFORM_2D,2,0,vertices);
		}
		// lprintf("listlen: %iB\n", sceGuCheckList()); // ~480 only
	}
	else
#endif
		sceGuDrawArray(GU_SPRITES,GU_TEXTURE_16BIT|GU_VERTEX_16BIT|GU_TRANSFORM_2D,2,0,g_vertices);

	sceGuFinish();
}


static void cd_leds(void)
{
	unsigned int reg, col_g, col_r, *p;

	reg = Pico_mcd->s68k_regs[0];

	p = (unsigned int *)((short *)psp_screen + 512*2+4+2);
	col_g = (reg & 2) ? 0x06000600 : 0;
	col_r = (reg & 1) ? 0x00180018 : 0;
	*p++ = col_g; *p++ = col_g; p+=2; *p++ = col_r; *p++ = col_r; p += 512/2 - 12/2;
	*p++ = col_g; *p++ = col_g; p+=2; *p++ = col_r; *p++ = col_r; p += 512/2 - 12/2;
	*p++ = col_g; *p++ = col_g; p+=2; *p++ = col_r; *p++ = col_r;
}

static void draw_pico_ptr(void)
{
	unsigned char *p = (unsigned char *)g_screen_ptr + 8;

	// only if pen enabled and for 8bit mode
	if (pico_inp_mode == 0 || is_16bit_mode()) return;

	p += 512 * (pico_pen_y + PICO_PEN_ADJUST_Y);
	p += pico_pen_x + PICO_PEN_ADJUST_X;
	p[  -1] = 0xe0; p[   0] = 0xf0; p[   1] = 0xe0;
	p[ 511] = 0xf0; p[ 512] = 0xf0; p[ 513] = 0xf0;
	p[1023] = 0xe0; p[1024] = 0xf0; p[1025] = 0xe0;
}


// clears whole screen or just the notice area (in all buffers)
static void clearArea(int full)
{
	void *fb = psp_video_get_active_fb();

	if (full) {
		memset32_uncached(psp_screen, 0, 512*272*2/4);
		memset32_uncached(fb, 0, 512*272*2/4);
		memset32(VRAM_CACHED_STUFF, 0xe0e0e0e0, 512*240/4);
		memset32((int *)VRAM_CACHED_STUFF+512*240/4, 0, 512*240*2/4);
	} else {
		memset32_uncached((int *)((char *)psp_screen + 512*264*2), 0, 512*8*2/4);
		memset32_uncached((int *)((char *)fb         + 512*264*2), 0, 512*8*2/4);
	}
}

static void vidResetMode(void)
{
	// setup GU
	sceGuSync(0,0); // sync with prev
	sceGuStart(GU_DIRECT, guCmdList);

	sceGuClutMode(GU_PSM_5650,0,0xff,0);
	sceGuTexFunc(GU_TFX_REPLACE,GU_TCC_RGB);
	if (currentConfig.scaling)
	     sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	else sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexScale(1.0f,1.0f);
	sceGuTexOffset(0.0f,0.0f);

	Pico.m.dirtyPal = 1;

	sceGuFinish();
	set_scaling_params();
	sceGuSync(0,0);
}

/* sound stuff */
#define SOUND_BLOCK_SIZE_NTSC (1470*2) // 1024 // 1152
#define SOUND_BLOCK_SIZE_PAL  (1764*2)
#define SOUND_BLOCK_COUNT    8

static short __attribute__((aligned(4))) sndBuffer[SOUND_BLOCK_SIZE_PAL*SOUND_BLOCK_COUNT + 44100/50*2];
static short *snd_playptr = NULL, *sndBuffer_endptr = NULL;
static int samples_made = 0, samples_done = 0, samples_block = 0;
static int sound_thread_exit = 0;
static SceUID sound_sem = -1;

static void writeSound(int len);

static int sound_thread(SceSize args, void *argp)
{
	int ret = 0;

	lprintf("sthr: started, priority %i\n", sceKernelGetThreadCurrentPriority());

	while (!sound_thread_exit)
	{
		if (samples_made - samples_done < samples_block) {
			// wait for data (use at least 2 blocks)
			//lprintf("sthr: wait... (%i)\n", samples_made - samples_done);
			while (samples_made - samples_done <= samples_block*2 && !sound_thread_exit)
				ret = sceKernelWaitSema(sound_sem, 1, 0);
			if (ret < 0) lprintf("sthr: sceKernelWaitSema: %i\n", ret);
			continue;
		}

		// lprintf("sthr: got data: %i\n", samples_made - samples_done);

		ret = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, snd_playptr);

		samples_done += samples_block;
		snd_playptr  += samples_block;
		if (snd_playptr >= sndBuffer_endptr)
			snd_playptr = sndBuffer;
		// 1.5 kernel returns 0, newer ones return # of samples queued
		if (ret < 0)
			lprintf("sthr: sceAudioSRCOutputBlocking: %08x; pos %i/%i\n", ret, samples_done, samples_made);

		// shouln't happen, but just in case
		if (samples_made - samples_done >= samples_block*3) {
			//lprintf("sthr: block skip (%i)\n", samples_made - samples_done);
			samples_done += samples_block; // skip
			snd_playptr  += samples_block;
		}

	}

	lprintf("sthr: exit\n");
	sceKernelExitDeleteThread(0);
	return 0;
}

static void sound_init(void)
{
	SceUID thid;
	int ret;

	sound_sem = sceKernelCreateSema("sndsem", 0, 0, 1, NULL);
	if (sound_sem < 0) lprintf("sceKernelCreateSema() failed: %i\n", sound_sem);

	samples_made = samples_done = 0;
	samples_block = SOUND_BLOCK_SIZE_NTSC; // make sure it goes to sema
	sound_thread_exit = 0;
	thid = sceKernelCreateThread("sndthread", sound_thread, 0x12, 0x10000, 0, NULL);
	if (thid >= 0)
	{
		ret = sceKernelStartThread(thid, 0, 0);
		if (ret < 0) lprintf("sound_init: sceKernelStartThread returned %08x\n", ret);
	}
	else
		lprintf("sceKernelCreateThread failed: %i\n", thid);
}

void pemu_sound_start(void)
{
	static int PsndRate_old = 0, PicoOpt_old = 0, pal_old = 0;
	static int mp3_init_done;
	int ret, stereo;

	samples_made = samples_done = 0;

	if (PicoIn.AHW & PAHW_MCD) {
		// mp3...
		if (!mp3_init_done) {
			ret = mp3_init();
			mp3_init_done = 1;
			if (ret) emu_status_msg("mp3 init failed (%i)", ret);
		}
	}

	if (PicoIn.sndRate != PsndRate_old || (PicoIn.opt&0x0b) != (PicoOpt_old&0x0b) || Pico.m.pal != pal_old) {
		PsndRerate(Pico.m.frame_count ? 1 : 0);
	}
	stereo=(PicoIn.opt&8)>>3;

	samples_block = Pico.m.pal ? SOUND_BLOCK_SIZE_PAL : SOUND_BLOCK_SIZE_NTSC;
	if (PicoIn.sndRate <= 22050) samples_block /= 2;
	sndBuffer_endptr = &sndBuffer[samples_block*SOUND_BLOCK_COUNT];

	lprintf("starting audio: %i, len: %i, stereo: %i, pal: %i, block samples: %i\n",
			PicoIn.sndRate, Pico.snd.len, stereo, Pico.m.pal, samples_block);

	// while (sceAudioOutput2GetRestSample() > 0) psp_msleep(100);
	// sceAudioSRCChRelease();
	ret = sceAudioSRCChReserve(samples_block/2, PicoIn.sndRate, 2); // seems to not need that stupid 64byte alignment
	if (ret < 0) {
		lprintf("sceAudioSRCChReserve() failed: %i\n", ret);
		emu_status_msg("sound init failed (%i), snd disabled", ret);
		currentConfig.EmuOpt &= ~EOPT_EN_SOUND;
	} else {
		PicoIn.writeSound = writeSound;
		memset32((int *)(void *)sndBuffer, 0, sizeof(sndBuffer)/4);
		snd_playptr = sndBuffer_endptr - samples_block;
		samples_made = samples_block; // send 1 empty block first..
		PicoIn.sndOut = sndBuffer;
		PsndRate_old = PicoIn.sndRate;
		PicoOpt_old  = PicoIn.opt;
		pal_old = Pico.m.pal;
	}
}

void pemu_sound_stop(void)
{
	int i;
	if (samples_done == 0)
	{
		// if no data is written between sceAudioSRCChReserve and sceAudioSRCChRelease calls,
		// we get a deadlock on next sceAudioSRCChReserve call
		// so this is yet another workaround:
		memset32((int *)(void *)sndBuffer, 0, samples_block*4/4);
		samples_made = samples_block * 3;
		sceKernelSignalSema(sound_sem, 1);
	}
	sceKernelDelayThread(100*1000);
	samples_made = samples_done = 0;
	for (i = 0; sceAudioOutput2GetRestSample() > 0 && i < 16; i++)
		psp_msleep(100);
	sceAudioSRCChRelease();
}

/* wait until we can write more sound */
void pemu_sound_wait(void)
{
	// TODO: test this
	while (!sound_thread_exit && samples_made - samples_done > samples_block * 4)
		psp_msleep(10);
}

static void sound_deinit(void)
{
	sound_thread_exit = 1;
	sceKernelSignalSema(sound_sem, 1);
	sceKernelDeleteSema(sound_sem);
	sound_sem = -1;
}

static void writeSound(int len)
{
	int ret;

	PicoIn.sndOut += len / 2;
	/*if (PicoIn.sndOut > sndBuffer_endptr) {
		memcpy((int *)(void *)sndBuffer, (int *)endptr, (PicoIn.sndOut - endptr + 1) * 2);
		PicoIn.sndOut = &sndBuffer[PicoIn.sndOut - endptr];
		lprintf("mov\n");
	}
	else*/
	if (PicoIn.sndOut > sndBuffer_endptr) lprintf("snd oflow %i!\n", PicoIn.sndOut - sndBuffer_endptr);
	if (PicoIn.sndOut >= sndBuffer_endptr)
		PicoIn.sndOut = sndBuffer;

	// signal the snd thread
	samples_made += len / 2;
	if (samples_made - samples_done > samples_block*2) {
		// lprintf("signal, %i/%i\n", samples_done, samples_made);
		ret = sceKernelSignalSema(sound_sem, 1);
		//if (ret < 0) lprintf("snd signal ret %08x\n", ret);
	}
}


/* set default configuration values */
void pemu_prep_defconfig(void)
{
	defaultConfig.s_PsndRate = 22050;
	defaultConfig.s_PicoCDBuffers = 64;
	defaultConfig.CPUclock = 333;
	defaultConfig.scaling = 1;     // bilinear filtering for psp
	defaultConfig.scale = 1.20;    // fullscreen
	defaultConfig.hscale40 = 1.25;
	defaultConfig.hscale32 = 1.56;
}

/* check configuration for inconsistencies */
void pemu_validate_config(void)
{
	if (currentConfig.CPUclock < 33 || currentConfig.CPUclock > 333)
		currentConfig.CPUclock = 333;
}

/* finalize rendering a frame */
void pemu_finalize_frame(const char *fps, const char *notice)
{
	int emu_opt = currentConfig.EmuOpt;

	if (PicoIn.opt&POPT_ALT_RENDERER)
	{
		int i;
		unsigned char *pd;

		// clear top and bottom trash
		for (pd = Pico.est.Draw2FB, i = 8; i > 0; i--, pd += 512)
			memset32((int *)pd, 0xe0e0e0e0, 320/4);
		for (pd = Pico.est.Draw2FB+512*232, i = 8; i > 0; i--, pd += 512)
			memset32((int *)pd, 0xe0e0e0e0, 320/4);
	}

	if (PicoIn.AHW & PAHW_PICO)
		draw_pico_ptr();

	blitscreen_clut();

	// XXX move this to flip to give texture renderer more time?
	if (notice)      osd_text(4, notice, 0, 0);
	if (emu_opt & 2) osd_text(OSD_FPS_X, fps, 0, 0);

	if ((emu_opt & 0x400) && (PicoIn.AHW & PAHW_MCD))
		cd_leds();
}

/* FIXME: move plat_* to plat? */

void plat_debug_cat(char *str)
{
}

/* platform dependend emulator initialization */
void plat_init(void)
{
	flip_after_sync = 1;
	in_psp_init(in_psp_defbinds);
	in_probe();
	sound_init();
	plat_get_data_dir(rom_fname_loaded, sizeof(rom_fname_loaded));
}

/* platform dependend emulator deinitialization */
void plat_finish(void)
{
	sound_deinit();
}

/* display emulator status messages before holding emulation */
void plat_status_msg_busy_first(const char *msg)
{
	plat_status_msg_busy_next(msg);
}

void plat_status_msg_busy_next(const char *msg)
{
	plat_status_msg_clear();
	pemu_finalize_frame("", msg);
	plat_video_flip();
	emu_status_msg("");
	reset_timing = 1;
}

/* clear status message area */
void plat_status_msg_clear(void)
{
	clearArea(0);
}

/* change the audio volume setting */
void plat_update_volume(int has_changed, int is_up)
{
}

/* prepare for MD screen mode change */
void emu_video_mode_change(int start_line, int line_count, int is_32cols)
{
	h32_mode = is_32cols;
	vidResetMode();
	if (h32_mode)	// clear borders from h40 remnants
		clearArea(1);
}

/* render one frame in RGB */
void pemu_forced_frame(int no_scale, int do_emu)
{
	PicoIn.opt &= ~POPT_DIS_32C_BORDER;
	Pico.m.dirtyPal = 1;

	if (!no_scale)
		no_scale = currentConfig.scaling == EOPT_SCALE_NONE;
	emu_cmn_forced_frame(no_scale, do_emu);
}

/* change the platform output rendering */
void plat_video_toggle_renderer(int change, int is_menu_call)
{
	change_renderer(change);

	if (is_menu_call)
		return;

	apply_renderer();
	vidResetMode();
	rendstatus_old = -1;

	if (PicoIn.AHW & PAHW_32X)
		emu_status_msg(renderer_names32x[get_renderer()]);
	else
		emu_status_msg(renderer_names[get_renderer()]);
}

/* set the buffer for emulator output rendering */
void plat_video_set_buffer(void *buf)
{
	if (currentConfig.renderer == RT_16BIT || (PicoIn.AHW & PAHW_32X))
		PicoDrawSetOutBuf(g_screen_ptr, g_screen_ppitch * 2);
	else
		PicoDrawSetOutBuf(g_screen_ptr, g_screen_ppitch);
}

/* prepare for emulator output rendering */
void plat_video_loop_prepare(void) 
{
	apply_renderer();
	vidResetMode();
	clearArea(1);
}

/* prepare for entering the emulator loop */
void pemu_loop_prep(void)
{
}

/* terminate the emulator loop */
void pemu_loop_end(void)
{
	pemu_sound_stop();
}

// TODO 
void emu_handle_resume(void)
{
	if (!(PicoIn.AHW & PAHW_MCD)) return;

	// reopen first CD track
	if (cdd.toc.tracks[0].fd != NULL)
	{
		char *fname = rom_fname_reload;
		int len = strlen(rom_fname_reload);
		cue_data_t *cue_data = NULL;

		if (len > 4 && strcasecmp(fname + len - 4,  ".cue") == 0)
		{
			cue_data = cue_parse(rom_fname_reload);
			if (cue_data != NULL)
				fname = cue_data->tracks[1].fname;
		}

		lprintf("emu_HandleResume: reopen %s\n", fname);
		pm_close(cdd.toc.tracks[0].fd);
		cdd.toc.tracks[0].fd = pm_open(fname);
		lprintf("reopen %s\n", cdd.toc.tracks[0].fd != NULL ? "ok" : "failed");

		if (cue_data != NULL) cue_destroy(cue_data);
	}

	mp3_reopen_file();

#if 0	// TODO
	if (!(Pico_mcd->s68k_regs[0x36] & 1)/* && (Pico_mcd->scd.Status_CDC & 1)*/)
		cdd_change_track(cdd.index, cdd.lba);
#endif
}

