#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/hakios.dma.pagetable.h>

#define TABLES 64u
#define WORDS 512u
#define PAGE_SIZE 4096u
#define ADDRESS_MASK 0x0000fffffffff000ull
#define DATA_BASE 0xa1000000ull
#define RW (HAKIOS_DMA_PT_READ | HAKIOS_DMA_PT_WRITE)

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;
static unsigned failure_cases;

struct fixture {
	struct hakios_dma_pagetable *table;
	struct hakios_dma_pt_page pages[TABLES];
	uint64_t *arena;
	uint64_t *visible;
	uint32_t count;
	uint64_t root;
	unsigned calls;
	unsigned fail_at;
	unsigned syncs;
	unsigned invalidations;
	unsigned quiesces;
	unsigned trace[8192];
	unsigned trace_count;
	int callback_error;
	bool apply_before_error;
	bool quiesce_failure;
	bool attached;
	bool cached;
	uint64_t cached_iova;
	uint64_t cached_entry;
};

static uint32_t
page_index(struct fixture *fixture, uint64_t physical)
{
	for (uint32_t index = 0; index < fixture->count; index++) {
		if (fixture->pages[index].physical == physical) {
			return index;
		}
	}
	assert(false);
	return 0;
}

static uint64_t
walk(struct fixture *fixture, uint64_t iova, bool visible)
{
	uint64_t *memory = visible ? fixture->visible : fixture->arena;
	uint64_t *entries = &memory[page_index(fixture, fixture->pages[0].physical) * WORDS];
	const unsigned shifts[] = {30, 21, 12};
	for (unsigned level = 0; level < 3; level++) {
		uint64_t entry = entries[(iova >> shifts[level]) & 511];
		if (entry == 0 || level == 2) {
			return entry;
		}
		assert((entry & 3) == 3);
		entries = &memory[page_index(fixture, entry & ADDRESS_MASK) * WORDS];
	}
	assert(false);
	return 0;
}

static uint64_t
device_lookup(struct fixture *fixture, uint64_t iova)
{
	assert(fixture->attached);
	if (!fixture->cached || fixture->cached_iova != iova) {
		fixture->cached_iova = iova;
		fixture->cached_entry = walk(fixture, iova, true);
		fixture->cached = true;
	}
	return fixture->cached_entry;
}

static int
callback(struct fixture *fixture)
{
	fixture->calls++;
	sched_yield();
	return fixture->calls == fixture->fail_at ? fixture->callback_error : 0;
}

static int
sync_page(void *owner, const struct hakios_dma_pt_page *page)
{
	struct fixture *fixture = owner;
	unsigned index = page_index(fixture, page->physical);
	assert(page->entries == fixture->pages[index].entries);
	fixture->syncs++;
	assert(fixture->trace_count < sizeof(fixture->trace) / sizeof(fixture->trace[0]));
	fixture->trace[fixture->trace_count++] = index;
	int error = callback(fixture);
	if (error == 0 || fixture->apply_before_error) {
		memcpy(&fixture->visible[index * WORDS], page->entries, PAGE_SIZE);
	}
	return error;
}

static int
invalidate(void *owner, uint64_t iova, uint64_t size)
{
	struct fixture *fixture = owner;
	assert(fixture->attached && size != 0 && size % PAGE_SIZE == 0);
	fixture->invalidations++;
	for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
		assert(walk(fixture, iova + offset, true) == walk(fixture, iova + offset, false));
	}
	int error = callback(fixture);
	if (error == 0 || fixture->apply_before_error) {
		fixture->cached = false;
	}
	return error;
}

static int
quiesce(void *owner)
{
	struct fixture *fixture = owner;
	assert(fixture->attached);
	fixture->quiesces++;
	int error = callback(fixture);
	if (error != 0 || fixture->quiesce_failure) {
		return error != 0 ? error : ETIMEDOUT;
	}
	fixture->attached = false;
	fixture->cached = false;
	return 0;
}

