#!/usr/bin/env python3
import argparse
import base64
import datetime
import json
import pathlib
import re
import shlex
import shutil
import subprocess


ENDPOINT = "/sys/bus/pci/devices/0000:01:00.0"
DT = "/sys/firmware/devicetree/base/soc"
DEFAULT_MSI_NODE = "qcom,pcie0_msi@17a10040"
PCI_CAPABILITY_PTR = 0x34
PCI_STATUS = 0x06
PCI_COMMAND = 0x04


def config_u16(data, offset):
    if offset < 0 or offset + 2 > len(data):
        return None
    return int.from_bytes(data[offset:offset + 2], "little")


def config_u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        return None
    return int.from_bytes(data[offset:offset + 4], "little")


def decode_pci_config(data):
    result = {"length": len(data), "readable": len(data) >= 64, "capabilities": [], "errors": []}
    command = config_u16(data, PCI_COMMAND)
    status = config_u16(data, PCI_STATUS)
    if command is not None:
        result["command"] = command
        result["memory_decode"] = bool(command & 0x2)
        result["bus_master"] = bool(command & 0x4)
    if status is None:
        result["readable"] = False
        result["errors"].append("missing command/status")
        return result

    result["status"] = status
    result["capability_list"] = bool(status & 0x10)
    pointer = data[PCI_CAPABILITY_PTR] if len(data) > PCI_CAPABILITY_PTR else 0
    result["capability_pointer"] = pointer
    visited = set()
    for _ in range(48):
        if pointer == 0:
            break
        if pointer < 0x40 or pointer >= len(data) or (pointer & 3):
            result["errors"].append(f"invalid capability pointer 0x{pointer:02x}")
            break
        if pointer in visited:
            result["errors"].append(f"capability cycle at 0x{pointer:02x}")
            break
        visited.add(pointer)
        if pointer + 2 > len(data):
            result["errors"].append(f"truncated capability at 0x{pointer:02x}")
            break
        capability_id = data[pointer]
        next_pointer = data[pointer + 1]
        capability = {"offset": pointer, "id": capability_id, "next": next_pointer}
        if capability_id == 0x05:
            control = config_u16(data, pointer + 2)
            if control is None:
                result["errors"].append(f"truncated MSI capability at 0x{pointer:02x}")
            else:
                is_64bit = bool(control & 0x80)
                address_low = config_u32(data, pointer + 4)
                address_high = config_u32(data, pointer + 8) if is_64bit else 0
                data_offset = pointer + (12 if is_64bit else 8)
                message_data = config_u16(data, data_offset)
                msi = {
                    "control": control,
                    "enabled": bool(control & 0x1),
                    "multiple_message_capable": (control >> 1) & 0x7,
                    "multiple_message_enable": (control >> 4) & 0x7,
                    "vector_count": 1 << ((control >> 4) & 0x7),
                    "is_64bit": is_64bit,
                    "mask_capable": bool(control & 0x100),
                    "address": None if address_low is None or address_high is None else address_low | (address_high << 32),
                    "data": message_data,
                }
                if control & 0x100:
                    mask_offset = data_offset + 2
                    pending_offset = mask_offset + 4
                    msi["mask"] = config_u32(data, mask_offset)
                    msi["pending"] = config_u32(data, pending_offset)
                capability["msi"] = msi
                result["msi"] = msi
        result["capabilities"].append(capability)
        if next_pointer == pointer:
            result["errors"].append(f"self-referencing capability at 0x{pointer:02x}")
            break
        pointer = next_pointer
    else:
        result["errors"].append("capability chain exceeds 48 entries")
    return result


def decode_msi_interrupts(record):
    try:
        raw = base64.b64decode(record.get("stdout_b64", ""), validate=True).decode("utf-8", errors="replace")
    except (ValueError, TypeError):
        return []
    decoded = []
    pattern = re.compile(r"^\s*(\d+):.*\s+msm_pci_msi\s+(\d+)\s+\S+\s+(.*)$")
    for line in raw.splitlines():
        match = pattern.match(line)
        if match:
            decoded.append({"linux_irq": int(match.group(1)), "controller_value": int(match.group(2)),
                            "name": match.group(3).strip()})
    return decoded


def probes(msi_node):
    result = [("kernel", "uname -r"), ("model", "getprop ro.product.device")]
    result.append(("endpoint/config", f"cat {shlex.quote(f'{ENDPOINT}/config')}"))
    for name in ("vendor", "device", "revision", "class", "resource", "power/runtime_status"):
        result.append((f"endpoint/{name}", f"cat {shlex.quote(f'{ENDPOINT}/{name}')}"))
    for name in ("driver", "iommu_group", "of_node"):
        result.append((f"endpoint/{name}", f"readlink -f {shlex.quote(f'{ENDPOINT}/{name}')}"))
    result.extend([
        ("endpoint/path", f"readlink -f {ENDPOINT}"),
        ("endpoint/msi_irqs", f"ls -l {ENDPOINT}/msi_irqs"),
        ("endpoint/msi_irq_metadata", "for irq in $(ls -1 " + f"{ENDPOINT}/msi_irqs" + " 2>/dev/null); do printf 'irq=%s\\n' \"$irq\"; for field in chip_name hwirq effective_affinity; do printf '%s=' \"$field\"; cat \"/proc/irq/$irq/$field\" 2>/dev/null || true; done; cat \"/sys/kernel/debug/irq/irqs/$irq\" 2>/dev/null || true; done"),
        ("interrupts", "cat /proc/interrupts"),
        ("iomem", "cat /proc/iomem"),
        ("platform-drivers", "ls -ld /sys/bus/platform/devices/*pcie*/driver /sys/bus/platform/devices/*smmu*/driver"),
    ])
    for node, properties in (
        ("qcom,pcie@1c00000", ("compatible", "reg", "reg-names", "iommu-map", "msi-parent", "clock-names", "reset-names")),
        (msi_node, ("compatible", "reg", "interrupts", "qcom,msi-gicm-addr")),
        ("interrupt-controller@17a00000", ("compatible", "reg", "#interrupt-cells")),
    ):
        for name in properties:
            result.append((f"dt/{node}/{name}", f"cat {shlex.quote(f'{DT}/{node}/{name}')}"))
    return result


