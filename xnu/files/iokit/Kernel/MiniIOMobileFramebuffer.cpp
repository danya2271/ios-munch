/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). A "swap" copies the pixels of the surfaces of its layers into that memory, and
 * "vsync" is a timer. Panel blanking uses DCS commands but does not reconfigure scanout.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/kernel_mach_header.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMessage.h>

#include "MiniIOSurface.h"
#include "munch-sde.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
extern "C" void loader_log_line(const char *line);
extern "C" void hakios_cpufreq_set_screen_on(uint32_t on);
extern "C" lck_mtx_t hakios_smmu_lock;
extern "C" uint64_t kvtophys(vm_offset_t);
extern "C" vm_offset_t ml_static_ptovirt(vm_offset_t);

#define MUNCH_DSI_CTRL_BASE             0x0ae94000
#define MUNCH_DSI_CTRL_SIZE             0x400
#define MUNCH_DSI_CTRL                  0x004
#define MUNCH_DSI_STATUS                0x008
#define MUNCH_DSI_COMMAND_MODE_DMA_CTRL 0x03c
#define MUNCH_DSI_DMA_CMD_LENGTH        0x04c
#define MUNCH_DSI_DMA_FIFO_CTRL         0x050
#define MUNCH_DSI_CMD_DMA_SW_TRIGGER    0x090
#define MUNCH_DSI_INT_CTRL              0x110
#define MUNCH_DSI_TPG_CTRL              0x15c
#define MUNCH_DSI_TPG_DMA_INIT          0x17c
#define MUNCH_DSI_TPG_DMA_FIFO_RESET    0x1ec
#define MUNCH_DSI_CMD_DMA_DONE          (1u << 0)
#define MUNCH_DSI_CMD_DMA_DONE_MASK     (1u << 1)
#define MUNCH_DSI_ERROR_STATUS          (1u << 24)
#define MUNCH_DSI_CMD_MODE_MDP_BUSY     (1u << 2)
#define MUNCH_DISPLAY_GCC_BASE          0x0010b000
#define MUNCH_DISPLAY_CC_BASE           0x0af02000
#define MUNCH_DISPLAY_POWER_BASE        0x0af03000

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */
#define kFBNativeRefreshHz      60                      /* native handoff defaults to the inherited 60-Hz mode */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_GPU_FRAME_MS       50
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=1: use bilinear filtering. The default nearest-neighbour path is substantially
 * cheaper on the CPU; fb_filter=1 restores the quality-first behavior for comparison. On the normal
 * 1125x2436-to-1080x2400 panel, nearest neighbour leaves out 45 columns and 36 rows.
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 0);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gNativeRefreshHz, "munch_sde_hz", kFBNativeRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 0);
static TUNABLE(uint32_t, gMunchSDEProbe, "munch_sde_probe", 0);
static TUNABLE(uint32_t, gMunchSDEPresent, "munch_sde", 0);
static TUNABLE(uint32_t, gMunchSDENative, "munch_sde_native", 0);
static TUNABLE(uint32_t, gMunchSDETakeover, "munch_sde_takeover", 0);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Eight by default on the eight-CPU boot; lower it only for comparison.
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 8);
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/hakios.input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static inline uint32_t
refreshHz(void)
{
	if (gMunchSDENative) return gNativeRefreshHz == 120 ? 120 : kFBNativeRefreshHz;
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;
struct MunchSDE;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	uint64_t        fStatSDEAcquire, fStatSDECompose, fStatSDECommit, fStatSDECompletion;
	uint32_t        fStatSDEPreserved, fStatSDEOverlapped, fStatSDERetired, fStatSDEQueued, fStatSDEFullCompose;
	uint32_t        fStatGPUBudgetMisses;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService * whatDevice) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	IOReturn present(const uint8_t * swap);
	IOReturn submitSwap(const void *bytes, uint32_t size);
	IOReturn waitSwap(uint32_t swapID);
	void finishSwap(uint32_t swapID, IOReturn result);
	bool presentationTime(uint64_t *count, uint64_t *time);
	MunchSDE *fSDE;
	bool fSDERequested;
	bool fSDENativeRequested;
	bool fDisplayBlanked;
	IOReturn fSDEError;
	enum munch_sde_result fSDEInitResult;
	const char *fSDEInitPhase;
	uint64_t fSDESwapRequests, fSDEAcceptedRequests, fSDEStatusTime;
	uint32_t fSDELastSwapID, fSDELastSwapSize;
	IOReturn fSDELastSwapResult;
	bool fSDEStatusLogged;
	void reportSDEStatus(uint64_t now);
	struct { uint32_t id; IOReturn result; bool pending, valid; } fSwapResults[64];
	bool fSDEInFlight;
	uint32_t fSDEInFlightID;
	uint8_t fSDEInFlightSwap[SWAP_SIZE];
	MiniIOSurface *fSDEInFlightSurfaces[SWAP_LAYERS];
	uint64_t fSDEInFlightBegan, fSDEInFlightReady, fSDEInFlightShown, fSDEInFlightSubmitted;
	uint64_t fHardwareFrames, fHardwareTime;
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void retireNativeSwap(uint32_t swapID, MiniIOSurface ** surfaces);
	void finishNativeSwapInOrder(uint32_t swapID, IOReturn result);
	enum munch_sde_result waitNativeFrame(bool allowQueued = false);
	void reportNativeFailure(enum munch_sde_result result, IOReturn swapResult, uint32_t swapID);
	void completeNativeSwap(enum munch_sde_result result);
	void dropNativeSurfaces(MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void reportSDEPerformance(uint64_t now);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	IOReturn setDisplayPower(uint32_t state);
	IOReturn sendDcsCommand(uint8_t command, uint8_t parameter, bool hasParameter);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	IOLock *        fDisplayLock;
	volatile uint32_t * fDSIRegs;
	volatile uint32_t * fDisplayGCC;
	volatile uint32_t * fDisplayCC;
	volatile uint32_t * fDisplayPower;
	bool            fPanelOn;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
	IOReturn changeRefresh(uint32_t refresh_hz);
};

