#!/usr/bin/env python3
"""Build the minimal Apple Device Tree used by the XNU bring-up.

The default profile is the inherited SWEET donor profile. ``--munch`` emits
the POCO F4 SoC/display identity and omits UFS unless ``--ufs`` is also set;
its RAM window must be supplied explicitly. On the direct fastboot path, the
shim validates that window against the runtime Android-ABL FDT before loading
XNU.
Usage: mkadt.py OUT.adt [--dump] [--qemu] [--highmem] [--munch] [--ufs]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: above every Qualcomm XPU carve-out (see NOTES.md)
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
# --highmem (phone only): the whole upper DDR bank. The live FDT memory node is
# <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
# carve-out is in the low bank and the dynamic pools are limited below 4G.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0x9c000000, 0x1700000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic
MUNCH_ECID = 0x4d554e4348000001  # 'MUNCH' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2400, 1080 * 4
MUNCH_FB, MUNCH_FB_SIZE = 0x9c000000, 0x2300000
# Internal UFS host (ufshc@1d84000 "ufs_mem"); QEMU virt has none
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
MUNCH_VTIMER_INTID = 27
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False, munch=False, dram_base=None, dram_size=None,
          ram_only=False, ufs_enabled=False):
    """Build a SWEET, QEMU, or explicit-memory Munch profile.

    Munch memory is intentionally not guessed. Callers must pass an explicit
    ``dram_base`` and ``dram_size`` candidate; on the direct ABL path the shim
    checks it against the FDT actually supplied at boot. The profile is
    always RAM-only.
    """
    if munch and qemu:
        raise ValueError('--munch and --qemu are separate hardware profiles')
    if ufs_enabled and not munch:
        raise ValueError('--ufs requires the --munch profile')
    if munch and (dram_base is None or dram_size is None):
        raise ValueError('--munch requires an explicit --dram-base and --dram-size candidate; the shim validates it against the runtime ABL FDT')
    if dram_base is not None or dram_size is not None:
        if dram_base is None or dram_size is None:
            raise ValueError('--dram-base and --dram-size must be supplied together')
        if dram_base < 0 or dram_size <= 0:
            raise ValueError('dram-base must be non-negative and dram-size must be positive')

    if dram_base is not None:
        phys_base, mem_size = dram_base, dram_size
    elif highmem:
        phys_base, mem_size = HIGH_PHYS_BASE, HIGH_MEM_SIZE
    else:
        phys_base, mem_size = PHYS_BASE, MEM_SIZE
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    platform = 'munch' if munch else 'sweet'
    ecid = MUNCH_ECID if munch else SWEET_ECID
    fb, fb_size = (MUNCH_FB, MUNCH_FB_SIZE) if munch else (FB, FB_SIZE)
    display_compatible = 'munch,display' if munch else 'sweet,display'
    display_w, display_h = (FB_W, FB_H) if munch else (1125, 2436)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr(f'{platform}-shim')),
        ('system-firmware-version', cstr(f'{platform}-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', ecid)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # The donor SWEET profile and the Munch profile both expose Aff1:Aff0
    # MPIDRs. Munch's actual Kona topology is 4 little + 3 big + 1 prime;
    # the current XNU patches still treat the CPUs as one generic cluster.
    # QEMU retains its original single-CPU tree: its MPIDR differs from phone.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        if munch:
            cpu_compatible = ('qcom,kryo', 'ARM,v8')
            freq_domain = 0 if core < 4 else 1 if core < 7 else 2
            capacity = 448 if core < 4 else 1023 if core < 7 else 1024
            dynamic_power = 89 if core < 4 else 304 if core < 7 else 386
        else:
            cpu_compatible = ('qcom,kryo470', 'ARM,v8')
            freq_domain = None
            capacity = None
            dynamic_power = None
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', b''.join(cstr(value) for value in cpu_compatible)),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2304000000 if core >= 6 else 1804800000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ] + ([('qcom,freq-domain', u32(freq_domain)),
              ('capacity-dmips-mhz', u32(capacity)),
              ('dynamic-power-coefficient', u32(dynamic_power))] if munch else [])))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    timer_intid = QEMU_VTIMER_INTID if qemu else MUNCH_VTIMER_INTID if munch else VTIMER_INTID
    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(timer_intid)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr(f'{platform}-io')),
        ('compatible', cstr(f'{platform}-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display') + cstr(display_compatible) if munch else cstr(display_compatible)),
            ('canvas-width', u32(display_w)),
            ('canvas-height', u32(display_h)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(fb, fb_size)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    ufs = node('munch-ufs' if munch else 'sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    children = [chosen, defaults, cpus, arm_io, fbcon, buttons] + product
    if not qemu and not ram_only and (not munch or ufs_enabled):
        children.append(ufs)

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') +
         (cstr('xiaomi,sweet') + cstr('xiaomi,munch') if munch else cstr('xiaomi,sweet')) +
         cstr('qcom,sm8250' if munch else 'qcom,sm7150') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], children)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit('usage: mkadt.py OUT.adt [--dump] [--qemu] [--highmem] [--ram-only] [--munch [--ufs] --dram-base N --dram-size N]')
    args = set(sys.argv[2:])

    def option_value(name):
        for i, arg in enumerate(sys.argv[2:], 2):
            if arg == name:
                if i + 1 >= len(sys.argv):
                    raise SystemExit(f'{name} requires a value')
                try:
                    return int(sys.argv[i + 1], 0)
                except ValueError as exc:
                    raise SystemExit(f'{name} must be an integer literal') from exc
        return None

    root = build(qemu='--qemu' in args, highmem='--highmem' in args,
                 munch='--munch' in args, ram_only='--ram-only' in args,
                 dram_base=option_value('--dram-base'), dram_size=option_value('--dram-size'),
                 ufs_enabled='--ufs' in args)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
