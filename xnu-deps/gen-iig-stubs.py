#!/usr/bin/env python3
"""Generate the ARM64 DriverKit compatibility implementation for Linux XNU builds.

Include both kernel references and exported generated methods. Kernel wrappers
with an available _Impl use that implementation; unsupported RPC entry points
return kIOReturnUnsupported. This does not implement user-space DriverKit RPC.

usage: gen-iig-stubs.py undefined.txt outdir [kernel-symbols] --exports-dir XNU/config
"""
import argparse
import re
from pathlib import Path


META = {
    'IODispatchSource': ('OSObject', True),
    'IOEventLink': ('OSObject', False),
    'IOWorkGroup': ('OSObject', False),
    'IODispatchQueue': ('OSObject', False),
    'OSAction': ('OSObject', False),
    'IODataQueueDispatchSource': ('IODispatchSource', False),
    'IOInterruptDispatchSource': ('IODispatchSource', False),
    'IOServiceNotificationDispatchSource': ('IODispatchSource', False),
    'IOServiceStateNotificationDispatchSource': ('IODispatchSource', False),
    'OSAction_IOUserClient_KernelCompletion': ('OSAction', False),
}
RPC_CLASSES = set(META) | {
    'OSObject', 'IOService', 'IOMemoryDescriptor', 'IOBufferMemoryDescriptor',
    'IOMemoryMap', 'IODMACommand', 'IOUserClient', 'IOUserServer',
}
SUPER = 'PFiP15OSMetaClassBase5IORPCE'
INCLUDES = (
    'IOKit/IOLib.h', 'IOKit/IOReturn.h', 'IOKit/IOService.h',
    'IOKit/IOUserClient.h', 'IOKit/IOUserServer.h',
    'DriverKit/IODispatchSource.h', 'DriverKit/IODispatchQueue.h',
    'DriverKit/IOEventLink.h', 'DriverKit/IOWorkGroup.h', 'DriverKit/OSAction.h',
    'DriverKit/IODataQueueDispatchSource.h', 'DriverKit/IOInterruptDispatchSource.h',
    'DriverKit/IOServiceNotificationDispatchSource.h', 'DriverKit/IOServiceStateNotificationDispatchSource.h',
)


def member_name(symbol):
    match = re.match(r'__ZN(\d+)', symbol)
    if not match:
        return None
    position = match.end() + int(match[1])
    owner = symbol[match.end():position]
    method_length = re.match(r'(\d+)', symbol[position:])
    if not method_length:
        return None
    start = position + method_length.end()
    end = start + int(method_length[1])
    return owner, symbol[start:end], symbol[end:], position


def impl_name(symbol):
    member = member_name(symbol)
    if not member or not symbol.endswith(SUPER):
        return None
    _, method, rest, position = member
    if not rest.startswith('E'):
        return None
    parameters = rest[1:-len(SUPER)] or 'v'
    return f'{symbol[:position]}{len(method) + 5}{method}_ImplE{parameters}'


def exported_iig_symbols(directory):
    methods, classes = set(), set()
    for component in ('IOKit', 'Libkern', 'Private'):
        for suffix in ('.exports', '.arm64.exports'):
            for line in (directory / (component + suffix)).read_text().splitlines():
                entry = line.partition('#')[0].strip()
                if not entry:
                    continue
                symbol = entry.split(':')[-1].split()[0]
                member = member_name(symbol)
                if not member:
                    continue
                owner, method, _, _ = member
                if owner in META and method in ('metaClass', 'gMetaClass', 'superClass'):
                    classes.add(owner)
                if owner in RPC_CLASSES and (method == '_Dispatch' or method.endswith('_Invoke') or
                                             symbol.endswith(SUPER)):
                    methods.add(symbol)
    return methods, classes


def read_kernel_symbols(path):
    defined = set()
    if path:
        for line in path.read_text().splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in ('t', 'T', 's', 'S'):
                defined.add(parts[2])
    return defined


def generate(undefined, defined, exports_dir=None):
    classes, methods = set(), set()
    for symbol in undefined:
        member = member_name(symbol)
        if not member:
            continue
        owner, method, rest, _ = member
        if owner not in RPC_CLASSES:
            raise ValueError(f'Not a supported DriverKit interface: {symbol}')
        if method == 'metaClass' and rest == 'E':
            if owner not in META:
                raise ValueError(f'No metaclass definition for {owner}')
            classes.add(owner)
        else:
            methods.add(symbol)
    if exports_dir:
        exported_methods, exported_classes = exported_iig_symbols(exports_dir)
        methods |= exported_methods
        classes |= exported_classes
    if any(symbol.startswith('__ZTV') for symbol in undefined):
        classes.add('IODispatchSource')
    for owner in tuple(classes):
        parent = META[owner][0]
        if parent in META:
            classes.add(parent)
    for owner in classes:
        prefix = f'__ZN{len(owner)}{owner}'
        methods.add(prefix + '8DispatchE5IORPC')
        methods.add(prefix + '9MetaClass8DispatchE5IORPC')
    direct, unsupported = [], []
    for symbol in sorted(methods):
        implementation = impl_name(symbol)
        if implementation and implementation in defined:
            direct.append((symbol, implementation))
        else:
            unsupported.append(symbol)
    output = ['/* Generated by gen-iig-stubs.py: stand-in for iig output. Do not edit. */']
    output += [f'#include <{header}>' for header in INCLUDES]
    output.append('')
    for owner, (parent, abstract) in META.items():
        if owner in classes:
            macro = 'OSDefineMetaClassAndAbstractStructors' if abstract else 'OSDefineMetaClassAndStructors'
            output.append(f'{macro}({owner}, {parent});')
    output.append('')
    for symbol, implementation in direct:
        output.append('__asm__(".text\\n.globl %s\\n.p2align 2\\n%s:\\n\\tb %s\\n");' %
                      (symbol, symbol, implementation))
    output += ['', 'extern "C" {']
    for index, symbol in enumerate(unsupported):
        output.append(f'kern_return_t __iig_stub_{index}(void) __asm__("{symbol}");')
        output.append(f'kern_return_t __iig_stub_{index}(void) {{ return kIOReturnUnsupported; }}')
    output.append('}')
    return '\n'.join(output) + '\n', (len(classes), len(direct), len(unsupported))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('undefined', type=Path)
    parser.add_argument('outdir', type=Path)
    parser.add_argument('kernel_symbols', type=Path, nargs='?')
    parser.add_argument('--exports-dir', type=Path)
    args = parser.parse_args()
    if args.exports_dir and not args.kernel_symbols:
        parser.error('--exports-dir requires the kernel-symbols input for _Impl forwarding')
    undefined = [line.strip() for line in args.undefined.read_text().splitlines() if line.strip()]
    output, counts = generate(undefined, read_kernel_symbols(args.kernel_symbols), args.exports_dir)
    args.outdir.mkdir(parents=True, exist_ok=True)
    (args.outdir / 'OSObject.iig.cpp').write_text(output)
    print('metaclasses: %d, forwarded to _Impl: %d, unsupported RPC entries: %d' % counts)


if __name__ == '__main__':
    main()