static const struct hakios_dma_pt_operations operations = {
	.version = HAKIOS_DMA_PT_VERSION,
	.sync = sync_page, .invalidate = invalidate, .quiesce = quiesce,
};

static void
fixture_allocate(struct fixture *fixture, uint32_t count)
{
	assert(count != 0 && count <= TABLES);
	memset(fixture, 0, sizeof(*fixture));
	fixture->count = count;
	fixture->callback_error = EIO;
	fixture->arena = aligned_alloc(PAGE_SIZE, count * PAGE_SIZE);
	fixture->visible = malloc(count * PAGE_SIZE);
	assert(fixture->arena != NULL && fixture->visible != NULL);
	memset(fixture->arena, 0xa5, count * PAGE_SIZE);
	memset(fixture->visible, 0xa5, count * PAGE_SIZE);
	for (uint32_t index = 0; index < count; index++) {
		fixture->pages[index].entries = &fixture->arena[index * WORDS];
		fixture->pages[index].physical = 0x90000000ull + index * 0x3000;
	}
}

static struct hakios_dma_pt_config
config_for(struct fixture *fixture, bool nonsecure)
{
	return (struct hakios_dma_pt_config) {
		.pages = fixture->pages, .table_count = fixture->count, .output_bits = 40, .nonsecure = nonsecure,
	};
}

static void
fixture_init(struct fixture *fixture, uint32_t count, bool nonsecure)
{
	fixture_allocate(fixture, count);
	struct hakios_dma_pt_config config = config_for(fixture, nonsecure);
	assert(hakios_dma_pt_create(&config, &operations, fixture, &fixture->table) == 0);
	assert(fixture->calls == 0);
	for (uint32_t word = 0; word < count * WORDS; word++) {
		assert(fixture->arena[word] == 0);
	}
}

static void
publish(struct fixture *fixture)
{
	struct hakios_dma_pt_info info;
	assert(hakios_dma_pt_get_info(fixture->table, &info) == 0);
	assert(info.root == fixture->pages[0].physical && info.granule == PAGE_SIZE);
	assert(info.input_bits == 39 && info.output_bits == 40 && info.start_level == 1);
	assert(info.mair0 == 0x0004ff44);
	fixture->root = info.root;
	fixture->attached = true;
}

static void
fixture_free(struct fixture *fixture)
{
	if (fixture->table != NULL) {
		assert(hakios_dma_pt_destroy(&fixture->table) == 0 && fixture->table == NULL);
	}
	assert(!fixture->attached);
	free(fixture->visible);
	free(fixture->arena);
}

static void
assert_untouched(struct fixture *fixture)
{
	assert(fixture->table == NULL && fixture->calls == 0);
	for (uint32_t word = 0; word < fixture->count * WORDS; word++) {
		assert(fixture->arena[word] == 0xa5a5a5a5a5a5a5a5ull);
	}
}

static void
test_create_rejections(void)
{
	struct fixture fixture;
	fixture_allocate(&fixture, 4);
	struct hakios_dma_pt_config config = config_for(&fixture, false);
	struct hakios_dma_pt_operations missing = operations;
	missing.invalidate = NULL;
	assert(hakios_dma_pt_create(&config, &missing, &fixture, &fixture.table) == EINVAL);
	config.output_bits = 39;
	assert(hakios_dma_pt_create(&config, &operations, &fixture, &fixture.table) == EINVAL);
	config.output_bits = 40;
	config.table_count = HAKIOS_DMA_PT_TABLES_MAX + 1;
	assert(hakios_dma_pt_create(&config, &operations, &fixture, &fixture.table) == EINVAL);
	config.table_count = fixture.count;
	uint64_t physical = fixture.pages[1].physical;
	fixture.pages[1].physical = fixture.pages[0].physical;
	assert(hakios_dma_pt_create(&config, &operations, &fixture, &fixture.table) == EINVAL);
	fixture.pages[1].physical = physical + 1;
	assert(hakios_dma_pt_create(&config, &operations, &fixture, &fixture.table) == EINVAL);
	fixture.pages[1].physical = 1ull << 40;
	assert(hakios_dma_pt_create(&config, &operations, &fixture, &fixture.table) == EINVAL);
	fixture.pages[1].physical = physical;
	uint64_t *entries = fixture.pages[1].entries;
	fixture.pages[1].entries = fixture.pages[0].entries;
	assert(hakios_dma_pt_create(&config, &operations, &fixture, &fixture.table) == EINVAL);
	fixture.pages[1].entries = entries + 1;
	assert(hakios_dma_pt_create(&config, &operations, &fixture, &fixture.table) == EINVAL);
	fixture.pages[1].entries = entries;
	for (unsigned allocation = 1; allocation <= 2; allocation++) {
		pcie_test_fail_allocation_at = pcie_test_allocation_calls + allocation;
		assert(hakios_dma_pt_create(&config, &operations, &fixture, &fixture.table) == ENOMEM);
		assert_untouched(&fixture);
	}
	pcie_test_fail_allocation_at = 0;
	assert_untouched(&fixture);
	fixture_free(&fixture);
}