static IOPMPowerState gFramebufferPowerStates[2] = {
	{ 1, kIOPMPowerOn, kIOPMDeviceUsable | kIOPMMaxPerformance, kIOPMPowerOn, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
munchSmmuProbe(void)
{
	vm_offset_t smmuAddress, smmuSecondAddress;
	volatile uint32_t *smmu, *smmuSecond;
	uint32_t scr0, idr0, idr1, matches, contexts;
	uint32_t streamMask = 0x7fff;
	uint32_t requestedSid = MUNCH_SDE_IOMMU_UNSECURE_SID;
	uint32_t requestedMask = MUNCH_SDE_IOMMU_UNSECURE_MASK;
	uint32_t pageSize, pages;
	bool found = false;

	lck_mtx_lock(&hakios_smmu_lock);
	smmuAddress = ml_io_map(MUNCH_APPS_SMMU_PHYS, 0x2000);
	if (smmuAddress == 0) {
		IOLog("MUNCH SMMU: MMIO map unavailable; SDE probe skipped\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return;
	}
	smmu = (volatile uint32_t *)smmuAddress;
	scr0 = smmu[MUNCH_SMMU_SCR0 / 4];
	idr0 = smmu[MUNCH_SMMU_IDR0 / 4];
	idr1 = smmu[MUNCH_SMMU_IDR1 / 4];
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	pageSize = (idr1 & (1u << 31)) ? 0x10000 : 0x1000;
	pages = 2u << ((idr1 >> 28) & 7);
	smmuSecondAddress = ml_io_map(MUNCH_APPS_SMMU_PHYS + pageSize, pageSize);
	if (smmuSecondAddress == 0) {
		IOLog("MUNCH SMMU: second global register page unavailable\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return;
	}
	smmuSecond = (volatile uint32_t *)smmuSecondAddress;
	if (scr0 & MUNCH_SMMU_SCR0_EXIDENABLE) {
		if (!(idr0 & MUNCH_SMMU_IDR0_EXIDS)) {
			IOLog("MUNCH SMMU: inconsistent extended SID mode (SCR0=%#x IDR0=%#x)\n", scr0, idr0);
			lck_mtx_unlock(&hakios_smmu_lock);
			return;
		}
		streamMask = 0xffff;
	}
	IOLog("MUNCH SMMU: read-only SID=%#x mask=%#x SCR0=%#x IDR0=%#x IDR1=%#x SMRs=%u CBs=%u\n",
	    requestedSid, requestedMask, scr0, idr0, idr1, matches, contexts);
	if (scr0 & 1) {
		IOLog("MUNCH SMMU: global configuration reports disabled\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return;
	}
	struct munch_smmu_stream *streams = matches ?
	    (struct munch_smmu_stream *)IOMallocZero(matches * sizeof(*streams)) : NULL;
	if (!streams) {
		IOLog("MUNCH SMMU: no stream snapshot storage; qualification skipped\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return;
	}
	for (uint32_t index = 0; index < matches; index++) {
		uint32_t smr = smmu[MUNCH_SMMU_SMR(index) / 4];
		uint32_t s2cr = smmu[MUNCH_SMMU_S2CR(index) / 4];
		streams[index].smr = smr;
		streams[index].s2cr = s2cr;
		uint32_t sid = smr & streamMask;
		uint32_t mask = (smr >> 16) & streamMask;
		bool active = (scr0 & MUNCH_SMMU_SCR0_EXIDENABLE) ?
		    (s2cr & MUNCH_SMMU_S2CR_EXIDVALID) != 0 : (smr & MUNCH_SMMU_SMR_VALID) != 0;

		if (((sid ^ requestedSid) & ~(mask | requestedMask) & streamMask) != 0) continue;
		found = true;
		uint32_t type = (s2cr >> MUNCH_SMMU_S2CR_TYPE_SHIFT) & 3;
		uint32_t context = s2cr & MUNCH_SMMU_CBNDX_MASK;
		IOLog("MUNCH SMMU: match%u active=%u SMR=%#x S2CR=%#x type=%u CB=%u\n",
		    index, active, smr, s2cr, type, context);
		if (!active || type != 0 || context >= contexts) continue;
		uint32_t cbar = smmuSecond[MUNCH_SMMU_CBAR(context) / 4];
		uint32_t cba2r = smmuSecond[MUNCH_SMMU_CBA2R(context) / 4];
		uint64_t contextOffset = (uint64_t)(pages + context) * pageSize;
		if (contextOffset + 0x1000 > MUNCH_APPS_SMMU_WINDOW_SIZE) {
			IOLog("MUNCH SMMU: CB%u register window %#llx exceeds mapped SMMU range\n", context,
			    (unsigned long long)contextOffset);
			continue;
		}
		vm_offset_t contextAddress = ml_io_map(MUNCH_APPS_SMMU_PHYS + contextOffset, 0x1000);
		if (contextAddress == 0) {
			IOLog("MUNCH SMMU: CB%u context register map unavailable\n", context);
			continue;
		}
		volatile uint32_t *cb = (volatile uint32_t *)contextAddress;
		uint32_t ttbr0Low = cb[MUNCH_SMMU_CB_TTBR0 / 4];
		uint32_t ttbr0High = cb[(MUNCH_SMMU_CB_TTBR0 + 4) / 4];
		uint32_t farLow = cb[MUNCH_SMMU_CB_FAR / 4];
		uint32_t farHigh = cb[(MUNCH_SMMU_CB_FAR + 4) / 4];
		IOLog("MUNCH SMMU: CB%u CBAR=%#x CBA2R=%#x SCTLR=%#x TTBR0=%#x%08x TCR=%#x FSR=%#x FAR=%#x%08x\n",
		    context, cbar, cba2r, cb[MUNCH_SMMU_CB_SCTLR / 4], ttbr0High, ttbr0Low,
		    cb[MUNCH_SMMU_CB_TCR / 4], cb[MUNCH_SMMU_CB_FSR / 4], farHigh, farLow);
		IOLog("MUNCH SMMU: CB%u TCR2=%#x MAIR0=%#x MAIR1=%#x\n", context,
		    cb[MUNCH_SMMU_CB_TCR2 / 4], cb[MUNCH_SMMU_CB_MAIR0 / 4], cb[MUNCH_SMMU_CB_MAIR1 / 4]);
	}
	if (!found) IOLog("MUNCH SMMU: no SMR covers Kona SDE SID %#x/mask %#x\n", requestedSid, requestedMask);
	struct munch_smmu_domain domain = {};
	enum munch_sde_result qualification = munch_smmu_find_stream(scr0, idr0, idr1, streams, &domain);
	IOLog("MUNCH SMMU: stream qualification=%s index=%u CB=%u; page tables not walked or modified\n",
	    munch_sde_result_name(qualification), domain.stream, domain.context);
	IOFree(streams, matches * sizeof(*streams));
	lck_mtx_unlock(&hakios_smmu_lock);
}

static void
munchSdeReadSnapshot(volatile uint32_t *regs, volatile uint32_t *dsi,
    struct munch_sde_snapshot *snapshot, bool log)
{
	bzero(snapshot, sizeof(*snapshot));
	volatile uint32_t *intf = regs + MUNCH_SDE_INTF1_OFFSET / 4;
	snapshot->revision = regs[MUNCH_MDP_VERSION / 4];
	snapshot->mux = intf[MUNCH_SDE_INTF_MUX / 4];
	snapshot->tear_enable = intf[MUNCH_SDE_INTF_TEAR_ENABLE / 4];
	snapshot->tear_vsync = intf[MUNCH_SDE_INTF_TEAR_SYNC_VSYNC / 4];
	snapshot->autorefresh = intf[MUNCH_SDE_INTF_TEAR_AUTOREFRESH / 4];
	snapshot->dsi_ctrl = dsi[MUNCH_DSI_CTRL / 4];
	snapshot->dsi_status = dsi[MUNCH_DSI_STATUS / 4];
	for (unsigned index = 0; index < 8; index++) {
		volatile uint32_t *regsPipe = regs + munch_sde_sspps[index].offset / 4;
		struct munch_sde_pipe_state *pipe = &snapshot->pipes[index];
		pipe->size = regsPipe[MUNCH_SDE_SSPP_SRC_SIZE / 4];
		pipe->out_size = regsPipe[MUNCH_SDE_SSPP_OUT_SIZE / 4];
		pipe->source = regsPipe[MUNCH_SDE_SSPP_SRC_ADDR / 4];
		pipe->stride = regsPipe[MUNCH_SDE_SSPP_SRC_YSTRIDE / 4];
		pipe->format = regsPipe[MUNCH_SDE_SSPP_SRC_FORMAT / 4];
		pipe->unpack = regsPipe[MUNCH_SDE_SSPP_SRC_UNPACK / 4];
		pipe->src_xy = regsPipe[MUNCH_SDE_SSPP_SRC_XY / 4];
		pipe->out_xy = regsPipe[MUNCH_SDE_SSPP_OUT_XY / 4];
		pipe->opmode = regsPipe[MUNCH_SDE_SSPP_SRC_OP_MODE / 4];
		pipe->secure = regsPipe[MUNCH_SDE_SSPP_SECURE / 4];
		pipe->multirect = regsPipe[MUNCH_SDE_SSPP_MULTIRECT / 4];
		for (unsigned component = 0; component < MUNCH_SDE_SSPP_PE_COUNT; component++) {
			unsigned offset = MUNCH_SDE_SSPP_PE_BASE + component * MUNCH_SDE_SSPP_PE_STRIDE;
			pipe->pixel_ext[component].lr = regsPipe[offset / 4];
			pipe->pixel_ext[component].tb = regsPipe[(offset + 4) / 4];
			pipe->pixel_ext[component].required = regsPipe[(offset + 8) / 4];
		}
		if (index < 4) {
			pipe->scaler = regsPipe[MUNCH_SDE_VIG_SCALER_OP_MODE / 4];
			pipe->csc = regsPipe[MUNCH_SDE_VIG_CSC_OP_MODE / 4];
			pipe->pre_downscale = regsPipe[MUNCH_SDE_SSPP_PRE_DOWNSCALE / 4];
			pipe->decimation = regsPipe[MUNCH_SDE_SSPP_DECIMATION / 4];
		}
		if (log && pipe->size) IOLog("MUNCH SDE: %s unpack=%#x src-xy=%#x out-xy=%#x secure=%#x multirect=%#x\n",
		    munch_sde_sspps[index].name, pipe->unpack, pipe->src_xy, pipe->out_xy, pipe->secure, pipe->multirect);
	}
	for (unsigned index = 0; index < 6; index++) {
		volatile uint32_t *regsCtl = regs + munch_sde_ctl_offsets[index] / 4;
		struct munch_sde_ctl_state *ctl = &snapshot->ctls[index];
		ctl->top = regsCtl[MUNCH_SDE_CTL_TOP / 4];
		ctl->active = regsCtl[MUNCH_SDE_CTL_INTF_ACTIVE / 4];
		ctl->start = regsCtl[MUNCH_SDE_CTL_START / 4];
		ctl->flush = regsCtl[MUNCH_SDE_CTL_FLUSH / 4];
		ctl->merge = regsCtl[MUNCH_SDE_CTL_MERGE_ACTIVE / 4];
		ctl->dsc = regsCtl[MUNCH_SDE_CTL_DSC_ACTIVE / 4];
		ctl->wb = regsCtl[MUNCH_SDE_CTL_WB_ACTIVE / 4];
		ctl->cwb = regsCtl[MUNCH_SDE_CTL_CWB_ACTIVE / 4];
		ctl->cdm = regsCtl[MUNCH_SDE_CTL_CDM_ACTIVE / 4];
		volatile uint32_t *regsMixer = regs + (MUNCH_SDE_LM_BASE + index * MUNCH_SDE_LM_STRIDE) / 4;
		snapshot->mixer_size[index] = regsMixer[MUNCH_SDE_LM_OUT_SIZE / 4];
		snapshot->mixers[index].opmode = regsMixer[MUNCH_SDE_LM_OP_MODE / 4];
		for (unsigned stage = 0; stage < MUNCH_SDE_LM_BLEND_COUNT; stage++) {
			unsigned offset = munch_sde_blend_offsets[stage];
			snapshot->mixers[index].blend_op[stage] = regsMixer[offset / 4];
			snapshot->mixers[index].blend_alpha[stage] = regsMixer[(offset + 4) / 4];
		}
		if (log && ctl->active) IOLog("MUNCH SDE: CTL%u flush=%#x merge=%#x dsc=%#x wb=%#x cwb=%#x cdm=%#x\n",
		    index, ctl->flush, ctl->merge, ctl->dsc, ctl->wb, ctl->cwb, ctl->cdm);
		for (unsigned extension = 0; extension < 4; extension++) {
			for (unsigned mixer = 0; mixer < 6; mixer++)
				ctl->layers[extension][mixer] = regsCtl[munch_sde_ctl_layer_offsets[extension][mixer] / 4];
		}
	}
	if (log) IOLog("MUNCH SDE: LM sizes=%#x/%#x/%#x/%#x/%#x/%#x\n", snapshot->mixer_size[0], snapshot->mixer_size[1],
	    snapshot->mixer_size[2], snapshot->mixer_size[3], snapshot->mixer_size[4], snapshot->mixer_size[5]);
}

static void
munchSdeQualify(volatile uint32_t *regs)
{
	struct munch_sde_snapshot *snapshot = (struct munch_sde_snapshot *)IOMallocZero(sizeof(*snapshot));
	if (!snapshot) return;
	vm_offset_t dsiAddress = ml_io_map(MUNCH_DSI_CTRL_BASE, MUNCH_DSI_CTRL_SIZE);
	if (!dsiAddress) {
		IOFree(snapshot, sizeof(*snapshot));
		return;
	}
	munchSdeReadSnapshot(regs, (volatile uint32_t *)dsiAddress, snapshot, true);
	struct munch_sde_route route = {};
	enum munch_sde_result result = munch_sde_find_route(snapshot, &route);
	IOLog("MUNCH SDE: fixed-mode candidate=%s DSI0 ctrl=%#x status=%#x; takeover disabled\n",
	    munch_sde_result_name(result), snapshot->dsi_ctrl, snapshot->dsi_status);
	if (result == MUNCH_SDE_OK) IOLog("MUNCH SDE: candidate CTL%u LM%u %s PP%u IOVA=%#x stride=%u flush=%#x done=%#x\n",
	    route.ctl, route.mixer, munch_sde_sspps[route.pipe].name, route.pingpong, route.source,
	    route.stride, route.flush_mask, route.done_mask);
	IOFree(snapshot, sizeof(*snapshot));
}

static void
munchSdeProbe(void)
{
	vm_offset_t gcc, disp, power, mdp;
	uint32_t configuredPipes = 0, activeCtls = 0;

	if (!gMunchSDEProbe) return;
	gcc = ml_io_map(0x0010b000, 0x1000);
	disp = ml_io_map(0x0af02000, 0x1000);
	power = ml_io_map(0x0af03000, 0x1000);
	if (!gcc || !disp || !power) {
		IOLog("MUNCH SDE: MMIO map unavailable; probe skipped\n");
		return;
	}
	uint32_t gccStatus = *(volatile uint32_t *)(gcc + 0x00c);
	uint32_t dispStatus = *(volatile uint32_t *)(disp + 0x080);
	uint32_t powerStatus = *(volatile uint32_t *)power;
	if (!(gccStatus & 1) || (gccStatus & (1u << 31)) ||
	    !(dispStatus & 1) || (dispStatus & (1u << 31)) ||
	    !(powerStatus & (1u << 31))) {
		IOLog("MUNCH SDE: clocks/power not ready (gcc=%#x disp=%#x power=%#x); skipped\n",
		    gccStatus, dispStatus, powerStatus);
		return;
	}
	mdp = ml_io_map(MUNCH_MDP_PHYS, MUNCH_MDP_SIZE);
	if (!mdp) {
		IOLog("MUNCH SDE: MDP map unavailable; probe skipped\n");
		return;
	}
	volatile uint32_t *regs = (volatile uint32_t *)mdp;
	IOLog("MUNCH SDE: read-only Kona snapshot rev=%#x\n", regs[0]);
	for (unsigned pipeIndex = 0; pipeIndex < sizeof(munch_sde_sspps) / sizeof(munch_sde_sspps[0]); pipeIndex++) {
		volatile uint32_t *pipe = regs + munch_sde_sspps[pipeIndex].offset / sizeof(uint32_t);
		uint32_t srcSize = pipe[MUNCH_SDE_SSPP_SRC_SIZE / 4];
		uint32_t outSize = pipe[MUNCH_SDE_SSPP_OUT_SIZE / 4];
		uint32_t source = pipe[MUNCH_SDE_SSPP_SRC_ADDR / 4];
		uint32_t stride = pipe[MUNCH_SDE_SSPP_SRC_YSTRIDE / 4];
		uint32_t format = pipe[MUNCH_SDE_SSPP_SRC_FORMAT / 4];
		uint32_t opmode = pipe[MUNCH_SDE_SSPP_SRC_OP_MODE / 4];
		if (srcSize || outSize || source || format || opmode) {
			IOLog("MUNCH SDE: %s src=%#x out=%#x addr0=%#x stride=%#x format=%#x opmode=%#x\n",
			    munch_sde_sspps[pipeIndex].name, srcSize, outSize, source, stride, format, opmode);
			configuredPipes++;
		}
	}
	for (unsigned ctlIndex = 0; ctlIndex < MUNCH_SDE_CTL_COUNT; ctlIndex++) {
		volatile uint32_t *ctl = regs + munch_sde_ctl_offsets[ctlIndex] / sizeof(uint32_t);
		uint32_t top = ctl[MUNCH_SDE_CTL_TOP / 4];
		uint32_t start = ctl[MUNCH_SDE_CTL_START / 4];
		uint32_t intfActive = ctl[MUNCH_SDE_CTL_INTF_ACTIVE / 4];
		if (top || start || intfActive) {
			IOLog("MUNCH SDE: CTL%u top=%#x start=%#x intf-active=%#x\n",
			    ctlIndex, top, start, intfActive);
			activeCtls++;
			for (unsigned routeIndex = 0; routeIndex < sizeof(munch_sde_ctl_layer_offsets) / sizeof(munch_sde_ctl_layer_offsets[0]); routeIndex++) {
				uint32_t route[6];
				int configured = 0;

				for (unsigned layer = 0; layer < 6; layer++) {
					route[layer] = ctl[munch_sde_ctl_layer_offsets[routeIndex][layer] / 4];
					configured |= route[layer] != 0;
				}
				if (configured) {
					IOLog("MUNCH SDE: CTL%u layer-map%u=%#x/%#x/%#x/%#x/%#x/%#x\n",
					    ctlIndex, routeIndex, route[0], route[1], route[2], route[3], route[4], route[5]);
				}
			}
		}
	}
	volatile uint32_t *intf = regs + MUNCH_SDE_INTF1_OFFSET / sizeof(uint32_t);
	IOLog("MUNCH SDE: INTF1 engine=%#x mux=%#x status=%#x count-en=%#x frame=%#x line=%#x "
	    "tear-line=%#x write=%#x "
	    "nonzero-pipes=%u active-ctls=%u\n",
	    intf[MUNCH_SDE_INTF_ENABLE / 4], intf[MUNCH_SDE_INTF_MUX / 4], intf[MUNCH_SDE_INTF_STATUS / 4],
	    intf[MUNCH_SDE_INTF_FRAME_LINE_COUNT_EN / 4], intf[MUNCH_SDE_INTF_FRAME_COUNT / 4],
	    intf[MUNCH_SDE_INTF_LINE_COUNT / 4],
	    intf[MUNCH_SDE_INTF_TEAR_OUT_LINE / 4], intf[MUNCH_SDE_INTF_TEAR_LINE / 4],
	    configuredPipes, activeCtls);
	IOLog("MUNCH SDE: INTF1 tear mux=%#x enable=%#x vsync=%#x height=%#x wrcount=%#x init=%#x int=%#x "
	    "threshold=%#x start=%#x rd-irq=%#x wr-irq=%#x auto=%#x detect=%#x\n",
	    intf[MUNCH_SDE_INTF_TEAR_MDP_VSYNC / 4], intf[MUNCH_SDE_INTF_TEAR_ENABLE / 4],
	    intf[MUNCH_SDE_INTF_TEAR_SYNC_VSYNC / 4], intf[MUNCH_SDE_INTF_TEAR_HEIGHT / 4],
	    intf[MUNCH_SDE_INTF_TEAR_SYNC_WRCOUNT / 4], intf[MUNCH_SDE_INTF_TEAR_VSYNC_INIT / 4],
	    intf[MUNCH_SDE_INTF_TEAR_INT_COUNT / 4], intf[MUNCH_SDE_INTF_TEAR_SYNC_THRESH / 4],
	    intf[MUNCH_SDE_INTF_TEAR_START_POS / 4], intf[MUNCH_SDE_INTF_TEAR_RD_PTR_IRQ / 4],
	    intf[MUNCH_SDE_INTF_TEAR_WR_PTR_IRQ / 4], intf[MUNCH_SDE_INTF_TEAR_AUTOREFRESH / 4],
	    intf[MUNCH_SDE_INTF_TEAR_DETECT / 4]);
	if ((regs[MUNCH_MDP_VERSION / 4] >> 16) == 0x6000) munchSdeQualify(regs);
	munchSmmuProbe();
}

#include "munch-sde-xnu.h"

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz, with one colour mode, 8-bit RGB. fb_hz changes software pacing only. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                 /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	PMinit();
	joinPMtree(provider);
	registerPowerDriver(this, gFramebufferPowerStates, 2);
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	fDisplayLock = IOLockAlloc();
	if (!fLock || !fSwapLock) return false;
	fPanelOn = true;
	fDSIRegs = (volatile uint32_t *) ml_io_map(MUNCH_DSI_CTRL_BASE, MUNCH_DSI_CTRL_SIZE);
	fDisplayGCC = (volatile uint32_t *) ml_io_map(MUNCH_DISPLAY_GCC_BASE, 0x1000);
	fDisplayCC = (volatile uint32_t *) ml_io_map(MUNCH_DISPLAY_CC_BASE, 0x1000);
	fDisplayPower = (volatile uint32_t *) ml_io_map(MUNCH_DISPLAY_POWER_BASE, 0x1000);
	if (fDisplayLock == NULL) {
		IOLog("MiniIOMobileFramebuffer: no lock for panel power control; requests disabled\n");
	}
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fStatGPUBudgetMisses = 0;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fSDERequested = gMunchSDEPresent != 0;
	fSDENativeRequested = fSDERequested && gMunchSDENative != 0;
	/* CoreAnimation must see the iOS logical mode before any native presenter
	 * properties are published.  The presenter owns only the physical panel. */
	fWidth = gFBWidth ? gFBWidth : kFBDefaultWidth;
	fHeight = gFBHeight ? gFBHeight : kFBDefaultHeight;
	if (!fSDENativeRequested && gFBScale != 0 && gFBScale != kFBDefaultScale) {
		fWidth = kFBPointsWide * gFBScale;
		fHeight = kFBPointsHigh * gFBScale;
	}
	if (fSDERequested) {
		IOLog("MUNCH SDE: boot console retained until first completed frame; GPU test blits disabled\n");
		char liveLine[160];
		snprintf(liveLine, sizeof(liveLine), "MUNCH SDE START: native=%u panel=%ux%u display=%ux%u\n",
		    (unsigned)fSDENativeRequested, fPanelWidth, fPanelHeight, fWidth, fHeight);
		loader_log_line(liveLine);
		fSDEError = kIOReturnNotReady;
		fSDEInitResult = MUNCH_SDE_DISABLED;
		fSDEInitPhase = !fDisplayLock ? "no-display-lock" : "owner-unavailable";
		if (fDisplayLock && !gMunchSDEOwner) {
			fSDEInitPhase = "state-allocation";
			fSDE = IONewZero(MunchSDE, 1);
			if (fSDE) {
				fSDE->nativeRequested = fSDENativeRequested;
				fSDE->nativeTakeover = fSDENativeRequested && gMunchSDETakeover != 0;
				gMunchSDEOwner = fSDE;
				munchSdeBarrier(NULL);
				lck_mtx_lock(&hakios_smmu_lock);
				enum munch_sde_result result = munchSdeInitialize(fSDE, fBase, fRowBytes, fPanelWidth, fPanelHeight);
				fSDEInitResult = result;
				fSDEInitPhase = fSDE->initPhase;
				if (result != MUNCH_SDE_OK) munchSdeReleaseUnused(fSDE);
				lck_mtx_unlock(&hakios_smmu_lock);
				fSDEError = result == MUNCH_SDE_OK ? kIOReturnSuccess : kIOReturnNotReady;
				IOLog("MUNCH SDE: hardware presenter v1: %s: %s (%u); native=%u takeover=%u\n",
				    result == MUNCH_SDE_OK ? "ready" : "rejected", munch_sde_result_name(result), (unsigned)result,
				    (unsigned)fSDENativeRequested, (unsigned)fSDE->nativeTakeover);
			}
		}
		if (!fSDE) IOLog("MUNCH SDE: presenter allocation/ownership unavailable; swaps disabled\n");
		setProperty("MunchSDEBackend", fSDEError == kIOReturnSuccess);
	}
	/* Keep the iOS logical display contract stable. Native SDE owns the real
	 * 1080x2400 scanout, while CoreAnimation continues to render the known
	 * 1125x2436 iPhone layout and drawLayer scales it to the panel. */
	if (fSDENativeRequested) {
		setProperty("NativePanelWidth", fPanelWidth, 32);
		setProperty("NativePanelHeight", fPanelHeight, 32);
	}
	if (!fSDENativeRequested && gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);
	IOLog("MUNCH SDE PERF READY: native=%u panel=%ux%u display=%ux%u logical=%ux%u threads=%u queue=%u\n",
	    (unsigned)fSDENativeRequested, fPanelWidth, fPanelHeight, fWidth, fHeight, fWidth, fHeight,
	    fThreads, swapQueue());
	munchSdeProbe();
	if (fDSIRegs == NULL) {
		IOLog("MiniIOMobileFramebuffer: DSI control mapping unavailable; panel power requests disabled\n");
	}

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("HakiosWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	registerService();
	return true;
}

IOReturn
IOMobileFramebuffer::setPowerState(unsigned long powerStateOrdinal, IOService * whatDevice __unused)
{
	if (powerStateOrdinal > 1 || !fDisplayLock) return IOPMAckImplied;
	IOReturn result = setDisplayPower(powerStateOrdinal);
	if (result != kIOReturnSuccess)
		IOLog("MiniIOMobileFramebuffer: PM display transition to %lu returned 0x%x\n", powerStateOrdinal, result);
	return IOPMAckImplied;
}

IOReturn
IOMobileFramebuffer::sendDcsCommand(uint8_t command, uint8_t parameter, bool hasParameter)
{
	volatile uint32_t * regs = fDSIRegs;
	uint32_t controller, status = 0, interruptControl, dmaControl, dmaControlOriginal, fifoControl;
	uint32_t tpgControl, dmaLength, packet;
	bool completed = false;
	bool failed = false;

	if (regs == NULL || fDisplayLock == NULL || fDisplayGCC == NULL || fDisplayCC == NULL ||
	    fDisplayPower == NULL) return kIOReturnNotReady;
	uint32_t gccStatus = fDisplayGCC[0x00c / 4];
	uint32_t displayStatus = fDisplayCC[0x080 / 4];
	uint32_t powerStatus = fDisplayPower[0];
	if (!(gccStatus & 1) || (gccStatus & (1u << 31)) || !(displayStatus & 1) ||
	    (displayStatus & (1u << 31)) || !(powerStatus & (1u << 31))) {
		return kIOReturnNotReady;
	}
	controller = regs[MUNCH_DSI_CTRL / 4];
	if ((controller & (MUNCH_DSI_ENGINE_ENABLED | MUNCH_DSI_CMD_MODE_ENABLED)) !=
	    (MUNCH_DSI_ENGINE_ENABLED | MUNCH_DSI_CMD_MODE_ENABLED)) {
		return kIOReturnNotReady;
	}
	for (unsigned i = 0; i < 200; i++) {
		status = regs[MUNCH_DSI_STATUS / 4];
		if (!(status & MUNCH_DSI_CMD_MODE_MDP_BUSY)) break;
		IOSleep(1);
	}
	if (status & MUNCH_DSI_CMD_MODE_MDP_BUSY) return kIOReturnBusy;

	interruptControl = regs[MUNCH_DSI_INT_CTRL / 4] & 0xaaaaaaaau;
	if (regs[MUNCH_DSI_INT_CTRL / 4] & MUNCH_DSI_ERROR_STATUS) return kIOReturnIOError;
	dmaControlOriginal = regs[MUNCH_DSI_COMMAND_MODE_DMA_CTRL / 4];
	dmaControl = dmaControlOriginal;
	fifoControl = regs[MUNCH_DSI_DMA_FIFO_CTRL / 4];
	tpgControl = regs[MUNCH_DSI_TPG_CTRL / 4];
	dmaLength = regs[MUNCH_DSI_DMA_CMD_LENGTH / 4];
	packet = (uint32_t) parameter | ((hasParameter ? 0x15u : 0x05u) << 8) |
	    ((uint32_t) command << 16) | (0x80u << 24);

	regs[MUNCH_DSI_INT_CTRL / 4] = interruptControl | MUNCH_DSI_CMD_DMA_DONE_MASK | MUNCH_DSI_CMD_DMA_DONE;
	OSSynchronizeIO();
	if (regs[MUNCH_DSI_INT_CTRL / 4] & MUNCH_DSI_CMD_DMA_DONE) {
		regs[MUNCH_DSI_INT_CTRL / 4] = interruptControl;
		return kIOReturnIOError;
	}
	regs[MUNCH_DSI_TPG_CTRL / 4] = (1u << 1) | (1u << 2) | (3u << 16);
	regs[MUNCH_DSI_TPG_DMA_INIT / 4] = packet;
	regs[MUNCH_DSI_TPG_DMA_INIT / 4] = 0;
	regs[MUNCH_DSI_DMA_FIFO_CTRL / 4] = fifoControl | (1u << 16) | (1u << 20);
	dmaControl &= ~((1u << 31) | (1u << 30));
	dmaControl |= (1u << 28) | (1u << 26);
	regs[MUNCH_DSI_COMMAND_MODE_DMA_CTRL / 4] = dmaControl;
	regs[MUNCH_DSI_DMA_CMD_LENGTH / 4] = 4;
	OSSynchronizeIO();
	regs[MUNCH_DSI_CMD_DMA_SW_TRIGGER / 4] = 1;

	for (unsigned i = 0; i < 200; i++) {
		uint32_t interrupts = regs[MUNCH_DSI_INT_CTRL / 4];
		if (interrupts & MUNCH_DSI_ERROR_STATUS) { failed = true; break; }
		if (interrupts & MUNCH_DSI_CMD_DMA_DONE) {
			completed = true;
			break;
		}
		IOSleep(1);
	}

	regs[MUNCH_DSI_TPG_CTRL / 4] = 0;
	regs[MUNCH_DSI_TPG_DMA_FIFO_RESET / 4] = 1;
	delay(1);
	regs[MUNCH_DSI_TPG_DMA_FIFO_RESET / 4] = 0;
	regs[MUNCH_DSI_TPG_CTRL / 4] = tpgControl;
	regs[MUNCH_DSI_DMA_CMD_LENGTH / 4] = dmaLength;
	regs[MUNCH_DSI_COMMAND_MODE_DMA_CTRL / 4] = dmaControlOriginal;
	regs[MUNCH_DSI_DMA_FIFO_CTRL / 4] = fifoControl;
	regs[MUNCH_DSI_INT_CTRL / 4] = interruptControl | MUNCH_DSI_CMD_DMA_DONE;
	OSSynchronizeIO();
	return failed ? kIOReturnIOError : completed ? kIOReturnSuccess : kIOReturnTimeout;
}

IOReturn
IOMobileFramebuffer::setDisplayPower(uint32_t state)
{
	IOReturn result = kIOReturnSuccess;
	bool on;
	bool blanked_for_transition = false;

	if (state > 1) return kIOReturnBadArgument;
	if (fDisplayLock == NULL) return kIOReturnNotReady;
	on = state != 0;
	IOLockLock(fDisplayLock);
	if (fSDERequested && (!fSDE || fSDE->panelCommandFailed)) {
		IOLockUnlock(fDisplayLock);
		return kIOReturnNotReady;
	}
	if (on == fPanelOn) {
		IOLockUnlock(fDisplayLock);
		return kIOReturnSuccess;
	}
	if (!on && fSDERequested && fSDENativeRequested && fSDE && fSDE->nativeTakeover) {
		IOLockLock(fSwapLock);
		fDisplayBlanked = true;
		IOLockWakeup(fSwapLock, &fSwapsWaiting, false);
		IOLockUnlock(fSwapLock);
		blanked_for_transition = true;
		IOLockUnlock(fDisplayLock);

		bool drained = false;
		for (unsigned attempt = 0; attempt < 500; attempt++) {
			IOLockLock(fSwapLock);
			bool in_flight = fSDEInFlight;
			bool queued = fSwapsWaiting != 0;
			IOLockUnlock(fSwapLock);
			if (in_flight) {
				if (waitNativeFrame() != MUNCH_SDE_OK) break;
			} else if (!queued && fSDE->presenter.pending == 3 && fSDE->presenter.drawing == 3) {
				drained = true;
				break;
			} else {
				IODelay(1000);
			}
		}
		IOLockLock(fDisplayLock);
		if (!drained) {
			IOLockLock(fSwapLock);
			fDisplayBlanked = false;
			IOLockWakeup(fSwapLock, &fSwapsWaiting, false);
			IOLockUnlock(fSwapLock);
			IOLockUnlock(fDisplayLock);
			IOLog("MiniIOMobileFramebuffer: native panel-off drain timed out\n");
			return kIOReturnBusy;
		}
	}
	if (fSDERequested) {
		enum munch_sde_result prepared = munchSdePreparePanel(fSDE);
		if (prepared != MUNCH_SDE_OK) {
			if (blanked_for_transition) {
				IOLockLock(fSwapLock);
				fDisplayBlanked = false;
				IOLockWakeup(fSwapLock, &fSwapsWaiting, false);
				IOLockUnlock(fSwapLock);
			}
			IOLockUnlock(fDisplayLock);
			return prepared == MUNCH_SDE_BUSY ? kIOReturnBusy : kIOReturnNotReady;
		}
	}
	if (on) {
		if (fSDERequested && fSDE->mode.configured) {
			lck_mtx_lock(&hakios_smmu_lock);
			if (!munch_mode_light(&fSDE->mode)) result = kIOReturnIOError;
			lck_mtx_unlock(&hakios_smmu_lock);
		} else {
			result = sendDcsCommand(0x11, 0, false);
			if (result == kIOReturnSuccess) {
				IOSleep(120);
				result = sendDcsCommand(0x53, 0x20, true);
			}
			if (result == kIOReturnSuccess) result = sendDcsCommand(0x29, 0, false);
		}
	} else {
		if (fSDERequested && fSDE->mode.configured) {
			result = sendDcsCommand(0x51, 0, true);
		} else {
			result = sendDcsCommand(0x28, 0, false);
			if (result == kIOReturnSuccess) {
				IOSleep(20);
				result = sendDcsCommand(0x53, 0x20, true);
			}
			if (result == kIOReturnSuccess) {
				result = sendDcsCommand(0x10, 0, false);
				if (result == kIOReturnSuccess) IOSleep(120);
			}
		}
	}
	if (fSDERequested) {
		enum munch_sde_result finished = munchSdeFinishPanel(fSDE, on, result == kIOReturnSuccess);
		if (finished != MUNCH_SDE_OK && result == kIOReturnSuccess) result = kIOReturnIOError;
		IOLockLock(fSwapLock);
		if (result == kIOReturnSuccess) fDisplayBlanked = !on;
		else fSDEError = result;
		IOLockWakeup(fSwapLock, &fSwapsWaiting, false);
		IOLockUnlock(fSwapLock);
	}
	if (result == kIOReturnSuccess) fPanelOn = on;
	if (result == kIOReturnSuccess) hakios_cpufreq_set_screen_on(on ? 1 : 0);
	IOLog("MiniIOMobileFramebuffer: panel power request %u result=0x%x\n", state, result);
	IOLockUnlock(fDisplayLock);
	if (result != kIOReturnSuccess) {
		IOLog("MiniIOMobileFramebuffer: panel power transition to %u failed: 0x%x\n", state, result);
	}
	return result;
}

IOReturn
IOMobileFramebuffer::changeRefresh(uint32_t refresh_hz)
{
	if (refresh_hz != 60 && refresh_hz != 120) return kIOReturnBadArgument;
	if (!fDisplayLock || !fSDERequested || !fSDENativeRequested || !fSDE ||
	    !fSDE->nativeTakeover || !fPanelOn) return kIOReturnUnsupported;
	IOLockLock(fDisplayLock);
	if (fSDE->presenter.pending != 3 || fSDE->panelCommandActive || fSDE->panelCommandFailed) {
		IOLockUnlock(fDisplayLock);
		return kIOReturnBusy;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	enum munch_sde_result result = munchSdeSwitchRefresh(fSDE, refresh_hz);
	lck_mtx_unlock(&hakios_smmu_lock);
	if (result == MUNCH_SDE_OK) {
		gNativeRefreshHz = refresh_hz;
		IOLog("MUNCH SDE: dynamic refresh switched to %u Hz\n", refresh_hz);
	}
	IOLockUnlock(fDisplayLock);
	return result == MUNCH_SDE_OK ? kIOReturnSuccess :
	    result == MUNCH_SDE_BUSY ? kIOReturnBusy : kIOReturnIOError;
}

static bool
munchSdeComposeFullFrame(const uint8_t *source, uint32_t sourceWidth, uint32_t sourceHeight,
    uint32_t sourceStride, uint8_t *destination, uint32_t destinationWidth, uint32_t destinationHeight,
    uint32_t destinationStride)
{
	if (!source || !destination || !sourceWidth || !sourceHeight || !destinationWidth || !destinationHeight ||
	    sourceWidth > sourceStride / 4 || destinationWidth > destinationStride / 4 ||
	    sourceWidth != destinationWidth || sourceHeight != destinationHeight) return false;

	for (uint32_t y = 0; y < destinationHeight; y++) {
		memcpy(destination + (size_t)y * destinationStride, source + (size_t)y * sourceStride,
		    (size_t)destinationWidth * 4);
	}
	return true;
}

static bool
munchFramebufferDownscaleRows(const uint8_t *source, uint32_t sourceStride, uint8_t *destination,
    uint32_t destinationStride, uint32_t firstRow, uint32_t lastRow)
{
	if (!source || !destination || sourceStride < 1125 * 4 || destinationStride < 1080 * 4 ||
	    firstRow > lastRow || lastRow > 2400) return false;

	for (uint32_t y = firstRow; y < lastRow; y++) {
		const uint32_t *input = (const uint32_t *)(source + (size_t)((uint64_t)y * 2436 / 2400) * sourceStride);
		uint32_t *output = (uint32_t *)(destination + (size_t)y * destinationStride);

		for (unsigned group = 0; group < 45; group++) {
			const uint32_t *in = input + group * 25;
			uint32_t *out = output + group * 24;
			memcpy(out, in, 24 * sizeof(*out));
		}
	}
	return true;
}

static bool
munchSdeReplacesFrame(const uint8_t *swap, const MiniIOSurface *surface, uint32_t width, uint32_t height)
{
	if (!surface || surface->info.bytesPerElement != 4 || surface->info.planeCount) return false;
	int32_t source[4], destination[4];
	memcpy(source, swap + SWAP_SOURCE(0), sizeof(source));
	memcpy(destination, swap + SWAP_DEST(0), sizeof(destination));
	if (source[2] <= 0 || source[3] <= 0) {
		if (!surface->info.width || !surface->info.height ||
		    surface->info.width > INT32_MAX || surface->info.height > INT32_MAX) return false;
		source[0] = source[1] = 0;
		source[2] = (int32_t) surface->info.width;
		source[3] = (int32_t) surface->info.height;
	} else if (source[0] < 0 || source[1] < 0 ||
	    (uint64_t)source[0] + source[2] > surface->info.width ||
	    (uint64_t)source[1] + source[3] > surface->info.height) return false;
	bool fullSource = source[0] == 0 && source[1] == 0 &&
	    (uint32_t) source[2] == surface->info.width &&
	    (uint32_t) source[3] == surface->info.height;
	return fullSource && (destination[2] <= 0 || destination[3] <= 0 ||
	    (destination[0] == 0 && destination[1] == 0 &&
	    (uint32_t)destination[2] == width && (uint32_t)destination[3] == height));
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled, fastDownscale;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	if (job->fastDownscale) {
		munchFramebufferDownscaleRows(job->pixels, job->stride, fBase, fRowBytes,
		    (uint32_t)y0, (uint32_t)y1);
		return;
	}
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)],
				    fColumnWeight[x]) : from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			const uint32_t * __restrict column = fColumn + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				out[i] = from[column[i]];
			}
		}
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->py1 - job->py0;
		y0 = job->py0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->py0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0 && !fSDENativeRequested;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	bool fullDestination = dx == 0 && dy == 0 && (uint32_t) dw == fWidth && (uint32_t) dh == fHeight;
	bool fullSource = sx == 0 && sy == 0 && sw == (int32_t) s->info.width && sh == (int32_t) s->info.height;
	if (fSDENativeRequested && !fDiff && fullDestination && fullSource &&
	    munchSdeComposeFullFrame(s->pixels(), (uint32_t)sw, (uint32_t)sh, s->info.bytesPerRow,
	    fBase, fPanelWidth, fPanelHeight, fRowBytes)) {
		fStatSDEFullCompose++;
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	bool fastDownscale = !filter && !fDiff && fullDestination && fullSource &&
	    (uint32_t)sw == 1125 && (uint32_t)sh == 2436 &&
	    fPanelWidth == 1080 && fPanelHeight == 2400 &&
	    px0 == 0 && py0 == 0 && px1 == 1080 && py1 == 2400 &&
	    s->info.bytesPerRow >= 1125 * 4 && fRowBytes >= 1080 * 4;
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; !fastDownscale && x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	job.fastDownscale = fastDownscale;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, py0, py1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(&job, py0, py0 + (int32_t) ((int64_t) (py1 - py0) / fThreads), fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

void
IOMobileFramebuffer::reportSDEPerformance(uint64_t now)
{
	if (!fSDERequested || !fStatFrames || fStatSince == 0) return;
	uint64_t nanos;
	absolutetime_to_nanoseconds(now - fStatSince, &nanos);
	if (nanos < 5 * NSEC_PER_SEC) return;
	uint64_t drawing, waiting;
	absolutetime_to_nanoseconds(fStatDrawing, &drawing);
	absolutetime_to_nanoseconds(fStatWaiting, &waiting);
	uint32_t irqEvents = __atomic_exchange_n(&fSDE->completionIRQEvents, 0u, __ATOMIC_ACQ_REL);
	bool irqActive = fSDE->completionIRQAttached &&
	    !__atomic_load_n(&fSDE->completionIRQMasked, __ATOMIC_ACQUIRE);
	IOLockLock(fDisplayLock);
	IOLog("MUNCH SDE PERF r22: frames=%u fps=%llu driver=%llu gpu_wait=%llu acquire=%llu compose=%llu commit=%llu completion=%llu overlap=%u retired=%u queued=%u full_compose=%u clean=%llu wait=%llu guards=%llu polls=%llu irq=%u irq_active=%u in_flight=%u vblank=%llu hw=%llu fallback=%llu source=%s\n",
	    fStatFrames, fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
	    waiting / fStatFrames / NSEC_PER_USEC, micros(fStatSDEAcquire) / fStatFrames,
	    micros(fStatSDECompose - fStatSDEAcquire) / fStatFrames, micros(fStatSDECommit) / fStatFrames,
	    micros(fStatSDECompletion) / fStatFrames, fStatSDEOverlapped, fStatSDERetired, fStatSDEQueued,
	    fStatSDEFullCompose, micros(fSDE->cleanTime) / fStatFrames,
	    micros(fSDE->waitTime) / fStatFrames,
	    fSDE->guardCalls, fSDE->pollChecks, irqEvents, irqActive, (unsigned)fSDEInFlight, fSDE->vblankCount,
	    fSDE->vblankHardwareEvents, fSDE->vblankFallbackEvents, fSDE->vblankFallback ? "timer" : "frame");
	IOLockUnlock(fDisplayLock);
	fStatSDEAcquire = fStatSDECompose = fStatSDECommit = 0;
	fStatSDECompletion = 0;
	fStatSDEOverlapped = 0;
	fStatSDERetired = 0;
	fStatSDEQueued = 0;
	fStatSDEFullCompose = 0;
	fSDE->cleanTime = fSDE->waitTime = fSDE->guardCalls = fSDE->pollChecks = 0;
	fStatFrames = 0;
	fStatDrawing = 0;
	fStatWaiting = 0;
	fStatSince = now;
	IOLockLock(fLock);
	reportFrameTimes();
	IOLockUnlock(fLock);
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
IOReturn
IOMobileFramebuffer::present(const uint8_t * swap)
{
	static uint64_t lastTimeoutLogAt;
	AbsoluteTime deadline;
	uint32_t swapID = swapWord(swap, SWAP_ID);
	/* Native command-mode can keep one frame in hardware while the next one
	 * is being composed.  Limiting it to one outstanding swap turns every TE
	 * interval into a producer stall. */
	unsigned limit = fSDENativeRequested ? SWAP_QUEUE : (fSDERequested ? 1 : swapQueue());

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		hakios_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	if (fSDERequested && fSDEError != kIOReturnSuccess) {
		IOReturn result = fSDEError;
		IOLockUnlock(fSwapLock);
		return result;
	}
	if (fSDERequested && fDisplayBlanked) {
		IOLockUnlock(fSwapLock);
		return kIOReturnNotReady;
	}
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= limit;
	while (fSwapsWaiting >= limit) {
		if (fSDERequested && (fDisplayBlanked || fSDEError != kIOReturnSuccess)) break;
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= limit) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			uint32_t oldest = swapWord(fSwap[fSwapFirst], SWAP_ID);
			uint32_t waiting = fSwapsWaiting;
			uint64_t logInterval, timeoutAt = mach_absolute_time();
			nanoseconds_to_absolutetime(NSEC_PER_SEC, &logInterval);
			bool logTimeout = lastTimeoutLogAt == 0 || timeoutAt - lastTimeoutLogAt >= logInterval;
			if (logTimeout) lastTimeoutLogAt = timeoutAt;
			uint32_t lost = fStatLost;
			IOLockUnlock(fSwapLock);
			if (logTimeout) {
				char liveLine[192];
				snprintf(liveLine, sizeof(liveLine),
				    "MUNCH FB QUEUE TIMEOUT: swap=%u queued=%u/%u oldest=%u lost=%u\n",
				    swapID, waiting, limit, oldest, lost);
				loader_log_line(liveLine);
			}
			return kIOReturnTimeout;
		}
	}
	if (fSDERequested && fDisplayBlanked) {
		IOLockUnlock(fSwapLock);
		return kIOReturnNotReady;
	}
	if (fSDERequested && fSDEError != kIOReturnSuccess) {
		IOReturn result = fSDEError;
		IOLockUnlock(fSwapLock);
		return result;
	}
	if (fSDERequested) {
		auto &result = fSwapResults[swapID % 64];
		if (!swapID || result.pending || (result.valid && result.id == swapID)) {
			IOLockUnlock(fSwapLock);
			return kIOReturnBadArgument;
		}
		result.id = swapID;
		result.result = kIOReturnNotReady;
		result.pending = result.valid = true;
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	if (fSDENativeRequested) {
		__atomic_add_fetch(&fSDE->completionIRQSequence, 1u, __ATOMIC_RELEASE);
		munchSdeWakeup((void *)&fSDE->completionIRQSequence);
	}
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebuffer::submitSwap(const void *bytes, uint32_t size)
{
	bool valid = bytes && size >= SWAP_SIZE;
	IOReturn result = valid ? present((const uint8_t *)bytes) : kIOReturnBadArgument;
	if (!valid) IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", size, SWAP_SIZE);
	if (fSDERequested) {
		IOLockLock(fSwapLock);
		fSDESwapRequests++;
		fSDEAcceptedRequests += result == kIOReturnSuccess;
		fSDELastSwapID = valid ? swapWord((const uint8_t *)bytes, SWAP_ID) : 0;
		fSDELastSwapSize = size;
		fSDELastSwapResult = result;
		IOLockUnlock(fSwapLock);
	}
	return result;
}

IOReturn
IOMobileFramebuffer::waitSwap(uint32_t swapID)
{
	if (!fSDERequested) return kIOReturnSuccess;
	AbsoluteTime deadline;
	clock_interval_to_deadline(SWAP_GPU_MS * SWAP_LAYERS + SWAP_HELD_MS, kMillisecondScale, &deadline);
	IOLockLock(fSwapLock);
	auto &entry = fSwapResults[swapID % 64];
	while (entry.valid && entry.id == swapID && entry.pending) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapResults, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    entry.id == swapID && entry.pending) {
			IOLockUnlock(fSwapLock);
			return kIOReturnTimeout;
		}
	}
	IOReturn result = entry.valid && entry.id == swapID ? entry.result : kIOReturnNotFound;
	IOLockUnlock(fSwapLock);
	return result;
}

void
IOMobileFramebuffer::finishSwap(uint32_t swapID, IOReturn result)
{
	if (!fSDERequested) return;
	IOLockLock(fSwapLock);
	auto &entry = fSwapResults[swapID % 64];
	entry.result = result;
	entry.pending = false;
	if (result == kIOReturnSuccess) {
		if (!fGraphics) {
			fGraphics = true;
			PE_initialize_console(NULL, kPEGraphicsMode);
		}
		fHardwareFrames++;
		fHardwareTime = fSDE->completedTime;
	} else if (result != kIOReturnAborted) {
		if (fSDENativeRequested) {
			enum munch_sde_result nativeResult = result == kIOReturnTimeout ? MUNCH_SDE_TIMEOUT :
			    (fSDE && fSDE->presenter.fault != MUNCH_SDE_OK ? fSDE->presenter.fault : MUNCH_SDE_FAULT);
			reportNativeFailure(nativeResult, result, swapID);
		}
		fSDEError = result;
		IOLog("MUNCH SDE: swap %u failed: 0x%x; presentation stopped\n", swapID, result);
	}
	if (fSwapsWaiting != 0) {
		fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
		fSwapsWaiting--;
	}
	IOLockWakeup(fSwapLock, &fSwapResults, false);
	IOLockWakeup(fSwapLock, &fSwapsWaiting, false);
	IOLockUnlock(fSwapLock);
	if (result == kIOReturnSuccess) messageClients(kIOMessageServicePropertyChange);
}

bool
IOMobileFramebuffer::presentationTime(uint64_t *count, uint64_t *time)
{
	if (fSDENativeRequested) {
		if (!fSDE) return false;
		/*
		 * The producer holds fDisplayLock while it copies/composes a frame and
		 * waits for the command-mode kickoff. VSync sampling must continue in
		 * that interval; serialize only the MMIO/state access shared with the
		 * native SDE transaction.
		 */
		lck_mtx_lock(&hakios_smmu_lock);
		bool ready = munchSdeSampleVBlank(fSDE);
		if (ready) {
			*count = fSDE->vblankCount;
			*time = fSDE->vblankTime;
		}
		lck_mtx_unlock(&hakios_smmu_lock);
		return ready;
	}
	IOLockLock(fSwapLock);
	*count = fHardwareFrames;
	*time = fHardwareTime;
	bool ready = fSDERequested && !fDisplayBlanked && fSDEError == kIOReturnSuccess && fHardwareFrames != 0;
	IOLockUnlock(fSwapLock);
	return ready;
}

void
IOMobileFramebuffer::reportSDEStatus(uint64_t now)
{
	if (!fSDERequested || (fHardwareFrames && fSDEError == kIOReturnSuccess)) return;
	if (fSDEStatusLogged && now - fSDEStatusTime < 5000000) return;
	fSDEStatusLogged = true;
	fSDEStatusTime = now;
	IOLog("MUNCH SDE STATUS r15: init=%s phase=%s error=0x%08x\n",
	    munch_sde_result_name(fSDEInitResult), fSDEInitPhase, (unsigned)fSDEError);
	IOLog("MUNCH SDE STATUS: SwapEnd=%llu accepted=%llu id=%u bytes=%u rc=0x%08x\n",
	    fSDESwapRequests, fSDEAcceptedRequests, fSDELastSwapID, fSDELastSwapSize, (unsigned)fSDELastSwapResult);
	IOLog("MUNCH SDE STATUS: completed=%llu queued=%u\n", fHardwareFrames, fSwapsWaiting);
	if (fSDE && fSDE->nativeRequested) {
		IOLog("MUNCH SDE NATIVE r15: phase=%s configured=%u commands=%u failed=%u reg=%u:%x value=%x\n",
		    fSDE->mode.phase ? fSDE->mode.phase : "not-prepared", (unsigned)fSDE->mode.configured,
		    fSDE->mode.commands, (unsigned)fSDE->mode.failed, (unsigned)fSDE->mode.failed_space,
		    fSDE->mode.failed_offset, fSDE->mode.failed_value);
	}
	if (fSDE && fSDE->snapshotValid) {
		const struct munch_sde_snapshot *snapshot = &fSDE->baseline;
		IOLog("MUNCH SDE SAVED: rev=%08x mux=%x tear=%x auto=%08x dsi=%x dsistat=%x\n",
		    snapshot->revision, snapshot->mux, snapshot->tear_enable, snapshot->autorefresh,
		    snapshot->dsi_ctrl, snapshot->dsi_status);
		IOLog("MUNCH SDE AUTO: enabled=%u frames=%u vsync=%08x\n",
		    (unsigned)((snapshot->autorefresh & MUNCH_SDE_AUTOREFRESH_ENABLE) != 0),
		    snapshot->autorefresh & MUNCH_SDE_AUTOREFRESH_FRAMES, snapshot->tear_vsync);
		IOLog("MUNCH SDE AUTO STATE: phase=%s owned=%u paused=%u status=%08x line=%u\n",
		    fSDE->refresh.phase ? fSDE->refresh.phase : "inherited", (unsigned)fSDE->refresh.owned,
		    (unsigned)fSDE->refresh.paused, fSDE->refresh.status, fSDE->refresh.line);
		struct munch_sde_route route;
		struct munch_sde_route_diagnostic diagnostic;
		enum munch_sde_result result = munch_sde_find_route_checked(snapshot, &route, &diagnostic);
		IOLog("MUNCH SDE ROUTE: check=%s ctl=%d lm=%d pipe=%d result=%s\n",
		    diagnostic.check, diagnostic.ctl, diagnostic.mixer, diagnostic.pipe, munch_sde_result_name(result));
		if (result == MUNCH_SDE_FORMAT && diagnostic.pipe >= 0 && diagnostic.mixer >= 0) {
			struct munch_sde_layout_check layout;
			uint32_t failures = munch_sde_pipe_layout_failures(&snapshot->pipes[diagnostic.pipe],
			    snapshot->mixer_size[diagnostic.mixer], &layout);
			IOLog("MUNCH SDE LAYOUT: bad=%04x first=%s got=%08x\n", failures, layout.name, layout.actual);
			IOLog("MUNCH SDE EXPECT: rule=%s value=%08x alternative=%08x\n",
			    munch_sde_layout_rule_name(layout.rule), layout.expected, layout.alternative);
		}
		if (result == MUNCH_SDE_OK) IOLog("MUNCH SDE ROUTE: stage=%u blend-offset=%03x\n",
		    route.stage, route.stage ? munch_sde_blend_offsets[route.stage - 1] : 0);
		if (fSDE->domainSnapshotValid) {
			const struct munch_smmu_domain *domain = &fSDE->observedDomain;
			const struct munch_smmu_walk *walk = &fSDE->bootWalk;
			IOLog("MUNCH SMMU GLOBAL: scr0=%08x idr0=%08x idr1=%08x\n", fSDE->scr0, fSDE->idr0, fSDE->idr1);
			IOLog("MUNCH SMMU STREAM: index=%u smr=%08x s2cr=%08x cb=%u cbpa=%llx\n",
			    domain->stream, fSDE->streamSmr, domain->s2cr, domain->context, fSDE->contextPhysical);
			IOLog("MUNCH SMMU CTX: cbar=%08x cba2r=%08x sctlr=%08x\n", domain->cbar, domain->cba2r, domain->sctlr);
			IOLog("MUNCH SMMU TABLE: ttbr=%016llx tcr=%08x tcr2=%08x\n", domain->ttbr, domain->tcr, domain->tcr2);
			IOLog("MUNCH SMMU ATTR: mair=%016llx iova=%08x pa=%016llx size=%x\n",
			    domain->mair, fSDE->buffers[0].iova, fSDE->buffers[0].physical, fSDE->buffers[0].length);
			IOLog("MUNCH SMMU FSR: raw=%08x faults=%08x format=%u\n", domain->fsr, munch_smmu_fault_bits(domain->fsr),
			    (domain->fsr & MUNCH_SMMU_FSR_FORMAT) >> 9);
			IOLog("MUNCH SMMU FAULT: far=%016llx syn0=%08x syn1=%08x\n",
			    fSDE->faultAddress, fSDE->fsynr0, fSDE->fsynr1);
			IOLog("MUNCH SMMU WALK: check=%s iova=%08x level=%u pages=%u\n",
			    walk->check ? walk->check : "not-started", walk->iova, walk->level, fSDE->bootVerifiedPages);
			IOLog("MUNCH SMMU PTE: addr=%016llx value=%016llx read=%u access=%s\n", walk->entry, walk->descriptor,
			    (unsigned)walk->read, fSDE->tableAccess ? fSDE->tableAccess : "not-read");
			IOLog("MUNCH SMMU RESULT: pa=%016llx expected=%016llx\n", walk->physical, fSDE->expectedPhysical);
			IOLog("MUNCH SDE MEMORY: pool=%016llx size=%llx check=%s buffers=%s\n",
			    fSDE->poolPhysical, fSDE->poolLength, fSDE->poolCheck ? fSDE->poolCheck : "not-started",
			    fSDE->bufferCheck ? fSDE->bufferCheck : "not-started");
			for (unsigned index = 1; index < 3; index++)
				IOLog("MUNCH SDE BACK%u: pa=%016llx iova=%08x bytes=%x\n", index,
				    fSDE->buffers[index].physical, fSDE->buffers[index].iova, fSDE->buffers[index].length);
			if (result == MUNCH_SDE_OK) return;
		}
		for (unsigned index = 0; index < 6; index++) {
			const struct munch_sde_ctl_state *ctl = &snapshot->ctls[index];
			IOLog("MUNCH SDE CTL%u: top=%08x intf=%08x dsc=%x merge=%x wb=%x cwb=%x cdm=%x start=%x flush=%08x\n",
			    index, ctl->top, ctl->active, ctl->dsc, ctl->merge, ctl->wb, ctl->cwb, ctl->cdm, ctl->start, ctl->flush);
			if (!ctl->active && !ctl->top && (int)index != diagnostic.ctl) continue;
			for (unsigned mixer = 0; mixer < 6; mixer++) {
				if (!snapshot->mixer_size[mixer] && !ctl->layers[0][mixer] && !ctl->layers[1][mixer] &&
				    !ctl->layers[2][mixer] && !ctl->layers[3][mixer]) continue;
				IOLog("MUNCH SDE CTL%u LM%u: size=%08x map=%08x/%08x/%08x/%08x\n",
				    index, mixer, snapshot->mixer_size[mixer], ctl->layers[0][mixer], ctl->layers[1][mixer],
				    ctl->layers[2][mixer], ctl->layers[3][mixer]);
			}
		}
		for (unsigned mixer = 0; mixer < 6; mixer++) {
			if (!snapshot->mixer_size[mixer] && (int)mixer != diagnostic.mixer) continue;
			IOLog("MUNCH SDE LM%u: op=%08x\n", mixer, snapshot->mixers[mixer].opmode);
			for (unsigned stage = 0; stage < MUNCH_SDE_LM_BLEND_COUNT; stage++) {
				IOLog("MUNCH SDE LM%u B%u @%03x: op=%08x alpha=%08x\n", mixer, stage,
				    munch_sde_blend_offsets[stage], snapshot->mixers[mixer].blend_op[stage],
				    snapshot->mixers[mixer].blend_alpha[stage]);
			}
		}
		for (unsigned index = 0; index < 8; index++) {
			const struct munch_sde_pipe_state *pipe = &snapshot->pipes[index];
			if (!pipe->size && !pipe->out_size && !pipe->source) continue;
			IOLog("MUNCH SDE %s: src=%08x stride=%x size=%08x out=%08x fmt=%x unpack=%08x\n",
			    munch_sde_sspps[index].name, pipe->source, pipe->stride, pipe->size, pipe->out_size, pipe->format, pipe->unpack);
			IOLog("MUNCH SDE %s: xy=%08x/%08x op=%x secure=%x multirect=%x\n",
			    munch_sde_sspps[index].name, pipe->src_xy, pipe->out_xy, pipe->opmode, pipe->secure, pipe->multirect);
			IOLog("MUNCH SDE %s MODE: pe-override=%u forbidden=%08x\n", munch_sde_sspps[index].name,
			    (unsigned)((pipe->opmode & MUNCH_SDE_SSPP_PE_OVERRIDE) != 0), pipe->opmode & ~MUNCH_SDE_SSPP_PE_OVERRIDE);
			if (pipe->opmode & MUNCH_SDE_SSPP_PE_OVERRIDE) {
				for (unsigned component = 0; component < MUNCH_SDE_SSPP_PE_COUNT; component++) {
					const struct munch_sde_pixel_ext_state *pixel = &pipe->pixel_ext[component];
					IOLog("MUNCH SDE %s PE %s: lr=%08x tb=%08x required=%08x\n", munch_sde_sspps[index].name,
					    munch_sde_pe_names[component], pixel->lr, pixel->tb, pixel->required);
				}
			}
			if (index < 4) IOLog("MUNCH SDE %s: scaler=%08x csc=%08x pre=%08x decim=%08x\n",
			    munch_sde_sspps[index].name, pipe->scaler, pipe->csc, pipe->pre_downscale, pipe->decimation);
		}
	}
	IOLog("MUNCH SDE DIAG: native display path active; external iOS rdar diagnostics are userland messages\n");
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		if (fSDENativeRequested) {
			IOLockLock(fSwapLock);
			bool inFlight = fSDEInFlight;
			bool queued = !fSDE->nativeTakeover && fSwapsWaiting != 0;
			IOLockUnlock(fSwapLock);
			if (inFlight) {
				if (queued) {
					bool taken = false;
					IOLockLock(fSwapLock);
					if (fSDEInFlight && fSwapsWaiting != 0) {
						unsigned slot = fSwapFirst;
						memcpy(swap, fSwap[slot], SWAP_SIZE);
						memcpy(surfaces, fSwapSurfaces[slot], sizeof(surfaces));
						for (unsigned layer = 0; layer < SWAP_LAYERS; layer++)
							fSwapSurfaces[slot][layer] = NULL;
						taken = true;
					}
					IOLockUnlock(fSwapLock);
					if (taken) {
						fStatSDEQueued++;
						bool gpuReady = true;
						for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
							if (surfaces[layer] && !MiniIOSurfaceDrawn(surfaces[layer], 0)) gpuReady = false;
						}
						if (!gpuReady) waitNativeFrame();
						show(swap, surfaces);
						continue;
					}
				}
				waitNativeFrame(true);
				continue;
			}
		}
		IOLockLock(fSwapLock);
		reportSDEStatus(micros(mach_absolute_time()));
		while (fSwapsWaiting == 0) {
			if (fSDERequested && (!fHardwareFrames || fSDEError != kIOReturnSuccess)) {
				AbsoluteTime deadline;
				clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
				IOLockSleepDeadline(fSwapLock, fSwap, deadline, THREAD_UNINT);
				reportSDEStatus(micros(mach_absolute_time()));
			} else {
				IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
			}
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		if (fSDENativeRequested) {
			for (unsigned layer = 0; layer < SWAP_LAYERS; layer++)
				fSwapSurfaces[fSwapFirst][layer] = NULL;
		}
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		if (!fSDENativeRequested) {
			for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
				if (surfaces[layer] != NULL) MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceScheduleClear();
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint64_t gpuDeadline = 0;
	uint64_t acquisition = 0, composeTime = 0;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report, deferNative = false, gpuBudgetMiss = false;

	if (fSDERequested) {
		IOLockLock(fSwapLock);
		bool canceled = fDisplayBlanked || fSDEError != kIOReturnSuccess;
		IOLockUnlock(fSwapLock);
		if (canceled) {
			dropNativeSurfaces(surfaces);
			finishNativeSwapInOrder(swapWord(swap, SWAP_ID), kIOReturnAborted);
			return;
		}
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (fSDERequested && swapWord(swap, SWAP_SURFACE(layer)) && !surfaces[layer]) {
			dropNativeSurfaces(surfaces);
			finishNativeSwapInOrder(swapWord(swap, SWAP_ID), kIOReturnBadArgument);
			return;
		}
		if (fSDERequested && surfaces[layer] &&
		    (surfaces[layer]->info.bytesPerElement != 4 || surfaces[layer]->info.planeCount != 0)) {
			dropNativeSurfaces(surfaces);
			finishNativeSwapInOrder(swapWord(swap, SWAP_ID), kIOReturnUnsupported);
			return;
		}
		uint64_t gpuWait = SWAP_GPU_MS;
		if (!fSDERequested) {
			if (gpuDeadline == 0) {
				uint64_t budget;
				nanoseconds_to_absolutetime((uint64_t)SWAP_GPU_FRAME_MS * NSEC_PER_MSEC, &budget);
				gpuDeadline = mach_absolute_time() + budget;
			}
			uint64_t now = mach_absolute_time();
			gpuWait = 0;
			if (now < gpuDeadline) {
				uint64_t remaining;
				absolutetime_to_nanoseconds(gpuDeadline - now, &remaining);
				gpuWait = (remaining + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;
			}
		}
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], gpuWait)) {
			if (fSDERequested) {
				dropNativeSurfaces(surfaces);
				finishNativeSwapInOrder(swapWord(swap, SWAP_ID), kIOReturnAborted);
				return;
			}
			gpuBudgetMiss = true;
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	if (gpuBudgetMiss) fStatGPUBudgetMisses++;
	began = ready = mach_absolute_time();
	if (gOnTheBeat && !fSDERequested) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	if (!fSDERequested) {
		IOLockLock(fSwapLock);
		fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
		fSwapsWaiting--;
		IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
		IOLockUnlock(fSwapLock);
	}
	IOLockLock(fLock);
	if (fSDERequested) {
		IOLockLock(fDisplayLock);
		if (!fPanelOn || fSDE->panelCommandFailed) {
			IOLockUnlock(fDisplayLock);
			IOLockUnlock(fLock);
			dropNativeSurfaces(surfaces);
			finishNativeSwapInOrder(swapWord(swap, SWAP_ID), kIOReturnAborted);
			return;
		}
		uint8_t *back = NULL;
		uint64_t acquireBegan = mach_absolute_time();
		enum munch_sde_result result = munchSdeBeginFrame(fSDE, &back);
		acquisition = mach_absolute_time() - acquireBegan;
		if (result != MUNCH_SDE_OK) {
			IOLog("MUNCH SDE: frame acquisition rejected: %s (%u)\n", munch_sde_result_name(result), (unsigned)result);
			IOLockUnlock(fDisplayLock);
			IOLockUnlock(fLock);
			dropNativeSurfaces(surfaces);
			finishNativeSwapInOrder(swapWord(swap, SWAP_ID), kIOReturnIOError);
			return;
		}
		unsigned previous = munch_sde_latest_buffer(&fSDE->presenter);
		if (!munchSdeReplacesFrame(swap, surfaces[0], fWidth, fHeight) &&
		    back != fSDE->pixels[previous]) {
			memcpy(back, fSDE->pixels[previous], (size_t)fRowBytes * fPanelHeight);
			fStatSDEPreserved++;
		}
		fBase = back;
		IOLockUnlock(fDisplayLock);
	}
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics && !fSDERequested) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	if (fSDERequested) {
		if (!fGraphics) IOLog("MUNCH SDE: first compositor frame %u: submitting\n", swapWord(swap, SWAP_ID));
		uint64_t commit = mach_absolute_time();
		composeTime = commit - began;
		deferNative = fSDENativeRequested && fSDE->presenter.pending != 3;
		if (deferNative) fStatSDEOverlapped++;
		if (deferNative) {
			IOLockUnlock(fLock);
			enum munch_sde_result previous = waitNativeFrame();
			IOLockLock(fLock);
			if (previous != MUNCH_SDE_OK) {
				IOLockUnlock(fLock);
				dropNativeSurfaces(surfaces);
				finishNativeSwapInOrder(swapWord(swap, SWAP_ID), kIOReturnAborted);
				return;
			}
			commit = mach_absolute_time();
		}
		IOLockLock(fDisplayLock);
		if (!fPanelOn || fSDE->panelCommandFailed) {
			IOLockUnlock(fDisplayLock);
			IOLockUnlock(fLock);
			dropNativeSurfaces(surfaces);
			finishNativeSwapInOrder(swapWord(swap, SWAP_ID), kIOReturnAborted);
			return;
		}
		if (!fSDENativeRequested) IOLockUnlock(fDisplayLock);
		fStatSDEAcquire += acquisition;
		fStatSDECompose += composeTime;
		enum munch_sde_result result =
		    fSDENativeRequested ? munchSdeSubmitFrame(fSDE) : munchSdeEndFrame(fSDE);
		if (fSDENativeRequested) IOLockUnlock(fDisplayLock);
		if (fSDENativeRequested && !fGraphics) {
			IOLog("MUNCH SDE NATIVE r16: phase=%s result=%s inherited-refresh=%uHz requested-refresh=%uHz panel=%ux%u\n",
			    fSDE->mode.phase, munch_sde_result_name(result), fSDE->mode.refresh_hz,
			    refreshHz(), fPanelWidth, fPanelHeight);
		}
		fStatSDECommit += mach_absolute_time() - commit;
		if (result != MUNCH_SDE_OK) {
			IOLog("MUNCH SDE: commit rejected: %s (%u)\n", munch_sde_result_name(result), (unsigned)result);
			IOLockUnlock(fLock);
			dropNativeSurfaces(surfaces);
			finishNativeSwapInOrder(swapWord(swap, SWAP_ID),
			    result == MUNCH_SDE_TIMEOUT ? kIOReturnTimeout : kIOReturnIOError);
			return;
		}
		if (fSDENativeRequested) {
			memcpy(fSDEInFlightSwap, swap, SWAP_SIZE);
			fSDEInFlightID = swapWord(swap, SWAP_ID);
			fSDEInFlightBegan = began;
			fSDEInFlightReady = ready;
			fSDEInFlightShown = shown;
			fSDEInFlightSubmitted = mach_absolute_time();
			retireNativeSwap(fSDEInFlightID, surfaces);
			IOLockUnlock(fLock);
			return;
		}
	}
	drawn = mach_absolute_time();
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		hakios_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		char liveLine[256];
		snprintf(liveLine, sizeof(liveLine),
		    "MUNCH FB PERF: frames=%u fps=%llu draw_us=%llu gpu_wait_us=%llu beat_wait_us=%llu beat_wait_frames=%u held=%u lost=%u late=%u gpu_budget_misses=%u\n",
		    fStatFrames, (unsigned long long)(fStatFrames * NSEC_PER_SEC / nanos),
		    (unsigned long long)(drawing / fStatFrames / NSEC_PER_USEC),
		    (unsigned long long)(waiting / fStatFrames / NSEC_PER_USEC),
		    (unsigned long long)(fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0),
		    fStatOnBeat, fStatHeld, fStatLost, fStatSwapsLate, fStatGPUBudgetMisses);
		loader_log_line(liveLine);
		if (fSDERequested) {
			IOLockLock(fDisplayLock);
			IOLog("MUNCH SDE PERF r15: frames=%u acquire=%llu compose=%llu commit=%llu clean=%llu wait=%llu us/frame guards=%llu polls=%llu preserved=%u vblank=%llu hw=%llu fallback=%llu source=%s raw=%08x frame=%08x line=%08x tear=%08x\n",
			    fStatFrames, micros(fStatSDEAcquire) / fStatFrames,
			    micros(fStatSDECompose - fStatSDEAcquire) / fStatFrames, micros(fStatSDECommit) / fStatFrames,
			    micros(fSDE->cleanTime) / fStatFrames, micros(fSDE->waitTime) / fStatFrames,
			    fSDE->guardCalls, fSDE->pollChecks, fStatSDEPreserved, fSDE->vblankCount,
			    fSDE->vblankHardwareEvents, fSDE->vblankFallbackEvents,
			    fSDE->vblankFallback ? "timer" : "frame", fSDE->vblankRaw, fSDE->vblankFrameCount,
			    fSDE->vblankLineCount, fSDE->vblankTearLine);
			fStatSDEAcquire = fStatSDECompose = fStatSDECommit = 0;
			fSDE->cleanTime = fSDE->waitTime = fSDE->guardCalls = fSDE->pollChecks = 0;
			fStatSDEPreserved = 0;
			IOLockUnlock(fDisplayLock);
		}
		fStatSwapsLate = 0;
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
		fStatGPUBudgetMisses = 0;
	}
	IOLockUnlock(fLock);
	finishSwap(swapWord(swap, SWAP_ID), kIOReturnSuccess);
}

void
IOMobileFramebuffer::retireNativeSwap(uint32_t swapID, MiniIOSurface ** surfaces)
{
	/* SwapWait releases the IOSurface source; PP_DONE remains the hardware-present fence. */
	IOLockLock(fSwapLock);
	auto &entry = fSwapResults[swapID % 64];
	entry.result = kIOReturnSuccess;
	entry.pending = false;
	fStatSDERetired++;
	fSDEInFlight = true;
	fSDEInFlightID = swapID;
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL) {
			fSDEInFlightSurfaces[layer] = surfaces[layer];
			surfaces[layer] = NULL;
		}
	}
	if (fSwapsWaiting != 0) {
		fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
		fSwapsWaiting--;
	}
	IOLockWakeup(fSwapLock, &fSwapResults, false);
	IOLockWakeup(fSwapLock, &fSwapsWaiting, false);
	IOLockUnlock(fSwapLock);
	if (!fGraphics) {
		fGraphics = true;
		PE_initialize_console(NULL, kPEGraphicsMode);
	}
}

enum munch_sde_result
IOMobileFramebuffer::waitNativeFrame(bool allowQueued)
{
	for (;;) {
		uint32_t sequence = __atomic_load_n(&fSDE->completionIRQSequence, __ATOMIC_ACQUIRE);
		enum munch_sde_result result = munchSdeCompleteFrame(fSDE);
		if (result != MUNCH_SDE_BUSY) {
			completeNativeSwap(result);
			return result;
		}
		if (allowQueued && !fSDE->nativeTakeover) {
			IOLockLock(fSwapLock);
			bool queued = fSwapsWaiting != 0;
			IOLockUnlock(fSwapLock);
			if (queued) return MUNCH_SDE_BUSY;
		}
		bool irqHint = __atomic_load_n(&fSDE->completionIRQPending, __ATOMIC_ACQUIRE) != 0;
		bool irqActive = fSDE->completionIRQAttached &&
		    !__atomic_load_n(&fSDE->completionIRQMasked, __ATOMIC_ACQUIRE);
		if (!irqHint) {
			AbsoluteTime deadline;
			clock_interval_to_deadline(irqActive ? 1 : 250,
			    irqActive ? kMillisecondScale : kMicrosecondScale, &deadline);
			assert_wait_deadline((event_t)&fSDE->completionIRQSequence, THREAD_UNINT, deadline);
			if (sequence == __atomic_load_n(&fSDE->completionIRQSequence, __ATOMIC_ACQUIRE))
				thread_block(THREAD_CONTINUE_NULL);
			else
				clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			IODelay(10);
		}
	}
}

void
IOMobileFramebuffer::finishNativeSwapInOrder(uint32_t swapID, IOReturn result)
{
	IOLockLock(fSwapLock);
	bool olderInFlight = fSDEInFlight && fSDEInFlightID != swapID;
	IOLockUnlock(fSwapLock);
	if (olderInFlight) {
		enum munch_sde_result previous = waitNativeFrame();
		if (previous != MUNCH_SDE_OK) result = kIOReturnAborted;
	}
	finishSwap(swapID, result);
}

void
IOMobileFramebuffer::reportNativeFailure(enum munch_sde_result result, IOReturn swapResult, uint32_t swapID)
{
	MunchSDE *state = fSDE;
	uint32_t intr = 0, ctlStart = 0, ctlFlush = 0, smmuFSR = 0;
	if (state && state->mdp != NULL) {
		intr = state->mdp[MUNCH_SDE_INTR_STATUS / 4];
		if (state->route.ctl < MUNCH_SDE_CTL_COUNT) {
			uint32_t ctl = munch_sde_ctl_offsets[state->route.ctl];
			ctlStart = state->mdp[(ctl + MUNCH_SDE_CTL_START) / 4];
			ctlFlush = state->mdp[(ctl + MUNCH_SDE_CTL_FLUSH) / 4];
		}
	}
	if (state && state->cb != NULL) smmuFSR = state->cb[MUNCH_SMMU_CB_FSR / 4];
	char liveLine[768];
	snprintf(liveLine, sizeof(liveLine),
	    "MUNCH SDE NATIVE FAILURE: swap=%u munch_sde_result=%s(%u) fSDEError=0x%08x->0x%08x "
	    "presenter=pending:%u/front:%u/drawing:%u submitted:%llu/completed:%llu "
	    "irq=attached:%u/masked:%u/pending:%u intr=0x%08x ctl=start:0x%08x/flush:0x%08x smmu_fsr=0x%08x\n",
	    swapID, munch_sde_result_name(result), (unsigned)result, (unsigned)fSDEError, (unsigned)swapResult,
	    state ? state->presenter.pending : 3, state ? state->presenter.front : 3,
	    state ? state->presenter.drawing : 3, state ? state->presenter.submitted : 0,
	    state ? state->presenter.completed : 0, state ? (unsigned)state->completionIRQAttached : 0,
	    state ? __atomic_load_n(&state->completionIRQMasked, __ATOMIC_ACQUIRE) : 0,
	    state ? __atomic_load_n(&state->completionIRQPending, __ATOMIC_ACQUIRE) : 0,
	    intr, ctlStart, ctlFlush, smmuFSR);
	loader_log_line(liveLine);
	IOLog("%s", liveLine);
}

void
IOMobileFramebuffer::dropNativeSurfaces(MiniIOSurface ** surfaces)
{
	if (!fSDENativeRequested) return;
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL) {
			MiniIOSurfaceDrop(surfaces[layer]);
			surfaces[layer] = NULL;
		}
	}
}

