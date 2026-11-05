#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../../xnu/files/iokit/Kernel/munch-sde.h"
#include "../../../xnu/files/iokit/Kernel/munch-sde-mode.h"

using IOReturn = int;
using AbsoluteTime = uint64_t;
static constexpr int kIOReturnSuccess = 0, kIOReturnNotReady = static_cast<int>(0xe00002d8U);
static constexpr int kIOReturnTimeout = static_cast<int>(0xe00002d6U);
static constexpr int kIOReturnAborted = static_cast<int>(0xe00002ebU);
static constexpr int kIOReturnBusy = static_cast<int>(0xe00002d5U), kIOReturnIOError = static_cast<int>(0xe00002caU);
static constexpr int kIOReturnBadArgument = static_cast<int>(0xe00002c2U), kIOReturnNotFound = static_cast<int>(0xe00002f0U);
static constexpr unsigned SWAP_SIZE = 0x46c, SWAP_ID = 0x50, SWAP_LAYERS = 4, SWAP_QUEUE = 2;
static constexpr unsigned SWAP_GPU_MS = 500, SWAP_HELD_MS = 700, FRAME_PAUSE = 90;
static constexpr uint64_t NSEC_PER_SEC = 1000000000, NSEC_PER_MSEC = 1000000;
static constexpr int kMillisecondScale = 1000000, THREAD_UNINT = 0, THREAD_TIMED_OUT = 1;
static constexpr unsigned kIOMessageServicePropertyChange = 1;
static bool gTouchLat = false;
#define SWAP_SURFACE(layer) (0x54 + 4 * (layer))
#define SWAP_SOURCE(layer) (0x64 + 16 * (layer))
#define SWAP_DEST(layer) (0xc4 + 16 * (layer))
#define hakios_tl(...) ((void)0)

static std::string diagnosticLog;
static std::mutex diagnosticLogLock;
static void IOLog(const char *format, ...)
{
	char message[1024];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	std::lock_guard<std::mutex> hold(diagnosticLogLock);
	diagnosticLog += message;
}
extern "C" void loader_log_line(const char *) {}

struct IOLock { std::mutex mutex; std::condition_variable event; };
struct lck_mtx_t {};
static lck_mtx_t hakios_smmu_lock;
static void lck_mtx_lock(lck_mtx_t *) {}
static void lck_mtx_unlock(lck_mtx_t *) {}
static void IOLockLock(IOLock *lock) { lock->mutex.lock(); }
static void IOLockUnlock(IOLock *lock) { lock->mutex.unlock(); }
static void IOLockWakeup(IOLock *lock, void *, bool) { lock->event.notify_all(); }
static int IOLockSleepDeadline(IOLock *lock, void *, uint64_t deadline, int)
{
	std::unique_lock<std::mutex> hold(lock->mutex, std::adopt_lock);
	auto status = lock->event.wait_until(hold, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)));
	hold.release();
	return status == std::cv_status::timeout ? THREAD_TIMED_OUT : 0;
}
static uint64_t mach_absolute_time()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void nanoseconds_to_absolutetime(uint64_t nanos, uint64_t *time) { *time = nanos; }
static void clock_interval_to_deadline(unsigned interval, unsigned scale, uint64_t *deadline)
{
	*deadline = mach_absolute_time() + static_cast<uint64_t>(interval) * scale;
}
static unsigned refreshHz() { return 60; }
static unsigned swapQueue() { return 2; }
static uint32_t swapWord(const uint8_t *swap, unsigned offset)
{
	uint32_t value;
	memcpy(&value, swap + offset, sizeof(value));
	return value;
}
struct MiniIOSurface {
	struct { uint32_t width = 1125, height = 2436, bytesPerElement = 4, planeCount = 0; } info;
};
#include "sde-frame.inc"

