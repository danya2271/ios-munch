/* Fail-closed checks for a kernel-owned range against the ABL handoff FDT. */
#include "fdt.h"

#define FDT_MAGIC 0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9
#define FDT_MAX_DEPTH 32

struct fdt_view {
	uintptr_t base;
	uint32_t total_size;
	uint32_t struct_off;
	uint32_t struct_size;
	uint32_t strings_off;
	uint32_t strings_size;
	uint32_t reserve_off;
};

struct fdt_node {
	uint32_t address_cells;
	uint32_t size_cells;
	uint32_t reg_address_cells;
	uint32_t reg_size_cells;
	uint32_t reg_off;
	uint32_t reg_len;
	uint32_t size_off;
	uint32_t size_len;
	uint32_t alloc_off;
	uint32_t alloc_len;
	uint8_t has_size;
	uint8_t is_memory;
	uint8_t is_reserved;
	uint8_t is_reserved_entry;
	uint8_t is_chosen;
	uint8_t has_device_type;
	uint8_t is_memory_type;
	uint8_t is_disabled;
};

static uint32_t be32(uintptr_t address)
{
	return __builtin_bswap32(*(volatile uint32_t *)address);
}

static uint64_t be64(uintptr_t address)
{
	return (uint64_t)be32(address) << 32 | be32(address + 4);
}

static int span_ok(uint32_t total, uint32_t offset, uint32_t size)
{
	return offset <= total && size <= total - offset;
}

static int view_init(uintptr_t fdt, struct fdt_view *view)
{
	if (!fdt || be32(fdt) != FDT_MAGIC || be32(fdt + 20) < 17)
		return 0;
	view->base = fdt;
	view->total_size = be32(fdt + 4);
	view->struct_off = be32(fdt + 8);
	view->strings_off = be32(fdt + 12);
	view->reserve_off = be32(fdt + 16);
	view->strings_size = be32(fdt + 32);
	view->struct_size = be32(fdt + 36);
	return view->total_size >= 40 &&
	       span_ok(view->total_size, view->struct_off, view->struct_size) &&
	       span_ok(view->total_size, view->strings_off, view->strings_size) &&
	       view->reserve_off < view->total_size;
}

static uint32_t load32(const struct fdt_view *view, uint32_t offset)
{
	return be32(view->base + offset);
}

static int string_at(const struct fdt_view *view, uint32_t offset,
		     const char **value, uint32_t *length)
{
	if (offset >= view->strings_size)
		return 0;
	uintptr_t start = view->base + view->strings_off + offset;
	uint32_t available = view->strings_size - offset;
	uint32_t index;
	for (index = 0; index < available && *(volatile char *)(start + index); index++)
		;
	if (index == available)
		return 0;
	*value = (const char *)start;
	*length = index;
	return 1;
}

static int equals(const char *value, uint32_t length, const char *expected)
{
	uint32_t index = 0;
	while (expected[index]) {
		if (index == length || value[index] != expected[index])
			return 0;
		index++;
	}
	return index == length;
}

static int memory_node_name(const char *name, uint32_t length)
{
	return equals(name, length, "memory") ||
	       (length > 7 && name[0] == 'm' && name[1] == 'e' &&
		name[2] == 'm' && name[3] == 'o' && name[4] == 'r' &&
		name[5] == 'y' && name[6] == '@');
}

static int string_property(const struct fdt_view *view, uint32_t offset,
			   uint32_t length, const char *expected)
{
	uint32_t expected_len = 0;
	while (expected[expected_len])
		expected_len++;
	if (length != expected_len + 1 ||
	    !span_ok(view->total_size, offset, length))
		return 0;
	uintptr_t value = view->base + offset;
	for (uint32_t index = 0; index < expected_len; index++)
		if (*(volatile char *)(value + index) != expected[index])
			return 0;
	return *(volatile char *)(value + expected_len) == 0;
}

static int read_cells(const struct fdt_view *view, uint32_t offset,
		      uint32_t count, uint64_t *value)
{
	if (count < 1 || count > 2 ||
	    !span_ok(view->total_size, offset, count * 4))
		return 0;
	*value = count == 2 ? (uint64_t)load32(view, offset) << 32 |
				load32(view, offset + 4) : load32(view, offset);
	return 1;
}

static int range_end(uint64_t start, uint64_t size, uint64_t *end)
{
	if (!size || start > UINT64_MAX - size)
		return 0;
	*end = start + size;
	return 1;
}

