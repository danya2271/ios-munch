#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.dma.pagetable.h>

#define PT_INPUT_BITS 39u
#define PT_ADDRESS_MASK 0x0000fffffffff000ull
#define PT_TYPE 3ull
#define PT_NONSECURE (1ull << 5)
#define PT_UNPRIVILEGED (1ull << 6)
#define PT_READ_ONLY (1ull << 7)
#define PT_OUTER_SHAREABLE (2ull << 8)
#define PT_ACCESS_FLAG (1ull << 10)
#define PT_NOT_GLOBAL (1ull << 11)
#define PT_NO_EXECUTE (3ull << 53)
#define PT_NONSECURE_TABLE (1ull << 63)
#define PT_MAIR0 0x0004ff44u

struct pt_node {
	struct hakios_dma_pt_page page;
	uint8_t level;
	bool dirty;
};

struct hakios_dma_pagetable {
	lck_mtx_t lock;
	struct hakios_dma_pt_operations operations;
	struct pt_node *nodes;
	void *owner;
	uint32_t capacity;
	uint32_t used;
	uint8_t output_bits;
	bool nonsecure;
	bool published;
	bool faulted;
};

static LCK_GRP_DECLARE(pt_lock_group, "hakios-dma-pagetable");

static int
pt_error(int error)
{
	return error < 0 ? EIO : error;
}

static bool
pt_output_bits_valid(uint8_t bits)
{
	return bits == 32 || bits == 36 || bits == 40 || bits == 42 || bits == 44 || bits == 48;
}

static bool
pt_physical_valid(uint64_t physical, uint8_t bits)
{
	return (physical & (HAKIOS_DMA_PT_PAGE_SIZE - 1)) == 0 &&
	       physical <= (1ull << bits) - HAKIOS_DMA_PT_PAGE_SIZE;
}

static bool
pt_range_valid(uint64_t iova, uint32_t page_count)
{
	uint64_t size = (uint64_t)page_count * HAKIOS_DMA_PT_PAGE_SIZE;

	return page_count != 0 && page_count <= HAKIOS_DMA_PT_PAGES_MAX &&
	       (iova & (HAKIOS_DMA_PT_PAGE_SIZE - 1)) == 0 && iova <= (1ull << PT_INPUT_BITS) - size;
}

static uint64_t
pt_table_attributes(struct hakios_dma_pagetable *table)
{
	return PT_TYPE | (table->nonsecure ? PT_NONSECURE_TABLE : 0);
}

static uint64_t
pt_leaf_attributes(struct hakios_dma_pagetable *table, uint32_t protection)
{
	uint64_t attributes = PT_TYPE | PT_UNPRIVILEGED | PT_OUTER_SHAREABLE |
	    PT_ACCESS_FLAG | PT_NOT_GLOBAL | PT_NO_EXECUTE;

	if ((protection & HAKIOS_DMA_PT_WRITE) == 0) {
		attributes |= PT_READ_ONLY;
	}
	if (protection & HAKIOS_DMA_PT_MMIO) {
		attributes |= 2ull << 2;
	} else if (protection & HAKIOS_DMA_PT_CACHE) {
		attributes |= 1ull << 2;
	}
	return attributes | (table->nonsecure ? PT_NONSECURE : 0);
}

static bool
pt_leaf_valid(struct hakios_dma_pagetable *table, uint64_t entry)
{
	uint64_t required = pt_leaf_attributes(table, HAKIOS_DMA_PT_READ | HAKIOS_DMA_PT_WRITE);
	uint64_t variable = PT_ADDRESS_MASK | PT_READ_ONLY | (7ull << 2);

	return (entry & ~variable) == required && ((entry >> 2) & 7) <= 2 &&
	       pt_physical_valid(entry & PT_ADDRESS_MASK, table->output_bits);
}

static struct pt_node *
pt_child(struct hakios_dma_pagetable *table, uint64_t entry, uint8_t level)
{
	if ((entry & ~PT_ADDRESS_MASK) != pt_table_attributes(table)) {
		return NULL;
	}
	for (uint32_t index = 1; index < table->used; index++) {
		struct pt_node *node = &table->nodes[index];
		if (node->page.physical == (entry & PT_ADDRESS_MASK) && node->level == level) {
			return node;
		}
	}
	return NULL;
}

static int
pt_walk(struct hakios_dma_pagetable *table, uint64_t iova, bool create, struct pt_node **leaf)
{
	struct pt_node *node = &table->nodes[0];

	for (uint8_t level = 1; level < 3; level++) {
		uint32_t index = (uint32_t)((iova >> (PT_INPUT_BITS - level * 9)) & 511);
		uint64_t *entry = &node->page.entries[index];
		struct pt_node *child;

		if (*entry == 0) {
			if (!create) {
				return ENOENT;
			}
			if (table->used == table->capacity) {
				return ENOSPC;
			}
			child = &table->nodes[table->used++];
			child->level = level + 1;
			child->dirty = true;
			*entry = child->page.physical | pt_table_attributes(table);
			node->dirty = true;
		} else {
			child = pt_child(table, *entry, level + 1);
			if (child == NULL) {
				return EIO;
			}
		}
		node = child;
	}
	*leaf = node;
	return 0;
}