static void
test_geometry_attributes(void)
{
	const uint64_t bases[] = {0, 0x1ff000, 0x3ffff000, 0x100000000ull, (1ull << 39) - 8192};
	const uint32_t protections[] = {HAKIOS_DMA_PT_READ, RW, HAKIOS_DMA_PT_READ | HAKIOS_DMA_PT_CACHE,
	    RW | HAKIOS_DMA_PT_CACHE, RW | HAKIOS_DMA_PT_MMIO};
	for (unsigned nonsecure = 0; nonsecure < 2; nonsecure++) {
		for (unsigned region = 0; region < sizeof(bases) / sizeof(bases[0]); region++) {
			for (unsigned attributes = 0; attributes < sizeof(protections) / sizeof(protections[0]); attributes++) {
				struct fixture fixture;
				fixture_init(&fixture, 8, nonsecure != 0);
				uint64_t physical[] = {DATA_BASE, (1ull << 40) - PAGE_SIZE};
				uint32_t protection = protections[attributes];
				assert(hakios_dma_pt_map(fixture.table, bases[region], physical, 2, protection) == 0);
				assert(fixture.invalidations == 0);
				for (unsigned index = 1; index < fixture.trace_count; index++) {
					assert(fixture.trace[index - 1] > fixture.trace[index]);
				}
				publish(&fixture);
				for (unsigned page = 0; page < 2; page++) {
					uint64_t entry = device_lookup(&fixture, bases[region] + page * PAGE_SIZE);
					uint64_t expected = physical[page] | 3ull | (1ull << 6) | (2ull << 8) |
					    (1ull << 10) | (1ull << 11) | (3ull << 53);
					if ((protection & HAKIOS_DMA_PT_WRITE) == 0) {
						expected |= 1ull << 7;
					}
					if (protection & HAKIOS_DMA_PT_MMIO) {
						expected |= 2ull << 2;
					} else if (protection & HAKIOS_DMA_PT_CACHE) {
						expected |= 1ull << 2;
					}
					if (nonsecure != 0) {
						expected |= 1ull << 5;
					}
					assert(entry == expected);
				}
				assert((fixture.arena[(bases[region] >> 30) & 511] >> 63) == nonsecure);
				assert(hakios_dma_pt_unmap(fixture.table, bases[region], 2) == 0);
				assert(fixture.invalidations == 1 && device_lookup(&fixture, bases[region]) == 0);
				assert(hakios_dma_pt_map(fixture.table, bases[region], physical, 2, protection) == 0);
				fixture_free(&fixture);
			}
		}
	}
}

