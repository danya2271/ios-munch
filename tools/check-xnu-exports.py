#!/usr/bin/env python3
"""Check DriverKit generation, generic ARM64 exports and the real symbol-set validator."""
import argparse
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('iig', ROOT / 'xnu-deps/gen-iig-stubs.py')
iig = importlib.util.module_from_spec(spec)
spec.loader.exec_module(iig)
START = '__ZN9IOService5StartEPS_PFiP15OSMetaClassBase5IORPCE'
START_IMPL = '__ZN9IOService10Start_ImplEPS_'
DISPATCH = '__ZN9IOService9_DispatchEPS_5IORPC'
ACTION = 'OSAction_IOUserClient_KernelCompletion'


class GeneratorTests(unittest.TestCase):
    def test_impl_names(self):
        self.assertEqual(iig.impl_name(START), START_IMPL)
        self.assertEqual(iig.impl_name('__ZN8OSAction7AbortedEPFiP15OSMetaClassBase5IORPCE'),
                         '__ZN8OSAction12Aborted_ImplEv')
        self.assertIsNone(iig.impl_name(DISPATCH))

    def test_forwarding_and_failure(self):
        source, counts = iig.generate([START, DISPATCH, START], {START_IMPL})
        self.assertEqual(counts, (0, 1, 1))
        self.assertIn('\\tb ' + START_IMPL, source)
        self.assertIn('return kIOReturnUnsupported;', source)
        self.assertEqual(source.count(f'__asm__("{DISPATCH}")'), 1)

    def test_unknown_cpp_is_not_stubbed(self):
        with self.assertRaisesRegex(ValueError, 'Not a supported DriverKit'):
            iig.generate(['__ZN26IOUnifiedAddressTranslator3mapEP11IOMemoryMapj'], set())
        source, counts = iig.generate(['_pmap_iommu_init'], set())
        self.assertEqual(counts, (0, 0, 0))
        self.assertNotIn('_pmap_iommu_init', source)

    def test_exports_without_kernel_references(self):
        with tempfile.TemporaryDirectory() as directory:
            exports = Path(directory)
            for component in ('IOKit', 'Libkern', 'Private'):
                for suffix in ('.exports', '.arm64.exports'):
                    (exports / (component + suffix)).write_text('')
            (exports / 'IOKit.exports').write_text(START + '\n' + DISPATCH + '\n')
            (exports / 'Private.exports').write_text(
                f'__ZN{len(ACTION)}{ACTION}10gMetaClassE\n_pmap_iommu_init\n')
            source, counts = iig.generate([], {START_IMPL}, exports)
            self.assertEqual(counts[0], 2)
            self.assertIn('\\tb ' + START_IMPL, source)
            self.assertIn(DISPATCH, source)
            self.assertIn(f'OSDefineMetaClassAndStructors({ACTION}, OSAction);', source)
            self.assertIn(f'__ZN{len(ACTION)}{ACTION}9MetaClass8DispatchE5IORPC', source)
            self.assertNotIn('_pmap_iommu_init', source)

    def test_nm_types(self):
        with tempfile.TemporaryDirectory() as directory:
            symbols = Path(directory) / 'nm.txt'
            symbols.write_text('0001 S _text_exec\n0002 T _text\n0003 D _data\n U _undefined\n')
            self.assertEqual(iig.read_kernel_symbols(symbols), {'_text_exec', '_text'})