static int
pt_prepare_map(struct hakios_dma_pagetable *table, uint64_t iova,
    const uint64_t *physical_pages, uint32_t page_count)
{
	uint32_t needed = 0;
	uint64_t missing_upper = UINT64_MAX, missing_lower = UINT64_MAX;

	for (uint32_t page = 0; page < page_count; page++) {
		if (!pt_physical_valid(physical_pages[page], table->output_bits)) {
			return EINVAL;
		}
		for (uint32_t index = 0; index < table->capacity; index++) {
			if (physical_pages[page] == table->nodes[index].page.physical) {
				return EINVAL;
			}
		}
		uint64_t address = iova + (uint64_t)page * HAKIOS_DMA_PT_PAGE_SIZE;
		uint64_t upper = table->nodes[0].page.entries[(address >> 30) & 511];
		uint64_t lower = 0;

		if (upper != 0) {
			struct pt_node *middle = pt_child(table, upper, 2);
			if (middle == NULL) {
				return EIO;
			}
			lower = middle->page.entries[(address >> 21) & 511];
		} else if (missing_upper != (address >> 30)) {
			missing_upper = address >> 30;
			needed++;
		}
		if (lower != 0) {
			struct pt_node *leaf = pt_child(table, lower, 3);
			if (leaf == NULL) {
				return EIO;
			}
			uint64_t entry = leaf->page.entries[(address >> 12) & 511];
			if (entry != 0) {
				return pt_leaf_valid(table, entry) ? EEXIST : EIO;
			}
		} else if (missing_lower != (address >> 21)) {
			missing_lower = address >> 21;
			needed++;
		}
		if (needed > table->capacity - table->used) {
			return ENOSPC;
		}
	}
	return 0;
}

static int
pt_sync(struct hakios_dma_pagetable *table)
{
	for (uint32_t index = table->used; index != 0; index--) {
		struct pt_node *node = &table->nodes[index - 1];
		if (node->dirty) {
			int error = pt_error(table->operations.sync(table->owner, &node->page));
			if (error != 0) {
				return error;
			}
			node->dirty = false;
		}
	}
	return 0;
}

static int
pt_commit(struct hakios_dma_pagetable *table, uint64_t iova, uint32_t page_count)
{
	int error = pt_sync(table);

	if (error == 0 && table->published) {
		error = pt_error(table->operations.invalidate(table->owner, iova,
		    (uint64_t)page_count * HAKIOS_DMA_PT_PAGE_SIZE));
	}
	if (error != 0) {
		table->faulted = true;
	}
	return error;
}

int
hakios_dma_pt_create(const struct hakios_dma_pt_config *config,
    const struct hakios_dma_pt_operations *operations, void *owner, struct hakios_dma_pagetable **result)
{
	struct hakios_dma_pagetable *table;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || operations == NULL || owner == NULL || config->pages == NULL ||
	    config->table_count == 0 || config->table_count > HAKIOS_DMA_PT_TABLES_MAX ||
	    !pt_output_bits_valid(config->output_bits) || operations->version != HAKIOS_DMA_PT_VERSION ||
	    operations->sync == NULL || operations->invalidate == NULL || operations->quiesce == NULL) {
		return EINVAL;
	}
	for (uint32_t index = 0; index < config->table_count; index++) {
		const struct hakios_dma_pt_page *page = &config->pages[index];
		if (page->entries == NULL || ((uintptr_t)page->entries & (HAKIOS_DMA_PT_PAGE_SIZE - 1)) != 0 ||
		    page->physical == 0 || !pt_physical_valid(page->physical, config->output_bits)) {
			return EINVAL;
		}
		for (uint32_t previous = 0; previous < index; previous++) {
			if (page->physical == config->pages[previous].physical || page->entries == config->pages[previous].entries) {
				return EINVAL;
			}
		}
	}
	table = kalloc_type(struct hakios_dma_pagetable, Z_WAITOK | Z_ZERO);
	if (table == NULL) {
		return ENOMEM;
	}
	table->nodes = kalloc_type(struct pt_node, config->table_count, Z_WAITOK | Z_ZERO);
	if (table->nodes == NULL) {
		kfree_type(struct hakios_dma_pagetable, table);
		return ENOMEM;
	}
	lck_mtx_init(&table->lock, &pt_lock_group, LCK_ATTR_NULL);
	table->operations = *operations;
	table->owner = owner;
	table->capacity = config->table_count;
	table->output_bits = config->output_bits;
	table->nonsecure = config->nonsecure;
	table->used = 1;
	for (uint32_t index = 0; index < table->capacity; index++) {
		table->nodes[index].page = config->pages[index];
		bzero(table->nodes[index].page.entries, HAKIOS_DMA_PT_PAGE_SIZE);
	}
	table->nodes[0].level = 1;
	table->nodes[0].dirty = true;
	*result = table;
	return 0;
}

