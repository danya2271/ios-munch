#!/usr/bin/env python3
"""Inspect captured munch DT properties against UMI A650 port prerequisites."""
import argparse
import json
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('dtb')
a = p.parse_args()
def prop(node, name, kind='x'):
    text = subprocess.check_output(['fdtget', '-t', kind, a.dtb, node, name], text=True).strip()
    return text if kind == 's' else [int(x, 16) for x in text.split()]
checks = {
    'chip_id': ('/soc/qcom,kgsl-3d0@3d00000', 'qcom,chipid', [0x06050002]),
    'gpu_registers': ('/soc/qcom,kgsl-3d0@3d00000', 'reg', None),
    'gpucc_registers': ('/soc/qcom,gpucc@3d90000', 'reg', None),
    'gmu_registers': ('/soc/qcom,gmu@3d6a000', 'reg', [0x3d6a000,0x30000,0xb290000,0x10000,0xb490000,0x10000]),
    'zap_reserved_memory': ('/reserved-memory/pil_gpu_region@8681a000', 'reg', [0,0x8681a000,0,0x2000]),
}
result = {'device': 'munch', 'hardware_tested': False, 'properties': {}}
for label, (node, name, expected) in checks.items():
    value = prop(node, name)
    if expected is not None and value != expected:
        raise ValueError('UMI prerequisite mismatch: ' + label)
    result['properties'][label] = [hex(x) for x in value]
for label, node, expected in (
    ('gmu_kernel', '/soc/qcom,gmu@3d6a000/gmu_kernel', [5,0x400]),
    ('gfx3d_user', '/soc/qcom,kgsl-iommu@3da0000/gfx3d_user', [0,0x401]),
):
    value = prop(node, 'iommus')
    if value[1:] != expected: raise ValueError('UMI stream mismatch: ' + label)
    result['properties'][label + '_stream_and_mask'] = [hex(x) for x in value[1:]]
result['gpucc_compatible'] = prop('/soc/qcom,gpucc@3d90000','compatible','s')
if 'qcom,gpucc-kona' not in result['gpucc_compatible'].split():
    raise ValueError('Expected Kona GPUCC')
result['remaining'] = ['device-local A650 firmware', 'munch clock/power-level validation',
                       'kernel A650 driver and matching userland ABI', 'Mesa-enabled helper image',
                       'device startup/draw/shutdown tests']
print(json.dumps(result, indent=2))