static void
test_atomic_preflight(void)
{
	struct fixture fixture;
	fixture_init(&fixture, 4, false);
	uint64_t physical[] = {DATA_BASE, DATA_BASE + 0x5000};
	uint64_t original[4 * WORDS];
	memcpy(original, fixture.arena, sizeof(original));
	assert(hakios_dma_pt_map(fixture.table, 0x3ffff000, physical, 2, RW) == ENOSPC);
	assert(hakios_dma_pt_map(fixture.table, 1, physical, 1, RW) == EINVAL);
	assert(hakios_dma_pt_map(fixture.table, 0, physical, 0, RW) == EINVAL);
	assert(hakios_dma_pt_map(fixture.table, 0, physical, HAKIOS_DMA_PT_PAGES_MAX + 1, RW) == EINVAL);
	assert(hakios_dma_pt_map(fixture.table, UINT64_MAX - 4095, physical, 1, RW) == EINVAL);
	assert(hakios_dma_pt_map(fixture.table, (1ull << 39) - 4096, physical, 2, RW) == EINVAL);
	assert(hakios_dma_pt_map(fixture.table, 0, physical, 1, HAKIOS_DMA_PT_WRITE) == EINVAL);
	assert(hakios_dma_pt_map(fixture.table, 0, physical, 1, RW | HAKIOS_DMA_PT_CACHE | HAKIOS_DMA_PT_MMIO) == EINVAL);
	assert(hakios_dma_pt_map(fixture.table, 0, physical, 1, RW | 0x100) == EINVAL);
	physical[1]++;
	assert(hakios_dma_pt_map(fixture.table, 0, physical, 2, RW) == EINVAL);
	physical[1] = fixture.pages[3].physical;
	assert(hakios_dma_pt_map(fixture.table, 0, physical, 2, RW) == EINVAL);
	assert(memcmp(original, fixture.arena, sizeof(original)) == 0 && fixture.calls == 0);
	assert(hakios_dma_pt_status(fixture.table) == 0 && fixture.calls == 0);
	physical[1] = DATA_BASE + 0x5000;
	assert(hakios_dma_pt_map(fixture.table, PAGE_SIZE, physical, 1, RW) == 0);
	memcpy(original, fixture.arena, sizeof(original));
	unsigned calls = fixture.calls;
	assert(hakios_dma_pt_map(fixture.table, 0, physical, 2, RW) == EEXIST);
	assert(hakios_dma_pt_unmap(fixture.table, 0, 2) == ENOENT);
	assert(hakios_dma_pt_unmap(fixture.table, PAGE_SIZE, 2) == ENOENT);
	assert(memcmp(original, fixture.arena, sizeof(original)) == 0 && fixture.calls == calls);
	assert(hakios_dma_pt_unmap(fixture.table, PAGE_SIZE, 1) == 0);
	fixture_free(&fixture);
}

static void
test_full_pool(void)
{
	struct fixture fixture;
	fixture_init(&fixture, 34, false);
	uint64_t *physical = calloc(HAKIOS_DMA_PT_PAGES_MAX, sizeof(*physical));
	assert(physical != NULL);
	for (unsigned index = 0; index < HAKIOS_DMA_PT_PAGES_MAX; index++) {
		physical[index] = DATA_BASE + (uint64_t)index * 0x5000;
	}
	publish(&fixture);
	assert(hakios_dma_pt_map(fixture.table, 0x100000000ull, physical, HAKIOS_DMA_PT_PAGES_MAX, RW) == 0);
	assert((device_lookup(&fixture, 0x103fff000ull) & ADDRESS_MASK) == physical[HAKIOS_DMA_PT_PAGES_MAX - 1]);
	assert(hakios_dma_pt_map(fixture.table, 0x140000000ull, physical, 1, RW) == ENOSPC);
	assert(hakios_dma_pt_unmap(fixture.table, 0x100000000ull, HAKIOS_DMA_PT_PAGES_MAX) == 0);
	assert(hakios_dma_pt_map(fixture.table, 0x100000000ull, physical, HAKIOS_DMA_PT_PAGES_MAX, RW) == 0);
	fixture_free(&fixture);
	free(physical);
}

static int
change_mapping(struct fixture *fixture, bool unmap)
{
	uint64_t physical[] = {DATA_BASE, DATA_BASE + 0x5000};
	return unmap ? hakios_dma_pt_unmap(fixture->table, 0x3ffff000, 2) :
	       hakios_dma_pt_map(fixture->table, 0x3ffff000, physical, 2, RW);
}