static MiniIOSurface *MiniIOSurfaceCopy(uint32_t) { return nullptr; }
static unsigned surfaceDrops;
static void MiniIOSurfaceDrop(MiniIOSurface *) __attribute__((unused));
static void MiniIOSurfaceDrop(MiniIOSurface *) { surfaceDrops++; }
struct MunchSDE {
	bool nativeRequested = false;
	bool nativeTakeover = false;
	volatile uint32_t completionIRQSequence = 0, completionIRQPending = 0, completionIRQMasked = 0;
	bool completionIRQAttached = false;
	munch_sde_result completionResult = MUNCH_SDE_BUSY;
	munch_mode mode = {};
	uint64_t vblankCount = 0, vblankTime = 0;
	bool panelCommandFailed = false, panelCommandActive = false, panelBlanked = false;
	uint64_t completedTime = 42;
	munch_sde_snapshot baseline = {};
	munch_sde_presenter presenter = {};
	munch_sde_refresh refresh = {};
	munch_smmu_domain observedDomain = {};
	munch_smmu_walk bootWalk = {};
	munch_sde_buffer buffers[3] = {};
	const char *tableAccess = nullptr;
	const char *poolCheck = nullptr, *bufferCheck = nullptr;
	uint64_t poolPhysical = 0, poolLength = 0;
	uint64_t contextPhysical = 0, faultAddress = 0, expectedPhysical = 0;
	uint32_t fsynr0 = 0, fsynr1 = 0, bootVerifiedPages = 0;
	uint32_t scr0 = 0, idr0 = 0, idr1 = 0, streamSmr = 0;
	bool snapshotValid = false, domainSnapshotValid = false;
};
using event_t = void *;
static constexpr int kMicrosecondScale = 1000, THREAD_AWAKENED = 0;
static constexpr void *THREAD_CONTINUE_NULL = nullptr;
static std::mutex nativeWaitLock;
static std::condition_variable nativeWaitEvent;
static event_t nativeWaitAddress;
static bool nativeWaitReady;
static uint64_t nativeWaitDeadline;
static std::function<void()> beforeNativeWait;
static std::atomic<unsigned> nativeBlocks{0};
static void munchSdeWakeup(void *event)
{
	std::lock_guard<std::mutex> hold(nativeWaitLock);
	if (nativeWaitAddress == event) {
		nativeWaitReady = true;
		nativeWaitEvent.notify_all();
	}
}
static void assert_wait_deadline(event_t event, int, uint64_t deadline)
{
	if (beforeNativeWait) {
		auto callback = std::move(beforeNativeWait);
		beforeNativeWait = {};
		callback();
	}
	std::lock_guard<std::mutex> hold(nativeWaitLock);
	nativeWaitAddress = event;
	nativeWaitReady = false;
	nativeWaitDeadline = deadline;
}
static void thread_block(void *)
{
	std::unique_lock<std::mutex> hold(nativeWaitLock);
	nativeBlocks.fetch_add(1);
	nativeWaitEvent.wait_until(hold, std::chrono::steady_clock::time_point(
	    std::chrono::nanoseconds(nativeWaitDeadline)), [] { return nativeWaitReady; });
	nativeWaitAddress = nullptr;
}
static void *current_thread() { return nullptr; }
static void clear_wait(void *, int)
{
	std::lock_guard<std::mutex> hold(nativeWaitLock);
	nativeWaitAddress = nullptr;
}
static void IODelay(unsigned usec) { std::this_thread::sleep_for(std::chrono::microseconds(usec)); }
static munch_sde_result munchSdeCompleteFrame(MunchSDE *state)
{
	return __atomic_load_n(&state->completionResult, __ATOMIC_ACQUIRE);
}
static bool munchSdeSampleVBlank(MunchSDE *state)
{
	return state && state->nativeRequested && state->mode.configured &&
	    !state->panelBlanked && !state->mode.failed && state->vblankCount != 0;
}
static constexpr int kPEGraphicsMode = 1;
static unsigned consoleChanges;
static void PE_initialize_console(void *info, int mode)
{
	assert(info == nullptr && mode == kPEGraphicsMode);
	consoleChanges++;
}
static uint32_t gMunchSDEPresent;
static struct {
	uintptr_t v_baseaddr;
	uint32_t v_width, v_height, v_rowbytes, v_depth;
} vinfo;

#include "sde-blit.inc"

struct IOMobileFramebuffer {
	IOLock storage;
	IOLock *fSwapLock = &storage;
	bool fSDERequested = true;
	bool fSDENativeRequested = false;
	bool fDisplayBlanked = false, fPanelOn = true;
	IOLock displayStorage;
	IOLock *fDisplayLock = &displayStorage;
	IOReturn commandResult = kIOReturnSuccess;
	uint8_t failCommand = 0;
	std::vector<unsigned> commands;
	bool fGraphics = false;
	IOReturn fSDEError = kIOReturnSuccess;
	munch_sde_result fSDEInitResult = MUNCH_SDE_OK;
	const char *fSDEInitPhase = "ready";
	uint64_t fSDESwapRequests = 0, fSDEAcceptedRequests = 0, fSDEStatusTime = 0;
	uint32_t fSDELastSwapID = 0, fSDELastSwapSize = 0;
	IOReturn fSDELastSwapResult = kIOReturnSuccess;
	bool fSDEStatusLogged = false;
	uint32_t fStatSDEOverlapped = 0, fStatSDERetired = 0, fStatSDEQueued = 0;
	uint64_t fStatSwapsLate = 0, fSwapTime = 0, fStatHeld = 0, fStatLost = 0;
	unsigned fSwapsWaiting = 0, fSwapFirst = 0;
	uint8_t fSwap[SWAP_QUEUE][SWAP_SIZE] = {};
	MiniIOSurface *fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS] = {};
	struct { uint32_t id; IOReturn result; bool pending, valid; } fSwapResults[64] = {};
	bool fSDEInFlight = false;
	uint32_t fSDEInFlightID = 0;
	MiniIOSurface *fSDEInFlightSurfaces[SWAP_LAYERS] = {};
	MunchSDE runtime;
	MunchSDE *fSDE = &runtime;
	uint64_t fHardwareFrames = 0, fHardwareTime = 0;
	unsigned notifications = 0;
	void messageClients(unsigned type) { assert(type == kIOMessageServicePropertyChange); notifications++; }
	IOReturn present(const uint8_t *swap);
	IOReturn submitSwap(const void *bytes, uint32_t size);
	IOReturn waitSwap(uint32_t swapID);
	void finishSwap(uint32_t swapID, IOReturn result);
	bool presentationTime(uint64_t *count, uint64_t *time);
	void reportSDEStatus(uint64_t now);
	void finishNativeSwapInOrder(uint32_t swapID, IOReturn result);
	void retireNativeSwap(uint32_t swapID, MiniIOSurface **surfaces);
	enum munch_sde_result waitNativeFrame(bool allowQueued = false);
	void reportNativeFailure(munch_sde_result, IOReturn, uint32_t) {}
	unsigned nativeCompletions = 0;
	void completeNativeSwap(munch_sde_result result)
	{
		assert(result != MUNCH_SDE_BUSY);
		nativeCompletions++;
		fSDEInFlight = false;
	}
	void dropNativeSurfaces(MiniIOSurface **surfaces);
	IOReturn setDisplayPower(uint32_t state);
	IOReturn sendDcsCommand(uint8_t command, uint8_t parameter, bool hasParameter)
	{
		assert(fSDE->panelCommandActive || !fSDERequested);
		commands.push_back(command | (static_cast<unsigned>(parameter) << 8) | (static_cast<unsigned>(hasParameter) << 16));
		return command == failCommand ? kIOReturnTimeout : commandResult;
	}
};