def invoke(adb, serial, arguments, timeout=10):
    try:
        completed = subprocess.run([adb, "-s", serial, *arguments], check=False,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return {"returncode": completed.returncode, "stdout_b64": base64.b64encode(completed.stdout).decode("ascii"),
                "stderr": completed.stderr.decode("utf-8", errors="replace")}
    except subprocess.TimeoutExpired:
        return {"returncode": None, "stdout_b64": "", "stderr": "ADB command timed out"}
    except OSError as error:
        return {"returncode": None, "stdout_b64": "", "stderr": str(error)}


def invoke_command(adb, serial, command, privileged=False, timeout=10):
    if privileged:
        arguments = ["exec-out", "su", "-c", command]
    else:
        arguments = ["exec-out", command]
    return invoke(adb, serial, arguments, timeout=timeout)


def discover_msi_node(adb, serial, privileged):
    command = f"find {shlex.quote(DT)} -maxdepth 1 -type d -name '*msi*' -print"
    record = invoke_command(adb, serial, command, privileged=privileged)
    record["command"] = command
    if record["returncode"] != 0:
        return DEFAULT_MSI_NODE, record
    try:
        raw = base64.b64decode(record["stdout_b64"], validate=True).decode("utf-8", errors="replace")
    except (ValueError, UnicodeDecodeError):
        return DEFAULT_MSI_NODE, record
    candidates = []
    for line in raw.splitlines():
        path = line.strip().rstrip("/")
        if path.startswith(DT + "/"):
            candidates.append(path.rsplit("/", 1)[-1])
    for candidate in candidates:
        if candidate.startswith("qcom,pcie0_msi@"):
            return candidate, record
    return candidates[0] if candidates else DEFAULT_MSI_NODE, record


def evaluate(records):
    expected = {"vendor": 0x17CB, "device": 0x1101, "revision": 1, "class": 0x028000}
    checks = {}
    for name, value in expected.items():
        record = records.get(f"endpoint/{name}", {})
        try:
            raw = base64.b64decode(record.get("stdout_b64", ""), validate=True)
            observed = int(raw.strip(), 16) if record.get("returncode") == 0 else None
        except (ValueError, TypeError):
            observed = None
        checks[name] = {"expected": value, "observed": observed, "match": observed == value}
    config = records.get("endpoint/config", {})
    try:
        config_bytes = base64.b64decode(config.get("stdout_b64", ""), validate=True)
        config_readable = config.get("returncode") == 0 and len(config_bytes) >= 64 and config_bytes[:16] != b"\xff" * 16
    except (ValueError, TypeError):
        config_bytes = b""
        config_readable = False
    config_decode = decode_pci_config(config_bytes) if config_readable else {"readable": False}
    return {"endpoint_identity_matches": all(check["match"] for check in checks.values()), "identity": checks,
            "endpoint_config_readable": config_readable, "config_decode": config_decode,
            "linux_msi_interrupts": decode_msi_interrupts(records.get("interrupts", {})),
            "ownership_transferred": False, "safe_to_enable_bus_master": False,
            "note": "Android sysfs/DT observations are not a resident-UEFI to XNU ownership grant."}


def collect(adb, serial, privileged=False):
    connection = invoke(adb, serial, ["get-state"])
    connected = connection["returncode"] == 0 and base64.b64decode(connection["stdout_b64"]).strip() == b"device"
    records = {}
    if connected:
        msi_node, discovery = discover_msi_node(adb, serial, privileged)
        records["dt/msi-node-discovery"] = {"command": discovery.get("command", ""), **discovery,
                                             "selected_node": msi_node}
        for name, command in probes(msi_node):
            records[name] = {"command": command, **invoke_command(adb, serial, command, privileged=privileged)}
    return {"schema": 2, "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "connected": connected, "privileged": privileged, "connection": connection,
            "records": records, "assessment": evaluate(records)}


def main():
    parser = argparse.ArgumentParser(description="Read-only Android Wi-Fi ownership evidence; never an XNU hardware grant.")
    parser.add_argument("--serial", required=True, help="Explicit adb device serial; no automatic device selection")
    parser.add_argument("--output", required=True, type=pathlib.Path, help="New host JSON file; existing files are not overwritten")
    parser.add_argument("--adb", default=shutil.which("adb") or "adb")
    parser.add_argument("--su", action="store_true", help="Run read-only probes through the device's su -c")
    args = parser.parse_args()
    if not args.serial or args.serial.startswith("-"):
        parser.error("a nonempty explicit serial is required")
    try:
        with args.output.open("x", encoding="utf-8") as output:
            report = collect(args.adb, args.serial, privileged=args.su)
            json.dump(report, output, indent=2)
            output.write("\n")
    except OSError as error:
        parser.exit(1, f"cannot create evidence file: {error}\n")
    print(f"Saved read-only evidence to {args.output}; no ownership transfer is inferred.")
    return 0 if report["connected"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