static void
prepare_fault_fixture(struct fixture *fixture, bool unmap)
{
	fixture_init(fixture, 8, false);
	if (unmap) {
		assert(change_mapping(fixture, false) == 0);
	}
	publish(fixture);
	fixture->calls = 0;
}

static void
test_callback_failures(void)
{
	for (unsigned unmap = 0; unmap < 2; unmap++) {
		struct fixture baseline;
		prepare_fault_fixture(&baseline, unmap != 0);
		assert(change_mapping(&baseline, unmap != 0) == 0);
		unsigned call_count = baseline.calls;
		fixture_free(&baseline);
		for (unsigned after = 0; after < 2; after++) {
			for (unsigned failure = 1; failure <= call_count; failure++) {
				struct fixture fixture;
				prepare_fault_fixture(&fixture, unmap != 0);
				fixture.fail_at = failure;
				fixture.callback_error = after != 0 ? -ETIMEDOUT : EIO;
				fixture.apply_before_error = after != 0;
				assert(change_mapping(&fixture, unmap != 0) == EIO);
				assert(hakios_dma_pt_status(fixture.table) == EIO);
				unsigned calls = fixture.calls;
				struct hakios_dma_pt_info info;
				memset(&info, 0xff, sizeof(info));
				assert(hakios_dma_pt_get_info(fixture.table, &info) == EIO && info.root == 0);
				assert(change_mapping(&fixture, false) == EIO);
				assert(change_mapping(&fixture, true) == EIO && fixture.calls == calls);
				fixture.fail_at = 0;
				fixture.quiesce_failure = true;
				struct hakios_dma_pagetable *retained = fixture.table;
				assert(hakios_dma_pt_destroy(&fixture.table) == ETIMEDOUT);
				assert(fixture.table == retained && fixture.attached);
				fixture.quiesce_failure = false;
				fixture_free(&fixture);
				failure_cases++;
			}
		}
	}
	struct fixture fixture;
	fixture_init(&fixture, 4, false);
	fixture.fail_at = 1;
	struct hakios_dma_pt_info info;
	memset(&info, 0xff, sizeof(info));
	assert(hakios_dma_pt_get_info(fixture.table, &info) == EIO && info.root == 0);
	fixture.fail_at = 0;
	fixture_free(&fixture);
	assert(fixture.quiesces == 0);
	fixture_init(&fixture, 4, false);
	publish(&fixture);
	fixture.fail_at = fixture.calls + 1;
	assert(hakios_dma_pt_destroy(&fixture.table) == EIO && fixture.table != NULL);
	fixture.fail_at = 0;
	fixture_free(&fixture);
}

static void
test_unpublished_failures(void)
{
	for (unsigned after = 0; after < 2; after++) {
		for (unsigned failure = 1; failure <= 3; failure++) {
			struct fixture fixture;
			fixture_init(&fixture, 4, false);
			fixture.fail_at = failure;
			fixture.apply_before_error = after != 0;
			fixture.callback_error = EINVAL;
			uint64_t physical = DATA_BASE;
			assert(hakios_dma_pt_map(fixture.table, 0, &physical, 1, RW) == EINVAL);
			assert(hakios_dma_pt_status(fixture.table) == EIO);
			struct hakios_dma_pt_info info;
			assert(hakios_dma_pt_get_info(fixture.table, &info) == EIO && info.root == 0);
			fixture_free(&fixture);
			assert(fixture.quiesces == 0 && fixture.invalidations == 0);
			failure_cases++;
		}
	}
}