#include "sde-swap.inc"
#include "sde-retire.inc"
#include "sde-wait.inc"

static munch_sde_result panelPrepareResult = MUNCH_SDE_OK;
static munch_sde_result munchSdePreparePanel(MunchSDE *state)
{
	assert(!state->panelCommandActive);
	if (panelPrepareResult == MUNCH_SDE_OK) state->panelCommandActive = true;
	return panelPrepareResult;
}
static munch_sde_result munchSdeFinishPanel(MunchSDE *state, bool on, bool completed)
{
	assert(state->panelCommandActive);
	state->panelCommandActive = false;
	state->panelCommandFailed = !completed;
	if (completed) state->panelBlanked = !on;
	return completed ? MUNCH_SDE_OK : MUNCH_SDE_FAULT;
}
static unsigned panelDelay;
static void IOSleep(unsigned msec) { panelDelay += msec; }
#include "sde-power.inc"

static IOReturn enqueue(IOMobileFramebuffer &provider, uint32_t id)
{
	uint8_t swap[SWAP_SIZE] = {};
	memcpy(swap + SWAP_ID, &id, sizeof(id));
	return provider.present(swap);
}

static void testFullFrameComposition()
{
	struct Case { uint32_t sourceWidth, sourceHeight, destinationWidth, destinationHeight; };
	const Case cases[] = {{1800, 2400, 1080, 2400}, {5, 7, 3, 4}, {7, 5, 4, 3}, {8, 6, 8, 6}};

	for (const Case &test : cases) {
		const uint32_t sourceStride = test.sourceWidth * 4 + 16;
		const uint32_t destinationStride = test.destinationWidth * 4 + 20;
		std::vector<uint8_t> source((size_t)sourceStride * test.sourceHeight, 0xa5);
		std::vector<uint8_t> destination((size_t)destinationStride * test.destinationHeight, 0xcd);
		for (uint32_t y = 0; y < test.sourceHeight; y++) {
			auto *row = reinterpret_cast<uint32_t *>(source.data() + (size_t)y * sourceStride);
			for (uint32_t x = 0; x < test.sourceWidth; x++)
				row[x] = y * 10000 + x;
		}

		bool copied = munchSdeComposeFullFrame(source.data(), test.sourceWidth, test.sourceHeight, sourceStride,
		    destination.data(), test.destinationWidth, test.destinationHeight, destinationStride);
		bool sameSize = test.sourceWidth == test.destinationWidth && test.sourceHeight == test.destinationHeight;
		assert(copied == sameSize);
		if (!copied) {
			for (uint8_t byte : destination) assert(byte == 0xcd);
			continue;
		}
		for (uint32_t y = 0; y < test.destinationHeight; y++) {
			const uint32_t sourceY = (uint64_t)y * test.sourceHeight / test.destinationHeight;
			auto *row = reinterpret_cast<const uint32_t *>(destination.data() + (size_t)y * destinationStride);
			auto *input = reinterpret_cast<const uint32_t *>(source.data() + (size_t)sourceY * sourceStride);
			for (uint32_t x = 0; x < test.destinationWidth; x++) {
				const uint32_t sourceX = (uint64_t)x * test.sourceWidth / test.destinationWidth;
				assert(row[x] == input[sourceX]);
			}
			for (uint32_t byte = test.destinationWidth * 4; byte < destinationStride; byte++)
				assert(destination[(size_t)y * destinationStride + byte] == 0xcd);
		}
	}
	assert(!munchSdeComposeFullFrame(nullptr, 1, 1, 4, nullptr, 1, 1, 4));
	puts("Native full-frame copy preserves padding and delegates all scaling to row workers");
}