class BuildTests(unittest.TestCase):
    tree = ROOT / 'xnu-src/xnu-8796.121.2'

    @property
    def build(self):
        return self.tree / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837'

    def test_generic_exports_scope(self):
        original = set((self.tree / 'config/Private.arm64.exports').read_text().splitlines())
        selected = (ROOT / 'xnu/files/config/Private.arm64.bcm2837.exports').read_text()
        self.assertEqual(selected, (self.tree / 'config/Private.arm64.bcm2837.exports').read_text())
        selected = set(selected.splitlines())
        self.assertFalse(selected - original)
        removed = original - selected
        prefixes = ('_pmap_iommu_', '_nvme_ppl_', '_sart_get_desc', '_t8020dart_', '_t6000dart_',
                    '_t8110dart_', '_uat_get_desc', '__ZN26IOUnifiedAddressTranslator',
                    '__ZTV26IOUnifiedAddressTranslator')
        self.assertEqual(len(removed), 35)
        self.assertTrue(all(symbol.startswith(prefixes) for symbol in removed))
        self.assertIn('_ml_get_topology_info', selected)
        self.assertIn('_cpu_xcall', selected)

    def test_machine_specific_selection(self):
        makefile = (self.tree / 'config/Makefile').read_text()
        selection = makefile[makefile.index('EXPORTS_FILES :='):makefile.index('Kasan_EXPORTS :=')]
        text = selection + '\nprint:\n\t@printf "%s\\n" $(EXPORTS_FILES)\n'
        for machine, expected in (('BCM2837', 'Private.arm64.bcm2837.exports'),
                                  ('T8101', 'Private.arm64.exports')):
            result = subprocess.check_output(['make', '--no-print-directory', '-f', '-', 'print',
                                              'CURRENT_ARCH_CONFIG=ARM64', f'CURRENT_MACHINE_CONFIG={machine}',
                                              'EXPORT_SOURCE_ARCH_CONFIG_LC=arm64',
                                              'SYMBOL_COMPONENT_LIST=IOKit Private',
                                              f'SOURCE={self.tree / "config"}'], input=text, text=True)
            private = [name for name in result.splitlines() if name.startswith('Private.arm64')]
            self.assertEqual(private, [expected])
            self.assertIn('IOKit.arm64.exports', result.splitlines())

    def test_generator_reproducible(self):
        symbols = subprocess.check_output(['llvm-nm', '-n', str(self.build / 'mach.development.bcm2837.unstripped')],
                                          text=True)
        defined = {parts[2] for line in symbols.splitlines() if len(parts := line.split()) == 3 and
                   parts[1] in ('t', 'T', 's', 'S')}
        undefined = (ROOT / 'xnu-deps/iig-impl/undefined-all.txt').read_text().splitlines()
        generated, counts = iig.generate(undefined, defined, self.tree / 'config')
        self.assertEqual(generated, (ROOT / 'xnu-deps/iig-impl/OSObject.iig.cpp').read_text())
        self.assertEqual(counts, (10, 55, 103))
        self.assertEqual(generated, (self.build / 'iokit/DriverKit/OSObject.iig.cpp').read_text())

    def validate_symbols(self, symbols):
        tool = self.tree / 'BUILD/obj/SETUP/kextsymboltool/kextsymboltool'
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            exports = work / 'test.exports'
            exports.write_text(symbols)
            return subprocess.run([str(tool), '-arch', 'arm64', '-import', str(self.build / 'config/allsymbols'),
                                   '-export', str(exports), '-output', str(work / 'output.symbolset')],
                                  capture_output=True, text=True)

    def test_validator_accepts_real_definitions(self):
        result = self.validate_symbols('_pmap_cs_configuration\n_pmap_has_iofilter_protected_write\n' +
                                       START + '\n' + DISPATCH + '\n')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_validator_rejects_missing(self):
        result = self.validate_symbols('_munch_test_missing_export\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('exported name not in import list', result.stderr)

    def test_validator_rejects_duplicates(self):
        result = self.validate_symbols('_pmap_cs_configuration\n' * 2)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('duplicate export', result.stderr)

    def test_complete_export_checks_exist(self):
        for name in ('checked-public.symbolset', 'checked-private.symbolset', 'IOKit.symbolset',
                     'Libkern.symbolset', 'Private.symbolset'):
            self.assertGreater((self.build / 'config' / name).stat().st_size, 4096)
        config = (self.tree / 'config/Makefile').read_text()
        checks = config[config.index('check_all_exports:'):config.index('$(OBJPATH)/$(MD_SUPPORTED_KPI_FILENAME):')]
        self.assertEqual(checks.count('$(KEXT_CREATE_SYMBOL_SET)'), 3)
        self.assertNotIn('-sect', checks)
        self.assertNotIn('-diff', checks)

    def test_build_patches_apply_to_clean_sources(self):
        patch = (ROOT / 'xnu/hakios.patch').read_text()
        paths = ('config/Makefile', 'iokit/DriverKit/Makefile')
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            for name in paths:
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(subprocess.check_output(['git', '-C', str(self.tree), 'show', f'HEAD:{name}']))
            subprocess.run(['git', 'apply', *('--include=' + name for name in paths), '-'],
                           cwd=work, input=patch, text=True, check=True)
            for name in paths:
                self.assertEqual((work / name).read_bytes(), (self.tree / name).read_bytes())

    def test_complete_patch_applies(self):
        patch = (ROOT / 'xnu/hakios.patch').read_text()
        stats = subprocess.check_output(['git', 'apply', '--numstat', '-'], input=patch, text=True)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            for entry in stats.splitlines():
                name = entry.split('\t', 2)[2]
                original = subprocess.run(['git', '-C', str(self.tree), 'show', f'HEAD:{name}'],
                                          capture_output=True)
                if original.returncode == 0:
                    path = work / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(original.stdout)
                    metadata = subprocess.check_output(
                        ['git', '-C', str(self.tree), 'ls-tree', 'HEAD', '--', name], text=True)
                    path.chmod(int(metadata.split()[0], 8) & 0o777)
            subprocess.run(['git', 'apply', '--check', '-'], cwd=work, input=patch, text=True, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=BuildTests.tree)
    args = parser.parse_args()
    BuildTests.tree = args.tree.resolve()
    unittest.main(argv=[__file__])
