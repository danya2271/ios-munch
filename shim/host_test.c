/* Render the console layout on the host: ./out/host_test && view out/host.ppm */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"
#include "fdt.h"

static uint32_t host_fb[FB_H * FB_STRIDE / 4];

static int test_fdt_range(int argc, char **argv)
{
	if (argc != 6) {
		fprintf(stderr, "usage: %s --fdt-range DTB BASE SIZE EXPECTED\n", argv[0]);
		return 2;
	}
	errno = 0;
	char *end;
	uint64_t base = strtoull(argv[3], &end, 0);
	if (errno || *end)
		return 2;
	uint64_t size = strtoull(argv[4], &end, 0);
	if (errno || *end)
		return 2;
	if (strcmp(argv[5], "0") && strcmp(argv[5], "1"))
		return 2;
	int expected = !strcmp(argv[5], "1");
	FILE *file = fopen(argv[2], "rb");
	if (!file || fseek(file, 0, SEEK_END))
		return 2;
	long length = ftell(file);
	if (length <= 0 || fseek(file, 0, SEEK_SET))
		return 2;
	void *blob = malloc((size_t)length);
	if (!blob || fread(blob, 1, (size_t)length, file) != (size_t)length)
		return 2;
	fclose(file);
	int usable = fdt_memory_range_usable((uintptr_t)blob, base, size);
	printf("range 0x%llx+0x%llx: %s (expected %s)\n",
	       (unsigned long long)base, (unsigned long long)size,
	       usable ? "usable" : "blocked", expected ? "usable" : "blocked");
	free(blob);
	return usable == expected ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		return !strcmp(argv[1], "--fdt-range") ? test_fdt_range(argc, argv) : 2;
	con_fb = host_fb;
	con_clear(RED);
	con_str("SHIM", "munch milestone 2");
	con_hex("MMFR0", 0x0000000000101122ULL);
	con_hex2("PSCI", "ver=", 0x10001, "smccc=", 0x10002);
	con_hex2("CPU7", "ret=", 0, "mpidr=", 0x81000700);
	con_clear(BLUE);
	con_str("PANIC", "sync exception");
	con_hex("ESR", 0x96000010);
	FILE *f = fopen("out/host.ppm", "wb");
	fprintf(f, "P6 %d %d 255\n", FB_W, FB_H);
	for (int i = 0; i < FB_W * FB_H; i++) {
		unsigned char px[3] = { host_fb[i] >> 16, host_fb[i] >> 8, host_fb[i] };
		fwrite(px, 1, 3, f);
	}
	fclose(f);
	return 0;
}