static int overlaps(uint64_t start, uint64_t end,
		    uint64_t other_start, uint64_t other_size)
{
	uint64_t other_end;
	if (!range_end(other_start, other_size, &other_end))
		return 1;
	return start < other_end && other_start < end;
}

static int contains(uint64_t start, uint64_t end,
		    uint64_t other_start, uint64_t other_size)
{
	uint64_t other_end;
	return range_end(other_start, other_size, &other_end) &&
	       other_start >= start && other_end <= end;
}

static int check_reg_list(const struct fdt_view *view, uint32_t offset,
			 uint32_t length, uint32_t address_cells,
			 uint32_t size_cells, uint64_t base, uint64_t end,
			 int require_containment, int *contained)
{
	uint32_t tuple_cells = address_cells + size_cells;
	if (tuple_cells == 0 || tuple_cells > 4 || length == 0 ||
	    length % (tuple_cells * 4) ||
	    !span_ok(view->total_size, offset, length))
		return 0;
	for (uint32_t cursor = 0; cursor < length; cursor += tuple_cells * 4) {
		uint64_t start, size, range_hi;
		if (!read_cells(view, offset + cursor, address_cells, &start) ||
		    !read_cells(view, offset + cursor + address_cells * 4,
				size_cells, &size) ||
		    !range_end(start, size, &range_hi))
			return 0;
		if (require_containment) {
			if (contains(start, range_hi, base, end - base))
				*contained = 1;
		} else if (overlaps(base, end, start, size)) {
			return 0;
		}
	}
	return 1;
}

static int reserve_map_clear(const struct fdt_view *view,
			     uint64_t base, uint64_t end)
{
	uint32_t offset = view->reserve_off;
	while (span_ok(view->total_size, offset, 16)) {
		uint64_t start = be64(view->base + offset);
		uint64_t size = be64(view->base + offset + 8);
		offset += 16;
		if (!start && !size)
			return 1;
		if ((size && start > UINT64_MAX - size) ||
		    (size && overlaps(base, end, start, size)))
			return 0;
	}
	return 0;
}

static int check_dynamic_reservation(const struct fdt_view *view,
				     const struct fdt_node *node,
				     uint64_t base, uint64_t end)
{
	uint64_t reserved_size;
	if (!read_cells(view, node->size_off, node->reg_size_cells,
			&reserved_size) || !reserved_size) {
		return 0;
	}
	if (!node->alloc_len) {
		return 0;
	}
	uint32_t tuple_cells = node->reg_address_cells + node->reg_size_cells;
	if (!tuple_cells || tuple_cells > 4 ||
	    node->alloc_len % (tuple_cells * 4) ||
	    !span_ok(view->total_size, node->alloc_off, node->alloc_len))
		return 0;
	for (uint32_t offset = 0; offset < node->alloc_len;
	     offset += tuple_cells * 4) {
		uint64_t alloc_start, alloc_size;
		if (!read_cells(view, node->alloc_off + offset,
				node->reg_address_cells, &alloc_start) ||
		    !read_cells(view, node->alloc_off + offset +
				node->reg_address_cells * 4,
				node->reg_size_cells, &alloc_size) ||
		    overlaps(base, end, alloc_start, alloc_size)) {
			return 0;
		}
	}
	return 1;
}