int
hakios_dma_pt_get_info(struct hakios_dma_pagetable *table, struct hakios_dma_pt_info *info)
{
	int error;

	if (info == NULL) {
		return EINVAL;
	}
	bzero(info, sizeof(*info));
	if (table == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&table->lock);
	error = table->faulted ? EIO : pt_sync(table);
	if (error != 0) {
		table->faulted = true;
	} else {
		info->root = table->nodes[0].page.physical;
		info->granule = HAKIOS_DMA_PT_PAGE_SIZE;
		info->mair0 = PT_MAIR0;
		info->input_bits = PT_INPUT_BITS;
		info->output_bits = table->output_bits;
		info->start_level = 1;
		info->nonsecure = table->nonsecure;
		table->published = true;
	}
	lck_mtx_unlock(&table->lock);
	return error;
}

int
hakios_dma_pt_status(struct hakios_dma_pagetable *table)
{
	int error;

	if (table == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&table->lock);
	error = table->faulted ? EIO : 0;
	lck_mtx_unlock(&table->lock);
	return error;
}

int
hakios_dma_pt_map(struct hakios_dma_pagetable *table, uint64_t iova,
    const uint64_t *physical_pages, uint32_t page_count, uint32_t protection)
{
	uint32_t allowed = HAKIOS_DMA_PT_READ | HAKIOS_DMA_PT_WRITE | HAKIOS_DMA_PT_CACHE | HAKIOS_DMA_PT_MMIO;
	int error;

	if (table == NULL || physical_pages == NULL || !pt_range_valid(iova, page_count) ||
	    (protection & ~allowed) != 0 || (protection & HAKIOS_DMA_PT_READ) == 0 ||
	    ((protection & HAKIOS_DMA_PT_CACHE) != 0 && (protection & HAKIOS_DMA_PT_MMIO) != 0)) {
		return EINVAL;
	}
	lck_mtx_lock(&table->lock);
	error = table->faulted ? EIO : pt_prepare_map(table, iova, physical_pages, page_count);
	if (error != 0) {
		if (error == EIO) {
			table->faulted = true;
		}
		goto out;
	}
	uint64_t attributes = pt_leaf_attributes(table, protection);
	for (uint32_t page = 0; page < page_count; page++) {
		uint64_t address = iova + (uint64_t)page * HAKIOS_DMA_PT_PAGE_SIZE;
		struct pt_node *leaf;
		error = pt_walk(table, address, true, &leaf);
		if (error != 0) {
			table->faulted = true;
			goto out;
		}
		leaf->page.entries[(address >> 12) & 511] = physical_pages[page] | attributes;
		leaf->dirty = true;
	}
	error = pt_commit(table, iova, page_count);
out:
	lck_mtx_unlock(&table->lock);
	return error;
}

int
hakios_dma_pt_unmap(struct hakios_dma_pagetable *table, uint64_t iova, uint32_t page_count)
{
	int error = 0;

	if (table == NULL || !pt_range_valid(iova, page_count)) {
		return EINVAL;
	}
	lck_mtx_lock(&table->lock);
	if (table->faulted) {
		error = EIO;
		goto out;
	}
	for (uint32_t page = 0; page < page_count; page++) {
		uint64_t address = iova + (uint64_t)page * HAKIOS_DMA_PT_PAGE_SIZE;
		struct pt_node *leaf;
		error = pt_walk(table, address, false, &leaf);
		if (error != 0) {
			goto out;
		}
		uint64_t entry = leaf->page.entries[(address >> 12) & 511];
		if (entry == 0 || !pt_leaf_valid(table, entry)) {
			error = entry == 0 ? ENOENT : EIO;
			goto out;
		}
	}
	for (uint32_t page = 0; page < page_count; page++) {
		uint64_t address = iova + (uint64_t)page * HAKIOS_DMA_PT_PAGE_SIZE;
		struct pt_node *leaf;
		error = pt_walk(table, address, false, &leaf);
		if (error != 0) {
			table->faulted = true;
			goto out;
		}
		leaf->page.entries[(address >> 12) & 511] = 0;
		leaf->dirty = true;
	}
	error = pt_commit(table, iova, page_count);
out:
	if (error == EIO) {
		table->faulted = true;
	}
	lck_mtx_unlock(&table->lock);
	return error;
}

int
hakios_dma_pt_destroy(struct hakios_dma_pagetable **table)
{
	struct hakios_dma_pagetable *state;
	int error = 0;

	if (table == NULL || *table == NULL) {
		return EINVAL;
	}
	state = *table;
	lck_mtx_lock(&state->lock);
	if (state->published) {
		error = pt_error(state->operations.quiesce(state->owner));
	}
	if (error != 0) {
		state->faulted = true;
		lck_mtx_unlock(&state->lock);
		return error;
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &pt_lock_group);
	kfree_type(struct pt_node, state->capacity, state->nodes);
	kfree_type(struct hakios_dma_pagetable, state);
	*table = NULL;
	return 0;
}