void
IOMobileFramebuffer::completeNativeSwap(enum munch_sde_result result)
{
	uint32_t swapID;
	uint64_t began, ready, shown, submitted, drawn = mach_absolute_time();
	IOReturn swapResult = result == MUNCH_SDE_OK ? kIOReturnSuccess :
	    (result == MUNCH_SDE_TIMEOUT ? kIOReturnTimeout : kIOReturnIOError);
	bool notify = false;

	IOLockLock(fSwapLock);
	if (!fSDEInFlight) {
		IOLockUnlock(fSwapLock);
		return;
	}
	swapID = fSDEInFlightID;
	began = fSDEInFlightBegan;
	ready = fSDEInFlightReady;
	shown = fSDEInFlightShown;
	submitted = fSDEInFlightSubmitted;
	if (result == MUNCH_SDE_OK) {
		fSwapsShown++;
		fStatFrames++;
		fStatDrawing += drawn - began;
		fStatSDECompletion += drawn - submitted;
		fHardwareFrames++;
		fHardwareTime = fSDE->completedTime;
		if (fStatSince == 0) fStatSince = began;
		if (!fGraphics) {
			fGraphics = true;
			PE_initialize_console(NULL, kPEGraphicsMode);
		}
		notify = true;
	} else if (swapResult != kIOReturnAborted) {
		reportNativeFailure(result, swapResult, swapID);
		fSDEError = swapResult;
		IOLog("MUNCH SDE: transfer for retired swap %u failed: 0x%x; presentation stopped\n",
		    swapID, swapResult);
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (fSDEInFlightSurfaces[layer] != NULL) {
			MiniIOSurfaceDrop(fSDEInFlightSurfaces[layer]);
			fSDEInFlightSurfaces[layer] = NULL;
		}
	}
	fSDEInFlight = false;
	fSDEInFlightID = 0;
	IOLockUnlock(fSwapLock);
	if (result == MUNCH_SDE_OK) reportSDEPerformance(drawn);
	(void) ready;
	(void) shown;
	if (notify) messageClients(kIOMessageServicePropertyChange);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;
	uint32_t                fLastSwapID;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn message(UInt32 type, IOService *provider, void *argument = NULL) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	if (fClosed) return;
	if (fOwner->fSDERequested && !fOwner->fSDENativeRequested) return;
	uint64_t period, now = mach_absolute_time();
	if (fOwner->fSDENativeRequested) {
		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		fVSyncTime += period;
		if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
			fVSyncTime = now + period;
		}
		thread_call_enter_delayed(fVSyncCall, fVSyncTime);
		return;
	}

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fOwner->fSDERequested) {
		uint64_t count;
		if (!fOwner->presentationTime(&count, &now) || count <= fVSyncCount) {
			armVSync();
			IOLockUnlock(fLock);
			return;
		}
		fVSyncCount = count - 1;
	}
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