static void testNativeQueueWakeups()
{
	for (bool irq : {false, true}) {
		IOMobileFramebuffer native;
		native.fSDENativeRequested = native.fSDEInFlight = true;
		native.runtime.completionIRQAttached = irq;
		unsigned previousBlocks = nativeBlocks.load();
		auto waiting = std::async(std::launch::async, [&native] { return native.waitNativeFrame(true); });
		auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (nativeBlocks.load() == previousBlocks && std::chrono::steady_clock::now() < limit)
			std::this_thread::yield();
		assert(nativeBlocks.load() != previousBlocks);
		assert(enqueue(native, 1) == kIOReturnSuccess);
		assert(waiting.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
		assert(waiting.get() == MUNCH_SDE_BUSY);
		assert(native.fSDEInFlight && native.fSwapsWaiting == 1 && native.nativeCompletions == 0);

		auto draining = std::async(std::launch::async, [&native] { return native.waitNativeFrame(); });
		assert(draining.wait_for(std::chrono::milliseconds(5)) == std::future_status::timeout);
		__atomic_store_n(&native.runtime.completionResult, MUNCH_SDE_OK, __ATOMIC_RELEASE);
		__atomic_add_fetch(&native.runtime.completionIRQSequence, 1u, __ATOMIC_RELEASE);
		munchSdeWakeup((void *)&native.runtime.completionIRQSequence);
		assert(draining.get() == MUNCH_SDE_OK && native.nativeCompletions == 1);
	}
	{
		IOMobileFramebuffer native;
		native.fSDENativeRequested = native.fSDEInFlight = true;
		beforeNativeWait = [&native] { assert(enqueue(native, 1) == kIOReturnSuccess); };
		unsigned previousBlocks = nativeBlocks.load();
		assert(native.waitNativeFrame(true) == MUNCH_SDE_BUSY);
		assert(nativeBlocks.load() == previousBlocks && native.fSDEInFlight && !native.nativeCompletions);
		__atomic_store_n(&native.runtime.completionResult, MUNCH_SDE_FAULT, __ATOMIC_RELEASE);
		assert(native.waitNativeFrame(true) == MUNCH_SDE_FAULT && native.nativeCompletions == 1);
	}
	puts("Queued swaps interrupt completion waits without retiring pending DMA or losing wakeups");
}

static void testPanelPowerAndCopies()
{
	IOMobileFramebuffer panel;
	assert(panel.setDisplayPower(2) == kIOReturnBadArgument);
	assert(panel.setDisplayPower(1) == kIOReturnSuccess && panel.commands.empty());
	assert(enqueue(panel, 1) == kIOReturnSuccess);
	auto waiting = std::async(std::launch::async, [&panel] { return panel.waitSwap(1); });
	auto submitting = std::async(std::launch::async, [&panel] { return enqueue(panel, 2); });
	assert(submitting.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
	assert(panel.setDisplayPower(0) == kIOReturnSuccess);
	assert(panel.fDisplayBlanked && !panel.fPanelOn && panel.runtime.panelBlanked);
	assert(panel.commands == std::vector<unsigned>{0x28, 0x12053, 0x10} && panelDelay == 140);
	assert(submitting.get() == kIOReturnNotReady);
	assert(enqueue(panel, 3) == kIOReturnNotReady && panel.fSwapsWaiting == 1);
	panel.finishSwap(1, kIOReturnAborted);
	assert(waiting.get() == kIOReturnAborted && panel.fSDEError == kIOReturnSuccess);
	assert(!panel.fHardwareFrames && !panel.notifications && !panel.fGraphics && !panel.fSwapsWaiting);
	assert(panel.setDisplayPower(0) == kIOReturnSuccess && panel.commands.size() == 3);
	assert(panel.setDisplayPower(1) == kIOReturnSuccess && !panel.fDisplayBlanked && panel.fPanelOn);
	assert((panel.commands == std::vector<unsigned>{0x28, 0x12053, 0x10, 0x11, 0x12053, 0x29}));
	assert(panelDelay == 260);
	assert(enqueue(panel, 4) == kIOReturnSuccess);
	panel.finishSwap(4, kIOReturnSuccess);
	assert(panel.fHardwareFrames == 1 && panel.notifications == 1);
	uint64_t count, time;
	assert(panel.presentationTime(&count, &time));
	assert(panel.setDisplayPower(0) == kIOReturnSuccess);
	assert(!panel.presentationTime(&count, &time));
	panel.commandResult = kIOReturnTimeout;
	assert(panel.setDisplayPower(1) == kIOReturnTimeout);
	assert(panel.fDisplayBlanked && !panel.fPanelOn && panel.runtime.panelCommandFailed);
	assert(panel.setDisplayPower(0) == kIOReturnNotReady);
	assert(enqueue(panel, 5) != kIOReturnSuccess);
	IOMobileFramebuffer busy;
	panelPrepareResult = MUNCH_SDE_BUSY;
	assert(busy.setDisplayPower(0) == kIOReturnBusy && busy.commands.empty() && busy.fPanelOn);
	panelPrepareResult = MUNCH_SDE_OK;
	IOMobileFramebuffer legacyPanel;
	legacyPanel.fSDERequested = false;
	assert(legacyPanel.setDisplayPower(0) == kIOReturnSuccess);
	assert((legacyPanel.commands == std::vector<unsigned>{0x28, 0x12053, 0x10}));
	assert(legacyPanel.setDisplayPower(1) == kIOReturnSuccess);
	assert((legacyPanel.commands == std::vector<unsigned>{0x28, 0x12053, 0x10, 0x11, 0x12053, 0x29}));
	for (uint8_t command : {0x11, 0x53, 0x29}) {
		IOMobileFramebuffer failedWake;
		assert(failedWake.setDisplayPower(0) == kIOReturnSuccess);
		failedWake.failCommand = command;
		assert(failedWake.setDisplayPower(1) == kIOReturnTimeout);
		assert(failedWake.fDisplayBlanked && !failedWake.fPanelOn && failedWake.runtime.panelCommandFailed);
		assert((failedWake.commands.back() & 0xff) == command);
		assert(enqueue(failedWake, 1) != kIOReturnSuccess);
	}
	uint8_t swap[SWAP_SIZE] = {};
	MiniIOSurface surface;
	assert(!munchSdeReplacesFrame(swap, nullptr, 1125, 2436));
	assert(munchSdeReplacesFrame(swap, &surface, 1125, 2436));
	const int32_t rectangles[][4] = {{0, 0, 1125, 2436}, {1, 0, 1125, 2436}, {0, 1, 1125, 2436},
	    {0, 0, 1124, 2436}, {0, 0, 1125, 2435}, {-1, 0, 1125, 2436}, {0, 0, 1126, 2436}};
	for (unsigned index = 0; index < sizeof(rectangles) / sizeof(rectangles[0]); index++) {
		memcpy(swap + SWAP_DEST(0), rectangles[index], 16);
		assert(munchSdeReplacesFrame(swap, &surface, 1125, 2436) == (index == 0));
	}
	memcpy(swap + SWAP_DEST(0), rectangles[0], 16);
	for (unsigned index = 0; index < sizeof(rectangles) / sizeof(rectangles[0]); index++) {
		memcpy(swap + SWAP_SOURCE(0), rectangles[index], 16);
		assert(munchSdeReplacesFrame(swap, &surface, 1125, 2436) == (index == 0));
	}
	surface.info.bytesPerElement = 2;
	assert(!munchSdeReplacesFrame(swap, &surface, 1125, 2436));
	surface.info.bytesPerElement = 4;
	surface.info.planeCount = 1;
	assert(!munchSdeReplacesFrame(swap, &surface, 1125, 2436));
	puts("Panel blank/resume, queue cancellation/wakeup and full-frame copy elision passed");
}

int main()
{
	testFullFrameComposition();
	testNativeQueueWakeups();
	{
		IOMobileFramebuffer native;
		native.fSDENativeRequested = true;
		uint8_t swap[SWAP_SIZE] = {};
		uint32_t id = 42;
		memcpy(swap + SWAP_ID, &id, sizeof(id));
		memcpy(native.fSwap[0], swap, sizeof(swap));
		native.fSwapResults[id % 64] = {id, kIOReturnNotReady, true, true};
		native.fSwapsWaiting = 1;
		MiniIOSurface surface;
		MiniIOSurface *surfaces[SWAP_LAYERS] = {&surface};
		unsigned dropsBefore = surfaceDrops;
		native.retireNativeSwap(id, surfaces);
		assert(native.waitSwap(id) == kIOReturnSuccess);
		assert(native.fSDEInFlight && native.fSDEInFlightID == id);
		assert(native.fSwapsWaiting == 0 && native.fSwapFirst == 1);
		assert(native.fHardwareFrames == 0 && native.notifications == 0 && native.fGraphics);
		assert(native.fSDEInFlightSurfaces[0] == &surface && surfaces[0] == nullptr);
		assert(surfaceDrops == dropsBefore);
		assert(enqueue(native, 43) == kIOReturnSuccess && native.fSwapsWaiting == 1);
		consoleChanges = 0;
	}
	{
		IOMobileFramebuffer native;
		native.fSDENativeRequested = native.runtime.nativeRequested = true;
		native.runtime.mode.configured = true;
		native.runtime.vblankCount = 7;
		native.runtime.vblankTime = 123;
		uint64_t nativeCount, nativeTime;
		assert(native.presentationTime(&nativeCount, &nativeTime));
		assert(nativeCount == 7 && nativeTime == 123 && !native.fHardwareFrames);
		native.runtime.panelBlanked = true;
		assert(!native.presentationTime(&nativeCount, &nativeTime));
		native.runtime.panelBlanked = false;
		native.runtime.mode.failed = true;
		assert(!native.presentationTime(&nativeCount, &nativeTime));
		native.fDisplayLock = nullptr;
		assert(!native.presentationTime(&nativeCount, &nativeTime));
	}
	IOMobileFramebuffer provider;
	uint64_t count, time;
	assert(!provider.presentationTime(&count, &time));
	assert(provider.waitSwap(0) == kIOReturnNotFound);
	assert(provider.waitSwap(1) == kIOReturnNotFound);
	assert(enqueue(provider, 0) == kIOReturnBadArgument);
	assert(enqueue(provider, 1) == kIOReturnSuccess);
	auto waiter = std::async(std::launch::async, [&provider] { return provider.waitSwap(1); });
	assert(waiter.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
	assert(provider.fSwapsWaiting == 1 && provider.notifications == 0 && !provider.fGraphics && consoleChanges == 0);
	provider.finishSwap(1, kIOReturnSuccess);
	assert(provider.fGraphics && consoleChanges == 1);
	assert(waiter.get() == kIOReturnSuccess);
	assert(provider.fSwapsWaiting == 0 && provider.notifications == 1);
	assert(provider.presentationTime(&count, &time) && count == 1 && time == 42);
	assert(enqueue(provider, 1) == kIOReturnBadArgument);
	assert(enqueue(provider, 65) == kIOReturnSuccess);
	assert(provider.waitSwap(1) == kIOReturnNotFound);
	assert(enqueue(provider, 66) == kIOReturnTimeout);
	assert(provider.fSwapsWaiting == 1 && provider.waitSwap(66) == kIOReturnNotFound);
	provider.finishSwap(65, kIOReturnTimeout);
	assert(provider.waitSwap(65) == kIOReturnTimeout && provider.fSwapsWaiting == 0);
	assert(provider.notifications == 1 && !provider.presentationTime(&count, &time));
	assert(count == 1 && time == 42);
	assert(enqueue(provider, 66) == kIOReturnTimeout && provider.fSwapsWaiting == 0);
	IOMobileFramebuffer rejected;
	rejected.fSDEError = kIOReturnNotReady;
	assert(enqueue(rejected, 1) == kIOReturnNotReady && rejected.fSwapsWaiting == 0);
	assert(!rejected.fGraphics && consoleChanges == 1);
	IOMobileFramebuffer failed;
	assert(enqueue(failed, 1) == kIOReturnSuccess);
	failed.finishSwap(1, kIOReturnTimeout);
	assert(!failed.fGraphics && consoleChanges == 1);
	IOMobileFramebuffer successful;
	assert(enqueue(successful, 1) == kIOReturnSuccess);
	successful.finishSwap(1, kIOReturnSuccess);
	assert(successful.fGraphics && consoleChanges == 2);
	assert(enqueue(successful, 2) == kIOReturnSuccess);
	successful.finishSwap(2, kIOReturnSuccess);
	assert(consoleChanges == 2);
	IOMobileFramebuffer legacy;
	legacy.fSDERequested = false;
	assert(legacy.waitSwap(0) == kIOReturnSuccess);
	assert(enqueue(legacy, 1) == kIOReturnSuccess);
	assert(!legacy.fSwapResults[1].valid);
	uint8_t framebuffer[16] = {}, pixels[16];
	memset(pixels, 0xa5, sizeof(pixels));
	vinfo = {(uintptr_t)framebuffer, 2, 2, 8, 32};
	gMunchSDEPresent = 1;
	assert(MiniIOMobileFramebufferShow(pixels, 2, 2, 8) == 0);
	for (uint8_t pixel : framebuffer) assert(pixel == 0);
	gMunchSDEPresent = 0;
	assert(MiniIOMobileFramebufferShow(pixels, 2, 2, 8) == 1);
	assert(memcmp(framebuffer, pixels, sizeof(pixels)) == 0);
	vinfo.v_baseaddr = 0;
	assert(MiniIOMobileFramebufferShow(pixels, 2, 2, 8) == 0);
	uint8_t unaligned[SWAP_SIZE + 1] = {};
	uint32_t swapID = 7;
	memcpy(unaligned + 1 + SWAP_ID, &swapID, sizeof(swapID));
	IOMobileFramebuffer observed;
	assert(observed.submitSwap(unaligned + 1, SWAP_SIZE) == kIOReturnSuccess);
	assert(observed.fSDESwapRequests == 1 && observed.fSDEAcceptedRequests == 1);
	assert(observed.fSDELastSwapID == 7 && observed.fSDELastSwapSize == SWAP_SIZE);
	observed.finishSwap(7, kIOReturnSuccess);
	assert(observed.submitSwap(unaligned + 1, SWAP_SIZE - 1) == kIOReturnBadArgument);
	assert(observed.submitSwap(nullptr, SWAP_SIZE) == kIOReturnBadArgument);
	assert(observed.fSDESwapRequests == 3 && observed.fSDEAcceptedRequests == 1);
	rejected.fSDEInitPhase = "inherited-route";
	rejected.fSDEInitResult = MUNCH_SDE_UNSUPPORTED;
	rejected.runtime.baseline.revision = 0x60000000;
	rejected.runtime.snapshotValid = true;
	assert(rejected.submitSwap(unaligned + 1, SWAP_SIZE) == kIOReturnNotReady);
	diagnosticLog.clear();
	rejected.reportSDEStatus(0);
	assert(diagnosticLog.find("init=unsupported phase=inherited-route error=0xe00002d8") != std::string::npos);
	assert(diagnosticLog.find("SwapEnd=1 accepted=0 id=7 bytes=1132 rc=0xe00002d8") != std::string::npos);
	assert(diagnosticLog.find("completed=0 queued=0") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE SAVED: rev=60000000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE STATUS r15:") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE AUTO: enabled=0 frames=0 vsync=00000000\n") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE AUTO STATE: phase=inherited owned=0 paused=0") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE ROUTE: check=dsi-link ctl=-1 lm=-1 pipe=-1") != std::string::npos);
	auto firstStatus = diagnosticLog;
	rejected.reportSDEStatus(4999999);
	assert(diagnosticLog == firstStatus);
	rejected.reportSDEStatus(5000000);
	assert(diagnosticLog == firstStatus + firstStatus);
	IOMobileFramebuffer routeRejected;
	routeRejected.fSDEInitPhase = "inherited-route";
	routeRejected.fSDEInitResult = MUNCH_SDE_ROUTE;
	routeRejected.fSDEError = kIOReturnNotReady;
	auto &snapshot = routeRejected.runtime.baseline;
	routeRejected.runtime.snapshotValid = true;
	snapshot.revision = 0x60000000;
	snapshot.tear_enable = snapshot.autorefresh = 1;
	snapshot.dsi_ctrl = 0x1f5;
	snapshot.ctls[2].active = 2;
	snapshot.ctls[2].top = 0x20001;
	snapshot.ctls[2].dsc = 1;
	snapshot.ctls[2].layers[0][3] = 0x1040000;
	snapshot.ctls[2].layers[1][3] = 0x10000;
	snapshot.ctls[2].layers[2][3] = 0x100;
	snapshot.ctls[2].layers[3][3] = 0x10;
	snapshot.mixer_size[3] = snapshot.pipes[4].size = snapshot.pipes[4].out_size = 0x9600438;
	snapshot.pipes[4].source = 0x9c400000;
	snapshot.pipes[4].stride = 4320;
	snapshot.pipes[4].format = 0x236ff;
	snapshot.pipes[4].unpack = 0x03020001;
	snapshot.mixers[3].opmode = 2;
	snapshot.mixers[3].blend_op[0] = 0x100;
	snapshot.mixers[3].blend_alpha[0] = 0x00ff0000;
	snapshot.mixers[3].blend_op[3] = 0x400;
	snapshot.mixers[3].blend_alpha[3] = 0x00ff0000;
	snapshot.pipes[0] = snapshot.pipes[4];
	snapshot.pipes[0].scaler = 0x4401;
	snapshot.pipes[0].csc = 3;
	snapshot.pipes[0].pre_downscale = 2;
	snapshot.pipes[0].decimation = 0x101;
	auto before = snapshot;
	diagnosticLog.clear();
	routeRejected.reportSDEStatus(0);
	assert(diagnosticLog.find("MUNCH SDE ROUTE: check=ctl-mode ctl=2 lm=-1 pipe=-1") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE CTL2: top=00020001 intf=00000002 dsc=1 merge=0 wb=0 cwb=0 cdm=0 start=0 flush=00000000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE CTL2 LM3: size=09600438 map=01040000/00010000/00000100/00000010") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE LM3: op=00000002") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE LM3 B0 @020: op=00000100 alpha=00ff0000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE LM3 B3 @068: op=00000400 alpha=00ff0000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE LM3 B10 @110: op=00000000 alpha=00000000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE VIG0: scaler=00004401 csc=00000003 pre=00000002 decim=00000101") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE DMA0: src=9c400000 stride=10e0 size=09600438 out=09600438 fmt=236ff unpack=03020001") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE DMA0: xy=00000000/00000000 op=0 secure=0 multirect=0") != std::string::npos);
	for (unsigned index = 0; index < 6; index++) {
		assert(diagnosticLog.find("MUNCH SDE CTL" + std::to_string(index) + ":") != std::string::npos);
	}
	assert(!memcmp(&snapshot, &before, sizeof(before)));
	auto routeStatus = diagnosticLog;
	routeRejected.reportSDEStatus(4999999);
	assert(diagnosticLog == routeStatus);
	routeRejected.reportSDEStatus(5000000);
	assert(diagnosticLog == routeStatus + routeStatus);
	IOMobileFramebuffer revisionRejected;
	revisionRejected.runtime.snapshotValid = true;
	revisionRejected.fSDEInitPhase = "inherited-route";
	revisionRejected.fSDEInitResult = MUNCH_SDE_UNSUPPORTED;
	diagnosticLog.clear();
	revisionRejected.reportSDEStatus(0);
	assert(diagnosticLog.find("MUNCH SDE SAVED: rev=00000000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE ROUTE: check=revision") != std::string::npos);
	IOMobileFramebuffer layoutRejected;
	layoutRejected.runtime = routeRejected.runtime;
	layoutRejected.fSDEInitPhase = "inherited-route";
	layoutRejected.fSDEInitResult = MUNCH_SDE_FORMAT;
	layoutRejected.fSDEError = kIOReturnNotReady;
	auto &layoutSnapshot = layoutRejected.runtime.baseline;
	layoutSnapshot.ctls[2].top = 0x20000;
	layoutSnapshot.ctls[2].layers[1][3] = layoutSnapshot.ctls[2].layers[2][3] = layoutSnapshot.ctls[2].layers[3][3] = 0;
	layoutSnapshot.pipes[4].format = 0x23ff7;
	auto layoutBefore = layoutSnapshot;
	diagnosticLog.clear();
	layoutRejected.reportSDEStatus(0);
	assert(diagnosticLog.find("MUNCH SDE ROUTE: check=pipe-format ctl=2 lm=3 pipe=4") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE LAYOUT: bad=2000 first=pipe-format got=00023ff7") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE EXPECT: rule=one-of value=000236ff alternative=000237ff") != std::string::npos);
	assert(!memcmp(&layoutSnapshot, &layoutBefore, sizeof(layoutBefore)));
	auto layoutStatus = diagnosticLog;
	layoutRejected.reportSDEStatus(4999999);
	assert(diagnosticLog == layoutStatus);
	layoutRejected.reportSDEStatus(5000000);
	assert(diagnosticLog == layoutStatus + layoutStatus);
	layoutSnapshot.pipes[4].stride = 0x100010e1;
	diagnosticLog.clear();
	layoutRejected.reportSDEStatus(10000000);
	assert(diagnosticLog.find("MUNCH SDE LAYOUT: bad=2600 first=pipe-stride-max got=100010e1") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE EXPECT: rule=at-most value=0000ffff alternative=00000000") != std::string::npos);
	assert(!layoutRejected.fHardwareFrames && !layoutRejected.fSDEAcceptedRequests);
	layoutSnapshot.pipes[4].stride = 4320;
	layoutSnapshot.pipes[4].format = 0x236ff;
	layoutSnapshot.pipes[4].opmode = 0x80000000;
	diagnosticLog.clear();
	layoutRejected.reportSDEStatus(15000000);
	assert(diagnosticLog.find("MUNCH SDE LAYOUT: bad=920000 first=pipe-pe-c0-size got=00000000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE EXPECT: rule=equal value=09600438 alternative=00000000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE DMA0 MODE: pe-override=1 forbidden=00000000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE DMA0 PE C1C2: lr=00000000 tb=00000000 required=00000000") != std::string::npos);
	for (unsigned component = 0; component < 3; component++)
		layoutSnapshot.pipes[4].pixel_ext[component].required = 0x09600438;
	diagnosticLog.clear();
	layoutRejected.reportSDEStatus(20000000);
	assert(diagnosticLog.find("MUNCH SDE ROUTE: check=ok ctl=2 lm=3 pipe=4") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE DMA0 PE C3: lr=00000000 tb=00000000 required=09600438") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE LAYOUT:") == std::string::npos);
	layoutSnapshot.pipes[4].opmode |= 1u << 13;
	diagnosticLog.clear();
	layoutRejected.reportSDEStatus(25000000);
	assert(diagnosticLog.find("MUNCH SDE LAYOUT: bad=0020 first=pipe-opmode got=80002000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE EXPECT: rule=bits-clear value=7fffffff alternative=00000000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE DMA0 MODE: pe-override=1 forbidden=00002000") != std::string::npos);
	layoutSnapshot.pipes[4].opmode = 0;
	diagnosticLog.clear();
	layoutRejected.reportSDEStatus(30000000);
	assert(diagnosticLog.find("MUNCH SDE ROUTE: check=ok ctl=2 lm=3 pipe=4") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE DMA0 MODE: pe-override=0 forbidden=00000000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE LAYOUT:") == std::string::npos);
	layoutSnapshot.autorefresh = 0x80000001;
	layoutSnapshot.tear_vsync = 0x00181234;
	diagnosticLog.clear();
	layoutRejected.reportSDEStatus(35000000);
	assert(diagnosticLog.find("MUNCH SDE ROUTE: check=ok ctl=2 lm=3 pipe=4") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE AUTO: enabled=1 frames=1 vsync=00181234\n") != std::string::npos);
	layoutRejected.runtime.refresh.phase = "verify-off";
	layoutRejected.runtime.refresh.owned = true;
	layoutRejected.runtime.refresh.status = 0x80;
	layoutRejected.runtime.refresh.line = 2400;
	diagnosticLog.clear();
	layoutRejected.reportSDEStatus(40000000);
	assert(diagnosticLog.find("phase=verify-off owned=1 paused=0 status=00000080 line=2400") != std::string::npos);
	layoutSnapshot.autorefresh = 0x80000002;
	diagnosticLog.clear();
	layoutRejected.reportSDEStatus(45000000);
	assert(diagnosticLog.find("MUNCH SDE ROUTE: check=autorefresh-config ctl=2 lm=3 pipe=4") != std::string::npos);
	IOMobileFramebuffer smmuRejected;
	smmuRejected.fSDEInitPhase = "boot-translation";
	smmuRejected.fSDEInitResult = MUNCH_SDE_FAULT;
	smmuRejected.fSDEError = kIOReturnNotReady;
	smmuRejected.runtime.snapshotValid = smmuRejected.runtime.domainSnapshotValid = true;
	smmuRejected.runtime.baseline = layoutSnapshot;
	smmuRejected.runtime.baseline.autorefresh = 0x80000001;
	smmuRejected.runtime.observedDomain.fsr = 0x602;
	smmuRejected.runtime.observedDomain.ttbr = 0x123400009dd00000ULL;
	smmuRejected.runtime.observedDomain.mair = 0xff44;
	smmuRejected.runtime.contextPhysical = 0x15081000;
	smmuRejected.runtime.faultAddress = 0x1dead0000ULL;
	smmuRejected.runtime.fsynr0 = 0x10;
	smmuRejected.runtime.fsynr1 = 0x820;
	smmuRejected.runtime.bootWalk.check = "fsr-fault";
	smmuRejected.runtime.bootWalk.level = 4;
	smmuRejected.runtime.bootWalk.iova = 0x9c000000;
	diagnosticLog.clear();
	smmuRejected.reportSDEStatus(0);
	assert(diagnosticLog.find("init=hardware-fault phase=boot-translation") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SMMU FSR: raw=00000602 faults=00000002 format=3") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SMMU TABLE: ttbr=123400009dd00000") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SMMU FAULT: far=00000001dead0000 syn0=00000010 syn1=00000820") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SMMU WALK: check=fsr-fault iova=9c000000 level=4 pages=0") != std::string::npos);
	assert(diagnosticLog.find("read=0 access=not-read") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE CTL") == std::string::npos);
	for (unsigned format = 0; format < 4; format++) {
		smmuRejected.runtime.observedDomain.fsr = format << 9;
		diagnosticLog.clear();
		smmuRejected.reportSDEStatus((format + 1) * 5000000);
		assert(diagnosticLog.find("faults=00000000 format=" + std::to_string(format)) != std::string::npos);
	}
	diagnosticLog.clear();
	successful.reportSDEStatus(0);
	legacy.reportSDEStatus(0);
	assert(diagnosticLog.empty());
	IOMobileFramebuffer untouched;
	untouched.reportSDEStatus(0);
	assert(diagnosticLog.find("SwapEnd=0 accepted=0") != std::string::npos);
	assert(diagnosticLog.find("MUNCH SDE SAVED:") == std::string::npos);
	diagnosticLog.clear();
	failed.reportSDEStatus(0);
	assert(diagnosticLog.find("error=0xe00002d6") != std::string::npos);
	puts("Actual XNU swap queue/wait/result tests passed");
	puts("Boot console survives rejection/timeout; takeover follows completion; SDE blocks test blits");
	puts("SDE status preserves raw errors, counts malformed/unaligned swaps, and rate-limits idle diagnostics");
	puts("Route diagnostics identify the exact gate and repeat immutable CTL/LM/SSPP state");
	testPanelPowerAndCopies();
}
