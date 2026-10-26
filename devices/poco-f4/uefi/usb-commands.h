/* SPDX-License-Identifier: MIT */
static int probe_equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Complete fastboot replies. Unknown commands have no side effects. */
static const char *probe_command(const char *command)
{
    if (probe_equal(command, "getvar:product")) return "OKAYmunch-usb-probe";
    if (probe_equal(command, "getvar:version")) return "OKAY0.1";
    if (probe_equal(command, "getvar:probe")) return "OKAYusb-roundtrip";
    if (probe_equal(command, "oem ping")) return "OKAYpong";
    if (probe_equal(command, "reboot")) return "OKAY";
    return "FAILunsupported probe command";
}