int fdt_memory_range_usable(uintptr_t fdt, uint64_t base, uint64_t size)
{
	struct fdt_view view;
	uint64_t end;
	if (!view_init(fdt, &view) || !range_end(base, size, &end) ||
	    !reserve_map_clear(&view, base, end))
		return 0;

	struct fdt_node nodes[FDT_MAX_DEPTH];
	uint32_t cursor = view.struct_off;
	uint32_t limit = view.struct_off + view.struct_size;
	unsigned int depth = 0;
	int found_memory = 0;
	int usable_seen = 0;
	int usable_contains = 0;
	int ended = 0;

	while (cursor <= limit && span_ok(limit, cursor, 4)) {
		uint32_t token = load32(&view, cursor);
		cursor += 4;
		if (token == FDT_BEGIN_NODE) {
			uint32_t name_len = 0;
			while (cursor + name_len < limit &&
			       *(volatile char *)(view.base + cursor + name_len))
				name_len++;
			if (cursor + name_len >= limit || depth == FDT_MAX_DEPTH)
				return 0;
			struct fdt_node *node = &nodes[depth];
			for (unsigned int index = 0; index < sizeof(*node); index++)
				((uint8_t *)node)[index] = 0;
			if (depth) {
				node->address_cells = nodes[depth - 1].address_cells;
				node->size_cells = nodes[depth - 1].size_cells;
				node->reg_address_cells = node->address_cells;
				node->reg_size_cells = node->size_cells;
			} else {
				node->address_cells = 2;
				node->size_cells = 1;
				node->reg_address_cells = 2;
				node->reg_size_cells = 1;
			}
			if (depth == 1) {
				const char *name = (const char *)(view.base + cursor);
				node->is_memory = memory_node_name(name, name_len);
				node->is_reserved = equals(name, name_len, "reserved-memory");
				node->is_chosen = equals(name, name_len, "chosen");
			} else if (depth == 2 && nodes[1].is_reserved) {
				node->is_reserved_entry = 1;
			}
			depth++;
			cursor = (cursor + name_len + 4) & ~3U;
		} else if (token == FDT_END_NODE) {
			if (!depth)
				return 0;
			struct fdt_node *node = &nodes[depth - 1];
			if (node->is_memory && !node->is_disabled &&
			    (!node->has_device_type || node->is_memory_type)) {
				if (!check_reg_list(&view, node->reg_off, node->reg_len,
						    node->reg_address_cells,
						    node->reg_size_cells, base, end,
						    1, &found_memory)) {
					return 0;
				}
			}
			if (node->is_reserved_entry) {
				if (node->reg_len) {
					int ignored = 0;
					if (!check_reg_list(&view, node->reg_off, node->reg_len,
							    node->reg_address_cells,
							    node->reg_size_cells, base, end,
							    0, &ignored))
						return 0;
				} else if (node->has_size &&
					   !check_dynamic_reservation(&view, node,
							     base, end)) {
					return 0;
				}
			}
			depth--;
		} else if (token == FDT_PROP) {
			if (!depth || !span_ok(limit, cursor, 8))
				return 0;
			uint32_t length = load32(&view, cursor);
			uint32_t name_offset = load32(&view, cursor + 4);
			cursor += 8;
			if (!span_ok(limit, cursor, length))
				return 0;
			const char *name;
			uint32_t name_len;
			if (!string_at(&view, name_offset, &name, &name_len))
				return 0;
			struct fdt_node *node = &nodes[depth - 1];
			if ((depth == 1 || node->is_reserved) &&
			    (equals(name, name_len, "#address-cells") ||
			     equals(name, name_len, "#size-cells"))) {
				uint64_t value;
				if (length != 4 || !read_cells(&view, cursor, 1, &value) ||
				    value < 1 || value > 2)
					return 0;
				if (equals(name, name_len, "#address-cells"))
					node->address_cells = value;
				else
					node->size_cells = value;
			} else if (equals(name, name_len, "reg")) {
				node->reg_off = cursor;
				node->reg_len = length;
			} else if (equals(name, name_len, "size")) {
				node->size_off = cursor;
				node->size_len = length;
				node->has_size = 1;
			} else if (equals(name, name_len, "alloc-ranges")) {
				node->alloc_off = cursor;
				node->alloc_len = length;
			} else if (node->is_memory &&
				   equals(name, name_len, "device_type")) {
				node->has_device_type = 1;
				node->is_memory_type = string_property(&view, cursor,
								      length, "memory");
			} else if (node->is_memory && equals(name, name_len, "status")) {
				node->is_disabled =
					!string_property(&view, cursor, length, "okay") &&
					!string_property(&view, cursor, length, "ok");
			} else if (node->is_chosen &&
				   equals(name, name_len, "linux,usable-memory-range")) {
				uint32_t tuple_cells = node->address_cells + node->size_cells;
				uint64_t usable_base, usable_size, usable_end;
				if (!tuple_cells || tuple_cells > 4 ||
				    length != tuple_cells * 4 ||
				    !read_cells(&view, cursor, node->address_cells,
						&usable_base) ||
				    !read_cells(&view, cursor + node->address_cells * 4,
						node->size_cells, &usable_size) ||
				    !range_end(usable_base, usable_size, &usable_end))
					return 0;
				usable_seen = 1;
				usable_contains = contains(usable_base, usable_end,
						   base, size);
			}
			cursor = (cursor + length + 3) & ~3U;
		} else if (token == FDT_NOP) {
			continue;
		} else if (token == FDT_END) {
			if (depth)
				return 0;
			ended = 1;
			break;
		} else {
			return 0;
		}
	}
	return ended && found_memory && (!usable_seen || usable_contains);
}
