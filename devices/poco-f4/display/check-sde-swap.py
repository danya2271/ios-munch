#!/usr/bin/env python3
"""Compile actual XNU swap methods against host locks; check compositor/notification wiring."""
import os
import subprocess
import tempfile
from pathlib import Path


here = Path(__file__).resolve().parent
source = (here.parents[2] / 'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
frame_helpers = source[source.index('static bool\nmunchSdeComposeFullFrame('):
                       source.index('/* Keep a small copy of the frame')]
methods = source[source.index('IOReturn\nIOMobileFramebuffer::present('):
                 source.index('void\nIOMobileFramebuffer::presenterMain(')]
show = source[source.index('void\nIOMobileFramebuffer::show('):source.index('class IOMobileFramebufferUserClient :')]
sequence = ('munchSdeBeginFrame(', 'memcpy(back,', 'fBase = back;', 'drawLayer(s,',
            'munchSdeSubmitFrame(fSDE)', 'retireNativeSwap(fSDEInFlightID, surfaces)')
positions = [show.index(fragment) for fragment in sequence]
assert positions == sorted(positions)
assert 'munchSdeEndFrame(fSDE)' in show
assert show.index('munchSdeSubmitFrame(fSDE)') < show.index('retireNativeSwap(fSDEInFlightID, surfaces)')
native_publish = show.index('retireNativeSwap(fSDEInFlightID, surfaces)')
assert native_publish < show.index('IOLockUnlock(fLock)', native_publish)
assert 'gOnTheBeat && !fSDERequested' in show
assert 'fDisplayBlanked || fSDEError != kIOReturnSuccess' in show
assert 'finishNativeSwapInOrder(swapWord(swap, SWAP_ID), kIOReturnAborted)' in show
assert 'retireNativeSwap(fSDEInFlightID, surfaces)' in show
assert show.index('fDisplayBlanked') < show.index('MiniIOSurfaceDrawn(')
assert source.count('PE_initialize_console(NULL, kPEGraphicsMode)') == 4
assert 'if (!fGraphics && !fSDERequested)' in show
assert 'PE_initialize_console' not in show[show.index('munchSdeBeginFrame('):show.index('memcpy(back,')]
start = source[source.index('bool\nIOMobileFramebuffer::start('):source.index('IOReturn\nIOMobileFramebuffer::sendDcsCommand(')]
assert start.index('munchSdeInitialize(') < start.index('kernel_thread_start(&IOMobileFramebuffer::presenterMain')
presenter = source[source.index('void\nIOMobileFramebuffer::presenter(void)'):source.index('void\nIOMobileFramebuffer::show(')]
assert 'clock_interval_to_deadline(5000, kMillisecondScale, &deadline)' in presenter
assert presenter.count('reportSDEStatus(') == 2
assert 'bool queued = !fSDE->nativeTakeover && fSwapsWaiting != 0' in presenter
assert 'unsigned slot = fSwapFirst;' in presenter
assert 'MiniIOSurfaceDrawn(surfaces[layer], 0)' in presenter
assert 'if (!gpuReady) waitNativeFrame();' in presenter
assert 'waitNativeFrame(true);' in presenter
assert 'munchSdeCompleteFrame(fSDE)' in source
waiter = source[source.index('enum munch_sde_result\nIOMobileFramebuffer::waitNativeFrame(bool allowQueued)'):
                source.index('void\nIOMobileFramebuffer::finishNativeSwapInOrder(')]
assert 'clock_interval_to_deadline(irqActive ? 1 : 250' in waiter
assert 'kMicrosecondScale' in waiter
assert 'thread_block(THREAD_CONTINUE_NULL);' in waiter
assert 'completeNativeSwap(result)' in waiter
assert waiter.index('uint32_t sequence =') < waiter.index('munchSdeCompleteFrame(fSDE)')
assert 'if (queued) return MUNCH_SDE_BUSY;' in waiter
assert show.index('drawLayer(s,') < show.index('waitNativeFrame();') < show.index('munchSdeSubmitFrame(fSDE)')
native_complete = source[source.index('void\nIOMobileFramebuffer::completeNativeSwap('):
                         source.index('class IOMobileFramebufferUserClient :')]
assert 'finishSwap(' not in native_complete
assert 'messageClients(kIOMessageServicePropertyChange)' in native_complete
performance = source[source.index('void\nIOMobileFramebuffer::reportSDEPerformance('):
                      source.index('/* SwapEnd:')]
assert 'completion=%llu' in performance
assert 'reportFrameTimes();' in performance
assert 'fStatSDECompletion += drawn - submitted' in source
finish = source[source.index('void\nIOMobileFramebuffer::finishSwap('):
                source.index('bool\nIOMobileFramebuffer::presentationTime(')]
assert 'fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE' in finish
assert 'fSwapsWaiting--' in finish
assert finish.index('reportNativeFailure(') < finish.index('fSDEError = result')
assert 'MUNCH SDE NATIVE FAILURE:' in source
assert 'loader_log_line(liveLine)' in source
assert 'MUNCH SDE START:' in source
assert 'fOwner->submitSwap(args->structureInput, args->structureInputSize)' in source
assert 'const uint32_t rate = refreshHz() << 16' in source
assert '#define kFBNativeRefreshHz      60' in source
blit = source[source.index('extern "C" int\nMiniIOMobileFramebufferShow('):
              source.index('void\nMiniIOMobileFramebufferStart(')]
power = source[source.index('IOReturn\nIOMobileFramebuffer::setDisplayPower('):
               source.index('static bool\nmunchSdeComposeFullFrame(')]
arm = source[source.index('IOMobileFramebufferUserClient::armVSync(void)'):source.index('IOMobileFramebufferUserClient::vsync(void)')]
assert arm.index('if (fOwner->fSDERequested && !fOwner->fSDENativeRequested) return;') < arm.index('thread_call_enter_delayed')
assert 'nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period)' in arm
vsync = source[source.index('IOMobileFramebufferUserClient::vsync(void)'):source.index('IOMobileFramebufferUserClient::message(')]
assert vsync.index('presentationTime(') < vsync.index('sendAsyncResult64WithOptions')
assert vsync.index('armVSync();') < vsync.index('IOLockUnlock(fLock);')
with tempfile.TemporaryDirectory(prefix='munch-sde-swap-') as directory:
    work = Path(directory)
    (work / 'sde-frame.inc').write_text(frame_helpers)
    (work / 'sde-swap.inc').write_text(methods)
    retire = source[source.index('void\nIOMobileFramebuffer::retireNativeSwap('):
                    source.index('enum munch_sde_result\nIOMobileFramebuffer::waitNativeFrame(')]
    (work / 'sde-retire.inc').write_text(retire)
    (work / 'sde-wait.inc').write_text(waiter)
    (work / 'sde-blit.inc').write_text(blit)
    (work / 'sde-power.inc').write_text(power)
    binary = work / 'check-sde-swap'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-pthread', '-g', '-I' + str(work),
                    str(here / 'check-sde-swap.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