static void
test_output_widths(void)
{
	const uint8_t widths[] = {32, 36, 40, 42, 44, 48};
	for (unsigned index = 0; index < sizeof(widths) / sizeof(widths[0]); index++) {
		struct fixture fixture;
		fixture_allocate(&fixture, 4);
		struct hakios_dma_pt_config config = config_for(&fixture, false);
		config.output_bits = widths[index];
		assert(hakios_dma_pt_create(&config, &operations, &fixture, &fixture.table) == 0);
		uint64_t physical = (1ull << widths[index]) - PAGE_SIZE;
		assert(hakios_dma_pt_map(fixture.table, 0, &physical, 1, RW) == 0);
		assert((walk(&fixture, 0, false) & ADDRESS_MASK) == physical);
		physical += PAGE_SIZE;
		assert(hakios_dma_pt_map(fixture.table, PAGE_SIZE, &physical, 1, RW) == EINVAL);
		struct hakios_dma_pt_info info;
		assert(hakios_dma_pt_get_info(fixture.table, &info) == 0 && info.output_bits == widths[index]);
		fixture.root = info.root;
		fixture.attached = true;
		fixture_free(&fixture);
	}
}

static void
test_stale_translation(void)
{
	struct fixture fixture;
	fixture_init(&fixture, 4, false);
	uint64_t physical = DATA_BASE;
	assert(hakios_dma_pt_map(fixture.table, 0x100000000ull, &physical, 1, RW) == 0);
	publish(&fixture);
	uint64_t cached = device_lookup(&fixture, 0x100000000ull);
	fixture.fail_at = fixture.calls + 2;
	fixture.callback_error = ETIMEDOUT;
	assert(hakios_dma_pt_unmap(fixture.table, 0x100000000ull, 1) == ETIMEDOUT);
	assert(walk(&fixture, 0x100000000ull, true) == 0);
	assert(device_lookup(&fixture, 0x100000000ull) == cached);
	fixture.quiesce_failure = true;
	assert(hakios_dma_pt_destroy(&fixture.table) == ETIMEDOUT && fixture.attached);
	fixture.quiesce_failure = false;
	fixture_free(&fixture);
}

static void
test_corruption(void)
{
	struct fixture fixture;
	fixture_init(&fixture, 4, false);
	publish(&fixture);
	fixture.arena[0] = 1;
	uint64_t physical = DATA_BASE;
	assert(hakios_dma_pt_map(fixture.table, 0, &physical, 1, RW) == EIO);
	struct hakios_dma_pt_info info;
	assert(hakios_dma_pt_get_info(fixture.table, &info) == EIO && info.root == 0);
	fixture_free(&fixture);
}

struct worker {
	struct hakios_dma_pagetable *table;
	uint64_t iova;
	uint64_t physical;
};

static void *
mapping_worker(void *context)
{
	struct worker *worker = context;
	for (unsigned iteration = 0; iteration < 100; iteration++) {
		assert(hakios_dma_pt_map(worker->table, worker->iova, &worker->physical, 1, RW) == 0);
		assert(hakios_dma_pt_unmap(worker->table, worker->iova, 1) == 0);
	}
	return NULL;
}

static void
test_concurrent_maps(void)
{
	struct fixture fixture;
	fixture_init(&fixture, 4, false);
	publish(&fixture);
	struct worker workers[4];
	pthread_t threads[4];
	for (unsigned index = 0; index < 4; index++) {
		workers[index] = (struct worker) {
			.table = fixture.table, .iova = 0x100000000ull + index * PAGE_SIZE,
			.physical = DATA_BASE + index * 0x5000,
		};
		assert(pthread_create(&threads[index], NULL, mapping_worker, &workers[index]) == 0);
	}
	for (unsigned index = 0; index < 4; index++) {
		assert(pthread_join(threads[index], NULL) == 0);
	}
	for (unsigned index = 0; index < 4; index++) {
		assert(walk(&fixture, workers[index].iova, true) == 0);
	}
	assert(fixture.invalidations == 800);
	fixture_free(&fixture);
}

int
main(void)
{
	test_create_rejections();
	test_geometry_attributes();
	test_atomic_preflight();
	test_full_pool();
	test_callback_failures();
	test_unpublished_failures();
	test_output_widths();
	test_stale_translation();
	test_corruption();
	test_concurrent_maps();
	printf("dma-pagetable-test: PASS (%u injected sync/invalidation failures; geometry, permissions, 64 MiB pool, stale TLB, teardown, concurrency)\n",
	    failure_cases);
	return 0;
}