IOReturn
IOMobileFramebufferUserClient::message(UInt32 type, IOService *provider, void *argument)
{
	if (type == kIOMessageServicePropertyChange && provider == fOwner && fOwner->fSDERequested) {
		vsync();
		return kIOReturnSuccess;
	}
	return IOUserClient::message(type, provider, argument);
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		{
			IOReturn result = fOwner->submitSwap(args->structureInput, args->structureInputSize);
			if (result == kIOReturnSuccess) {
				IOLockLock(fLock);
				fLastSwapID = swapWord((const uint8_t *)args->structureInput, SWAP_ID);
				IOLockUnlock(fLock);
			}
			return result;
		}

	case kFBMethodSwapWait:
		if (fOwner->fSDERequested && args->scalarInputCount && in[0] > UINT32_MAX) return kIOReturnBadArgument;
		if (gTouchLat) {
			hakios_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		{
			IOLockLock(fLock);
			uint32_t swapID = args->scalarInputCount ? (uint32_t)in[0] : fLastSwapID;
			IOLockUnlock(fLock);
			return fOwner->waitSwap(swapID);
		}
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
		return kIOReturnSuccess;
	case kFBMethodRequestPowerChange:
		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		return fOwner->setDisplayPower((uint32_t) in[0]);
	case kFBMethodChangeFrameInfo:
		if (args->scalarInputCount < 2) return kIOReturnBadArgument;
		{
			uint32_t refresh_hz = in[1] == 60 || in[1] == 120 ? (uint32_t)in[1] :
			    (in[0] == 60 || in[0] == 120 ? (uint32_t)in[0] : 0);
			/* Type 3 is a framework frame-info query/update, not a Hz value. */
			return refresh_hz ? fOwner->changeRefresh(refresh_hz) : kIOReturnSuccess;
		}

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 2;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
			if (args->structureOutputSize >= 8) {
				uint32_t *rates = (uint32_t *)args->structureOutput;
				rates[0] = 60;
				rates[1] = 120;
			}
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);

	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		accelerator->registerService();
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" int
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (gMunchSDEPresent || vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return 0;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
	return 1;
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
