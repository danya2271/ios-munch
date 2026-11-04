#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>

#include "core.h"
#include "debug.h"
#include "lx.h"
#include <lx_os.h>

static void
driver_says(const char *kind, const char *format, va_list list)
{
	char line[320];
	size_t length;

	vsnprintf(line, sizeof(line), format, list);
	length = strlen(line);
	printf("ath11k%s: %s%s", kind, line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
}

void
ath11k_info(struct ath11k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says("", format, list);
	va_end(list);
}

static void
driver_complains(const char *kind, const char *format, va_list list)
{
	char line[320];

	vsnprintf(line, sizeof(line), format, list);
	lx_tell("ath11k %s: %s%s", kind, line, line[0] != 0 && line[strlen(line) - 1] == '\n' ? "" : "\n");
}

void
ath11k_warn(struct ath11k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_complains("WARNS", format, list);
	va_end(list);
}

void
ath11k_err(struct ath11k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_complains("ERROR", format, list);
	va_end(list);
}

void
__ath11k_dbg(struct ath11k_base *ab, enum ath11k_debug_mask mask, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says(" (debug)", format, list);
	va_end(list);
}

void
ath11k_dbg_dump(struct ath11k_base *ab, enum ath11k_debug_mask mask, const char *msg, const char *prefix,
    const void *buf, size_t len)
{
	(void)ab;
	(void)mask;
	(void)msg;
	(void)prefix;
	(void)buf;
	(void)len;
}

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	(void)np;
	(void)name;
	(void)out;
	return -EINVAL;
}

bool
of_machine_is_compatible(const char *compatible)
{
	(void)compatible;
	return false;
}

int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
	(void)np;
	(void)index;
	(void)res;
	return -ENODEV;
}

int
linuxkpi_device_get_mac_address(struct device *dev, char *address)
{
	(void)dev;
	return lx_os_radio_address((unsigned char *)address) ? 0 : -ENOENT;
}
