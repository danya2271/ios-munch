#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/hakios.dma.space.h>

#define TABLES 8u
#define PAGE_SIZE 4096u
#define WORDS 512u
#define ADDRESS_MASK 0x0000fffffffff000ull
#define DATA_BASE 0xa0000000ull
#define RW (HAKIOS_DMA_PT_READ | HAKIOS_DMA_PT_WRITE)

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;
static unsigned failure_cases;

struct fixture {
	struct hakios_dma_space *space;
	struct hakios_dma_pt_page pages[TABLES];
	uint64_t *cpu;
	uint64_t *visible;
	uint64_t base;
	uint64_t limit;
	uint64_t cached_iova;
	uint64_t cached_entry;
	unsigned calls;
	unsigned fail_at;
	unsigned quiesces;
	int callback_error;
	bool apply_before_error;
	bool attached;
	bool cached;
};

static unsigned
table_index(struct fixture *fixture, uint64_t physical)
{
	for (unsigned index = 0; index < TABLES; index++) {
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
	uint64_t *storage = visible ? fixture->visible : fixture->cpu;
	uint64_t *table = storage;
	const unsigned shifts[] = {30, 21, 12};
	for (unsigned level = 0; level < 3; level++) {
		uint64_t entry = table[(iova >> shifts[level]) & 511];
		if (entry == 0 || level == 2) {
			return entry;
		}
		assert((entry & 3) == 3);
		table = &storage[table_index(fixture, entry & ADDRESS_MASK) * WORDS];
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
	unsigned index = table_index(fixture, page->physical);
	assert(page->entries == fixture->pages[index].entries);
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
	assert(iova >= fixture->base && size <= fixture->limit - iova);
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
	fixture->quiesces++;
	int error = callback(fixture);
	if (error == 0 || fixture->apply_before_error) {
		fixture->attached = false;
		fixture->cached = false;
	}
	return error;
}

static const struct hakios_dma_pt_operations operations = {
	.version = HAKIOS_DMA_PT_VERSION,
	.sync = sync_page, .invalidate = invalidate, .quiesce = quiesce,
};

static struct hakios_dma_space_config
fixture_allocate(struct fixture *fixture, uint64_t base, uint64_t limit)
{
	memset(fixture, 0, sizeof(*fixture));
	fixture->base = base;
	fixture->limit = limit;
	fixture->callback_error = EIO;
	fixture->cpu = aligned_alloc(PAGE_SIZE, TABLES * PAGE_SIZE);
	fixture->visible = malloc(TABLES * PAGE_SIZE);
	assert(fixture->cpu != NULL && fixture->visible != NULL);
	memset(fixture->cpu, 0xa5, TABLES * PAGE_SIZE);
	memset(fixture->visible, 0xa5, TABLES * PAGE_SIZE);
	for (unsigned index = 0; index < TABLES; index++) {
		fixture->pages[index] = (struct hakios_dma_pt_page) {
			.entries = &fixture->cpu[index * WORDS], .physical = 0x90000000ull + index * 0x3000,
		};
	}
	return (struct hakios_dma_space_config) {
		.tables = {.pages = fixture->pages, .table_count = TABLES, .output_bits = 40},
		.iova_base = base, .iova_limit = limit,
	};
}

static void
fixture_init(struct fixture *fixture, uint64_t base, uint64_t limit)
{
	struct hakios_dma_space_config config = fixture_allocate(fixture, base, limit);
	assert(hakios_dma_space_create(&config, &operations, fixture, &fixture->space) == 0);
	assert(fixture->calls == 0 && hakios_dma_space_status(fixture->space) == 0);
}

static void
publish(struct fixture *fixture)
{
	struct hakios_dma_pt_info info;
	assert(hakios_dma_space_get_info(fixture->space, &info) == 0);
	assert(info.root == fixture->pages[0].physical && info.input_bits == 39 && info.granule == PAGE_SIZE);
	fixture->attached = true;
}

static void
fixture_free(struct fixture *fixture)
{
	if (fixture->space != NULL) {
		assert(hakios_dma_space_destroy(&fixture->space) == 0);
	}
	assert(!fixture->attached);
	free(fixture->cpu);
	free(fixture->visible);
}

static hakios_dma_lease_t
reserve(struct fixture *fixture, uint32_t count, uint64_t alignment, uint64_t boundary)
{
	hakios_dma_lease_t lease = 0;
	assert(hakios_dma_space_reserve(fixture->space, count, alignment, boundary, &lease) == 0);
	assert(lease != 0);
	return lease;
}

static uint64_t
map_pages(struct fixture *fixture, hakios_dma_lease_t lease, uint32_t count, uint64_t physical)
{
	uint64_t pages[16], iova = 0;
	assert(count <= 16);
	for (unsigned index = 0; index < count; index++) {
		pages[index] = physical + index * 0x5000;
	}
	assert(hakios_dma_space_map(fixture->space, lease, pages, count, RW, &iova) == 0);
	assert(iova != 0);
	return iova;
}

static void
test_creation_and_allocation(void)
{
	struct fixture fixture;
	struct hakios_dma_space_config config = fixture_allocate(&fixture, PAGE_SIZE, 0x10000);
	struct hakios_dma_space_config invalid = config;
	assert(hakios_dma_space_create(NULL, &operations, &fixture, &fixture.space) == EINVAL);
	invalid.iova_base = 0;
	assert(hakios_dma_space_create(&invalid, &operations, &fixture, &fixture.space) == EINVAL);
	invalid.iova_base = 0x1001;
	assert(hakios_dma_space_create(&invalid, &operations, &fixture, &fixture.space) == EINVAL);
	invalid = config;
	invalid.iova_limit = UINT64_MAX;
	assert(hakios_dma_space_create(&invalid, &operations, &fixture, &fixture.space) == EINVAL);
	invalid.iova_limit = invalid.iova_base;
	assert(hakios_dma_space_create(&invalid, &operations, &fixture, &fixture.space) == EINVAL);
	for (unsigned allocation = 1; allocation <= 3; allocation++) {
		pcie_test_fail_allocation_at = pcie_test_allocation_calls + allocation;
		assert(hakios_dma_space_create(&config, &operations, &fixture, &fixture.space) == ENOMEM);
		assert(fixture.space == NULL && fixture.calls == 0);
		for (unsigned word = 0; word < TABLES * WORDS; word++) {
			assert(fixture.cpu[word] == 0xa5a5a5a5a5a5a5a5ull);
		}
	}
	pcie_test_fail_allocation_at = 0;
	assert(hakios_dma_space_create(&config, &operations, &fixture, &fixture.space) == 0);
	pcie_test_fail_allocation = true;
	hakios_dma_lease_t lease = UINT64_MAX;
	assert(hakios_dma_space_reserve(fixture.space, 1, PAGE_SIZE, 0, &lease) == ENOMEM && lease == 0);
	pcie_test_fail_allocation = false;
	lease = reserve(&fixture, 1, PAGE_SIZE, 0);
	assert(map_pages(&fixture, lease, 1, DATA_BASE) == PAGE_SIZE);
	fixture_free(&fixture);
	assert(fixture.quiesces == 0);
}

static void
test_alignment_bounds_and_lifetime(void)
{
	struct fixture fixture;
	fixture_init(&fixture, 0x7000, 0x20000);
	publish(&fixture);
	hakios_dma_lease_t first = reserve(&fixture, 2, PAGE_SIZE, 0x8000);
	assert(map_pages(&fixture, first, 2, DATA_BASE) == 0x8000);
	hakios_dma_lease_t second = reserve(&fixture, 1, 0x10000, PAGE_SIZE);
	assert(map_pages(&fixture, second, 1, DATA_BASE + 0x10000) == 0x10000);
	hakios_dma_lease_t third = reserve(&fixture, 1, PAGE_SIZE, 0);
	assert(map_pages(&fixture, third, 1, DATA_BASE + 0x20000) == 0x7000);
	uint64_t iova = UINT64_MAX, physical = DATA_BASE;
	assert(hakios_dma_space_map(fixture.space, first, &physical, 1, RW, &iova) == EBUSY && iova == 0);
	hakios_dma_lease_t retained = first;
	assert(hakios_dma_space_release(fixture.space, &first) == EBUSY && first == retained);
	assert(hakios_dma_space_unmap(fixture.space, first) == 0);
	assert(hakios_dma_space_unmap(fixture.space, first) == ENOENT);
	assert(hakios_dma_space_release(fixture.space, &first) == 0 && first == 0);
	assert(hakios_dma_space_release(fixture.space, &retained) == ESTALE);
	assert(hakios_dma_space_unmap(fixture.space, retained) == ESTALE);
	assert(hakios_dma_space_map(fixture.space, retained, &physical, 1, RW, &iova) == ESTALE && iova == 0);
	first = reserve(&fixture, 2, PAGE_SIZE, 0);
	assert(first != retained && map_pages(&fixture, first, 2, DATA_BASE) == 0x8000);
	fixture_free(&fixture);
	fixture_init(&fixture, (1ull << 39) - 2 * PAGE_SIZE, 1ull << 39);
	first = reserve(&fixture, 2, PAGE_SIZE, 0);
	assert(map_pages(&fixture, first, 2, DATA_BASE) == (1ull << 39) - 2 * PAGE_SIZE);
	second = UINT64_MAX;
	assert(hakios_dma_space_reserve(fixture.space, 1, PAGE_SIZE, 0, &second) == ENOSPC && second == 0);
	fixture_free(&fixture);
}

static void
test_validation_and_wrong_space(void)
{
	struct fixture fixture, other;
	fixture_init(&fixture, PAGE_SIZE, 0x10000);
	fixture_init(&other, PAGE_SIZE, 0x10000);
	hakios_dma_lease_t first = reserve(&fixture, 2, PAGE_SIZE, 0);
	hakios_dma_lease_t second = reserve(&other, 2, PAGE_SIZE, 0);
	assert(first != second);
	uint64_t physical[] = {DATA_BASE, DATA_BASE + 0x5000}, iova = UINT64_MAX;
	assert(hakios_dma_space_map(fixture.space, second, physical, 2, RW, &iova) == ESTALE && iova == 0);
	assert(hakios_dma_space_release(fixture.space, &second) == ESTALE && second != 0);
	assert(hakios_dma_space_map(fixture.space, first, physical, 1, RW, &iova) == EINVAL && iova == 0);
	physical[1]++;
	assert(hakios_dma_space_map(fixture.space, first, physical, 2, RW, &iova) == EINVAL && iova == 0);
	assert(hakios_dma_space_status(fixture.space) == 0 && fixture.calls == 0);
	physical[1] = fixture.pages[7].physical;
	assert(hakios_dma_space_map(fixture.space, first, physical, 2, RW, &iova) == EINVAL && iova == 0);
	assert(hakios_dma_space_release(fixture.space, &first) == 0);
	const uint64_t alignments[] = {0, 1, PAGE_SIZE - 1, PAGE_SIZE + 1, UINT64_MAX, 1ull << 40};
	for (unsigned index = 0; index < sizeof(alignments) / sizeof(alignments[0]); index++) {
		first = UINT64_MAX;
		assert(hakios_dma_space_reserve(fixture.space, 1, alignments[index], 0, &first) == EINVAL && first == 0);
	}
	assert(hakios_dma_space_reserve(fixture.space, 2, PAGE_SIZE, PAGE_SIZE, &first) == EINVAL);
	assert(hakios_dma_space_reserve(fixture.space, 1, PAGE_SIZE, 0x3000, &first) == EINVAL);
	assert(hakios_dma_space_reserve(fixture.space, 0, PAGE_SIZE, 0, &first) == EINVAL);
	assert(hakios_dma_space_reserve(fixture.space, HAKIOS_DMA_PT_PAGES_MAX + 1, PAGE_SIZE, 0, &first) == EINVAL);
	assert(hakios_dma_space_reserve(fixture.space, 1, 1ull << 39, 0, &first) == ENOSPC);
	assert(hakios_dma_space_status(fixture.space) == 0 && fixture.calls == 0);
	fixture_free(&fixture);
	fixture_free(&other);
}

static void
test_capacity(void)
{
	struct fixture fixture;
	fixture_init(&fixture, PAGE_SIZE, (uint64_t)(HAKIOS_DMA_SPACE_RESERVATIONS_MAX + 2) * PAGE_SIZE);
	hakios_dma_lease_t *leases = calloc(HAKIOS_DMA_SPACE_RESERVATIONS_MAX, sizeof(*leases));
	assert(leases != NULL);
	for (unsigned index = 0; index < HAKIOS_DMA_SPACE_RESERVATIONS_MAX; index++) {
		leases[index] = reserve(&fixture, 1, PAGE_SIZE, 0);
	}
	hakios_dma_lease_t extra = UINT64_MAX;
	assert(hakios_dma_space_reserve(fixture.space, 1, PAGE_SIZE, 0, &extra) == ENOSPC && extra == 0);
	unsigned middle = HAKIOS_DMA_SPACE_RESERVATIONS_MAX / 2;
	hakios_dma_lease_t stale = leases[middle];
	assert(hakios_dma_space_release(fixture.space, &leases[middle]) == 0);
	leases[middle] = reserve(&fixture, 1, PAGE_SIZE, 0);
	assert(leases[middle] != stale);
	assert(map_pages(&fixture, leases[middle], 1, DATA_BASE) == (uint64_t)(middle + 1) * PAGE_SIZE);
	fixture_free(&fixture);
	free(leases);
}

static void
test_table_exhaustion(void)
{
	struct fixture fixture;
	struct hakios_dma_space_config config = fixture_allocate(&fixture, PAGE_SIZE, 0x400000);
	config.tables.table_count = 3;
	assert(hakios_dma_space_create(&config, &operations, &fixture, &fixture.space) == 0);
	hakios_dma_lease_t first = reserve(&fixture, 1, PAGE_SIZE, 0);
	assert(map_pages(&fixture, first, 1, DATA_BASE) == PAGE_SIZE);
	hakios_dma_lease_t gap = reserve(&fixture, 510, PAGE_SIZE, 0);
	hakios_dma_lease_t target = reserve(&fixture, 1, PAGE_SIZE, 0);
	uint64_t physical = DATA_BASE + 0x5000, iova = UINT64_MAX;
	unsigned calls = fixture.calls;
	assert(hakios_dma_space_map(fixture.space, target, &physical, 1, RW, &iova) == ENOSPC && iova == 0);
	assert(hakios_dma_space_status(fixture.space) == 0 && fixture.calls == calls);
	assert(hakios_dma_space_release(fixture.space, &target) == 0);
	assert(hakios_dma_space_release(fixture.space, &gap) == 0);
	target = reserve(&fixture, 1, PAGE_SIZE, 0);
	assert(map_pages(&fixture, target, 1, physical) == 2 * PAGE_SIZE);
	fixture_free(&fixture);
}

static void
test_destroy_and_recreate(void)
{
	struct fixture fixture;
	fixture_init(&fixture, PAGE_SIZE, 0x10000);
	hakios_dma_lease_t stale = reserve(&fixture, 1, PAGE_SIZE, 0);
	assert(map_pages(&fixture, stale, 1, DATA_BASE) == PAGE_SIZE);
	publish(&fixture);
	fixture.fail_at = fixture.calls + 1;
	fixture.callback_error = EINVAL;
	fixture.apply_before_error = true;
	struct hakios_dma_space *retained = fixture.space;
	assert(hakios_dma_space_destroy(&fixture.space) == EINVAL && fixture.space == retained);
	assert(!fixture.attached && hakios_dma_space_status(fixture.space) == EIO);
	hakios_dma_lease_t saved = stale;
	assert(hakios_dma_space_release(fixture.space, &stale) == EIO && stale == saved);
	fixture.fail_at = 0;
	fixture_free(&fixture);
	assert(fixture.quiesces == 2);
	fixture_init(&fixture, PAGE_SIZE, 0x10000);
	hakios_dma_lease_t fresh = reserve(&fixture, 1, PAGE_SIZE, 0);
	assert(fresh != stale);
	uint64_t physical = DATA_BASE, iova = UINT64_MAX;
	assert(hakios_dma_space_map(fixture.space, stale, &physical, 1, RW, &iova) == ESTALE && iova == 0);
	assert(map_pages(&fixture, fresh, 1, DATA_BASE) == PAGE_SIZE);
	fixture_free(&fixture);
}

static int
change_mapping(struct fixture *fixture, hakios_dma_lease_t lease, bool unmap, uint64_t *iova)
{
	const uint64_t pages[] = {DATA_BASE, DATA_BASE + 0x5000};
	return unmap ? hakios_dma_space_unmap(fixture->space, lease) :
	       hakios_dma_space_map(fixture->space, lease, pages, 2, RW, iova);
}

static hakios_dma_lease_t
fault_fixture(struct fixture *fixture, bool unmap)
{
	fixture_init(fixture, 0x3ffff000, 0x40010000);
	hakios_dma_lease_t lease = reserve(fixture, 2, PAGE_SIZE, 0);
	if (unmap) {
		assert(map_pages(fixture, lease, 2, DATA_BASE) == fixture->base);
	}
	publish(fixture);
	fixture->calls = 0;
	return lease;
}

static void
assert_quarantined(struct fixture *fixture, hakios_dma_lease_t lease)
{
	unsigned calls = fixture->calls;
	hakios_dma_lease_t retained = lease, extra = UINT64_MAX;
	uint64_t iova = UINT64_MAX, physical = DATA_BASE;
	assert(hakios_dma_space_status(fixture->space) == EIO);
	assert(hakios_dma_space_release(fixture->space, &lease) == EIO && lease == retained);
	assert(hakios_dma_space_reserve(fixture->space, 1, PAGE_SIZE, 0, &extra) == EIO && extra == 0);
	assert(hakios_dma_space_map(fixture->space, lease, &physical, 1, RW, &iova) == EIO && iova == 0);
	assert(hakios_dma_space_unmap(fixture->space, lease) == EIO);
	struct hakios_dma_pt_info info;
	memset(&info, 0xff, sizeof(info));
	assert(hakios_dma_space_get_info(fixture->space, &info) == EIO && info.root == 0);
	assert(fixture->calls == calls);
}

static void
test_fault_retention(void)
{
	const int errors[] = {EIO, EINVAL, -ETIMEDOUT};
	for (unsigned unmap = 0; unmap < 2; unmap++) {
		struct fixture baseline;
		hakios_dma_lease_t lease = fault_fixture(&baseline, unmap != 0);
		uint64_t iova = 0;
		assert(change_mapping(&baseline, lease, unmap != 0, &iova) == 0);
		unsigned call_count = baseline.calls;
		fixture_free(&baseline);
		for (unsigned after = 0; after < 2; after++) {
			for (unsigned failure = 1; failure <= call_count; failure++) {
				for (unsigned code = 0; code < sizeof(errors) / sizeof(errors[0]); code++) {
					struct fixture fixture;
					lease = fault_fixture(&fixture, unmap != 0);
					hakios_dma_lease_t unused = reserve(&fixture, 1, PAGE_SIZE, 0);
					fixture.fail_at = failure;
					fixture.callback_error = errors[code];
					fixture.apply_before_error = after != 0;
					iova = 0;
					assert(change_mapping(&fixture, lease, unmap != 0, &iova) == (errors[code] < 0 ? EIO : errors[code]));
					assert(iova == 0);
					assert_quarantined(&fixture, lease);
					assert_quarantined(&fixture, unused);
					struct hakios_dma_space *retained = fixture.space;
					fixture.fail_at = fixture.calls + 1;
					assert(hakios_dma_space_destroy(&fixture.space) != 0 && fixture.space == retained);
					assert_quarantined(&fixture, lease);
					fixture.fail_at = 0;
					fixture_free(&fixture);
					failure_cases++;
				}
			}
		}
	}
}

static void
test_publication_and_stale_translation(void)
{
	struct fixture fixture;
	fixture_init(&fixture, PAGE_SIZE, 0x10000);
	hakios_dma_lease_t lease = reserve(&fixture, 1, PAGE_SIZE, 0);
	fixture.fail_at = 1;
	fixture.callback_error = EINVAL;
	struct hakios_dma_pt_info info;
	assert(hakios_dma_space_get_info(fixture.space, &info) == EINVAL && info.root == 0);
	assert_quarantined(&fixture, lease);
	fixture_free(&fixture);
	assert(fixture.quiesces == 0);
	fixture_init(&fixture, PAGE_SIZE, 0x10000);
	lease = reserve(&fixture, 1, PAGE_SIZE, 0);
	fixture.fail_at = 1;
	uint64_t iova = UINT64_MAX, physical = DATA_BASE;
	assert(hakios_dma_space_map(fixture.space, lease, &physical, 1, RW, &iova) == EIO && iova == 0);
	assert_quarantined(&fixture, lease);
	fixture_free(&fixture);
	assert(fixture.quiesces == 0);
	fixture_init(&fixture, PAGE_SIZE, 0x10000);
	lease = reserve(&fixture, 1, PAGE_SIZE, 0);
	iova = map_pages(&fixture, lease, 1, DATA_BASE);
	publish(&fixture);
	uint64_t cached = device_lookup(&fixture, iova);
	fixture.fail_at = fixture.calls + 2;
	fixture.callback_error = ETIMEDOUT;
	assert(hakios_dma_space_unmap(fixture.space, lease) == ETIMEDOUT);
	assert(walk(&fixture, iova, true) == 0 && device_lookup(&fixture, iova) == cached);
	assert_quarantined(&fixture, lease);
	fixture.fail_at = fixture.calls + 1;
	assert(hakios_dma_space_destroy(&fixture.space) == ETIMEDOUT && fixture.space != NULL);
	assert(device_lookup(&fixture, iova) == cached);
	fixture.fail_at = 0;
	fixture_free(&fixture);
}

static void
test_accounting_mismatch(void)
{
	for (unsigned missing = 0; missing < 2; missing++) {
		struct fixture fixture;
		fixture_init(&fixture, PAGE_SIZE, 0x10000);
		hakios_dma_lease_t lease = reserve(&fixture, 1, PAGE_SIZE, 0);
		uint64_t iova = map_pages(&fixture, lease, 1, DATA_BASE);
		publish(&fixture);
		uint64_t *middle = &fixture.cpu[table_index(&fixture, fixture.cpu[0] & ADDRESS_MASK) * WORDS];
		uint64_t *leaf = &fixture.cpu[table_index(&fixture, middle[0] & ADDRESS_MASK) * WORDS];
		uint64_t saved = leaf[1], physical = DATA_BASE;
		if (missing == 0) {
			assert(hakios_dma_space_unmap(fixture.space, lease) == 0);
			leaf[1] = saved;
			assert(hakios_dma_space_map(fixture.space, lease, &physical, 1, RW, &iova) == EEXIST && iova == 0);
		} else {
			leaf[1] = 0;
			assert(hakios_dma_space_unmap(fixture.space, lease) == ENOENT);
		}
		assert_quarantined(&fixture, lease);
		fixture_free(&fixture);
	}
}

struct model_slot {
	hakios_dma_lease_t lease;
	unsigned first;
	unsigned count;
	bool mapped;
};

static unsigned
random_value(uint32_t *state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state >> 16;
}

static void
test_model_sequences(void)
{
	struct fixture fixture;
	fixture_init(&fixture, 0x3ffe0000, 0x40020000);
	publish(&fixture);
	struct model_slot slots[64] = {0};
	bool occupied[64] = {0};
	uint32_t random = 0x63900002u;
	for (unsigned iteration = 0; iteration < 10000; iteration++) {
		unsigned selected = random_value(&random) % 64;
		struct model_slot *slot = &slots[selected];
		if (slot->lease == 0) {
			unsigned count = 1 + random_value(&random) % 8;
			uint64_t alignment = (uint64_t)PAGE_SIZE << (random_value(&random) % 4);
			uint64_t boundary = random_value(&random) % 2 ? 0x10000 : 0;
			unsigned expected = 64;
			for (unsigned first = 0; first + count <= 64; first++) {
				uint64_t address = fixture.base + first * PAGE_SIZE;
				if (address % alignment != 0 || (boundary != 0 && address / boundary !=
				    (address + count * PAGE_SIZE - 1) / boundary)) {
					continue;
				}
				bool free_range = true;
				for (unsigned page = first; page < first + count; page++) {
					free_range = free_range && !occupied[page];
				}
				if (free_range) {
					expected = first;
					break;
				}
			}
			int error = hakios_dma_space_reserve(fixture.space, count, alignment, boundary, &slot->lease);
			assert(error == (expected == 64 ? ENOSPC : 0));
			if (error == 0) {
				slot->first = expected;
				slot->count = count;
				for (unsigned page = expected; page < expected + count; page++) {
					occupied[page] = true;
				}
			}
		} else if (random_value(&random) % 3 == 0) {
			hakios_dma_lease_t retained = slot->lease;
			assert(hakios_dma_space_release(fixture.space, &slot->lease) == (slot->mapped ? EBUSY : 0));
			if (slot->mapped) {
				assert(slot->lease == retained);
			} else {
				for (unsigned page = slot->first; page < slot->first + slot->count; page++) {
					occupied[page] = false;
				}
			}
		} else if (slot->mapped) {
			assert(hakios_dma_space_unmap(fixture.space, slot->lease) == 0);
			slot->mapped = false;
		} else {
			assert(map_pages(&fixture, slot->lease, slot->count, DATA_BASE + selected * 0x100000) ==
			    fixture.base + slot->first * PAGE_SIZE);
			slot->mapped = true;
		}
		uint64_t expected_pages[64] = {0};
		for (unsigned index = 0; index < 64; index++) {
			if (slots[index].lease != 0 && slots[index].mapped) {
				for (unsigned page = 0; page < slots[index].count; page++) {
					expected_pages[slots[index].first + page] = DATA_BASE + index * 0x100000 + page * 0x5000;
				}
			}
		}
		for (unsigned page = 0; page < 64; page++) {
			assert((walk(&fixture, fixture.base + page * PAGE_SIZE, true) & ADDRESS_MASK) == expected_pages[page]);
		}
	}
	fixture_free(&fixture);
}

struct concurrent_context {
	struct fixture *fixture;
	pthread_mutex_t lock;
	uint64_t active[4];
};

struct worker {
	struct concurrent_context *context;
	unsigned index;
};

static void *
mapping_worker(void *argument)
{
	struct worker *worker = argument;
	struct concurrent_context *context = worker->context;
	for (unsigned iteration = 0; iteration < 100; iteration++) {
		hakios_dma_lease_t lease = reserve(context->fixture, 2, PAGE_SIZE, 0);
		uint64_t iova = map_pages(context->fixture, lease, 2, DATA_BASE + worker->index * 0x100000);
		assert(pthread_mutex_lock(&context->lock) == 0);
		for (unsigned index = 0; index < 4; index++) {
			assert(context->active[index] == 0 || iova + 2 * PAGE_SIZE <= context->active[index] ||
			    iova >= context->active[index] + 2 * PAGE_SIZE);
		}
		context->active[worker->index] = iova;
		assert(pthread_mutex_unlock(&context->lock) == 0);
		sched_yield();
		assert(hakios_dma_space_unmap(context->fixture->space, lease) == 0);
		assert(pthread_mutex_lock(&context->lock) == 0);
		context->active[worker->index] = 0;
		assert(pthread_mutex_unlock(&context->lock) == 0);
		assert(hakios_dma_space_release(context->fixture->space, &lease) == 0 && lease == 0);
	}
	return NULL;
}

static void
test_concurrent_lifetimes(void)
{
	struct fixture fixture;
	fixture_init(&fixture, 0x100000000ull, 0x100010000ull);
	publish(&fixture);
	struct concurrent_context context = {.fixture = &fixture};
	assert(pthread_mutex_init(&context.lock, NULL) == 0);
	struct worker workers[4];
	pthread_t threads[4];
	for (unsigned index = 0; index < 4; index++) {
		workers[index] = (struct worker) {.context = &context, .index = index};
		assert(pthread_create(&threads[index], NULL, mapping_worker, &workers[index]) == 0);
	}
	for (unsigned index = 0; index < 4; index++) {
		assert(pthread_join(threads[index], NULL) == 0);
	}
	for (uint64_t iova = fixture.base; iova < fixture.limit; iova += PAGE_SIZE) {
		assert(walk(&fixture, iova, true) == 0);
	}
	assert(pthread_mutex_destroy(&context.lock) == 0);
	fixture_free(&fixture);
}

int
main(void)
{
	test_creation_and_allocation();
	test_alignment_bounds_and_lifetime();
	test_validation_and_wrong_space();
	test_capacity();
	test_table_exhaustion();
	test_destroy_and_recreate();
	test_fault_retention();
	test_publication_and_stale_translation();
	test_accounting_mismatch();
	test_model_sequences();
	test_concurrent_lifetimes();
	printf("dma-space-test: PASS (%u injected map/unmap failures; IOVA model, leases, quarantine, teardown, concurrency)\n", failure_cases);
	return 0;
}
