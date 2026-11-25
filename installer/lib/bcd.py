"""Build and read Windows boot configuration stores (BCD) offline with libhivex.

A BCD store is a registry hive: Description (KeyName/System/TreatAsSystem) and Objects\\{guid}\\Description\\Type +
Objects\\{guid}\\Elements\\<type hex>\\Element. The element type's bits 24-27 give the value format. Formats used
here were taken from the store bcdboot wrote for the Toshiba install (build/boot21/efi-bcd/BCD.ondisk); the device
format was cross-checked against that disk's saved partition table (its disk GUID and ESP GUID match).
"""
import ctypes
import struct
import uuid

from hive import Hive, L, N, walk, children, REG_SZ, REG_BINARY, REG_DWORD, REG_MULTI_SZ

L.hivex_node_delete_child.argtypes = [ctypes.c_void_p, N]

BOOTMGR = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}"
GLOBALSETTINGS = "{7ea2e1ac-2e61-4728-aaa3-896d9d0a9f0e}"
DBGSETTINGS = "{4636856e-540f-4170-a130-a84776f4c654}"
EMSSETTINGS = "{0ce4991b-e6b3-4b16-b23c-5e0d9250e5d9}"
BADMEMORY = "{5189b25c-5558-4bf2-bca4-289b11bd29e2}"
BOOTLOADERSETTINGS = "{6efb52bf-1766-41db-a6b3-0ee5eff72bd7}"
HYPERVISORSETTINGS = "{7ff607e0-4395-11db-b0de-0800200c9a66}"

# element types (BcdLibrary*/BcdBootMgr*/BcdOSLoader*) used here
E = {
    "device": 0x11000001, "path": 0x12000002, "description": 0x12000004, "locale": 0x12000005,
    "inherit": 0x14000006, "recoveryenabled": 0x16000009, "debugtype": 0x15000011, "bootems": 0x16000020,
    "testsigning": 0x16000049, "isolatedcontext": 0x16000060, "allowedinmemorysettings": 0x17000077,
    "osdevice": 0x21000001, "systemroot": 0x22000002, "default": 0x23000003, "displayorder": 0x24000001,
    "timeout": 0x25000004, "nx": 0x25000020, "bootmenupolicy": 0x250000c2, "bootstatuspolicy": 0x250000e0,
    "hypervisorlaunchtype_f3": 0x250000f3, "hypervisor_f4": 0x250000f4, "hypervisor_f5": 0x250000f5,
    "nocrashautoreboot": 0x26000024, "usefirmwarepcisettings": 0x26000070,
}
NAMES = {v: k for k, v in E.items()}
FMT = {1: "device", 2: "string", 3: "object", 4: "objectlist", 5: "integer", 6: "boolean", 7: "integerlist"}


def fmt_of(etype):
    return FMT.get((etype >> 24) & 0xF, "?")


def gpt_partition_device(part_guid, disk_guid):
    """88-byte BCD device for a partition on a GPT disk (DeviceType 6, PartitionStyle 0), as bcdboot writes it."""
    return (b"\0" * 16 + struct.pack("<IIII", 6, 0, 0x48, 0) + uuid.UUID(part_guid).bytes_le + b"\0" * 8
            + uuid.UUID(disk_guid).bytes_le + b"\0" * 16)


def decode_device(raw):
    if len(raw) == 88 and struct.unpack_from("<I", raw, 16)[0] == 6 and struct.unpack_from("<I", raw, 48)[0] == 0:
        return {"kind": "gpt-partition", "partition": str(uuid.UUID(bytes_le=raw[32:48])),
                "disk": str(uuid.UUID(bytes_le=raw[56:72]))}
    return {"kind": f"other (type {struct.unpack_from('<I', raw, 16)[0] if len(raw) >= 20 else '?'})",
            "hex": raw.hex()}


def encode(etype, value):
    f = fmt_of(etype)
    if f == "device":
        return REG_BINARY, value
    if f in ("string", "object"):
        return REG_SZ, value
    if f == "objectlist":
        return REG_MULTI_SZ, list(value)
    if f == "integer":
        return REG_BINARY, struct.pack("<Q", value)
    if f == "boolean":
        return REG_BINARY, bytes([1 if value else 0])
    if f == "integerlist":
        return REG_BINARY, b"".join(struct.pack("<Q", v) for v in value)
    raise ValueError(f"element {etype:#x}: unknown format")


def decode(etype, t, raw):
    f = fmt_of(etype)
    if f == "device":
        return decode_device(raw)
    if t in (REG_SZ,):
        return raw.decode("utf-16-le").split("\0")[0]
    if t == REG_MULTI_SZ:
        return [s for s in raw.decode("utf-16-le").split("\0") if s]
    if f == "integer":
        return int.from_bytes(raw, "little")
    if f == "boolean":
        return bool(raw[0]) if raw else False
    if f == "integerlist":
        return [int.from_bytes(raw[i:i + 8], "little") for i in range(0, len(raw), 8)]
    return raw.hex()


def osloader_objects(o):
    """The objects of a minimal bcdboot-like store with one Windows entry, plus the PS5 settings.
    o: boot_part, win_part, disk_guid, loader_guid, locale, description, firmware_pci (bool)."""
    win_dev = gpt_partition_device(o["win_part"], o["disk_guid"])
    boot_dev = gpt_partition_device(o["boot_part"], o["disk_guid"])
    loader = {
        E["device"]: win_dev, E["path"]: "\\Windows\\system32\\winload.efi", E["description"]: o["description"],
        E["locale"]: o["locale"], E["inherit"]: [BOOTLOADERSETTINGS],
        # no WinRE / automatic repair loops: every restart means starting the PS5 Linux loader again
        E["recoveryenabled"]: False,
        E["isolatedcontext"]: True, E["allowedinmemorysettings"]: [0x15000075],
        E["osdevice"]: win_dev, E["systemroot"]: "\\Windows",
        E["nx"]: 0, E["bootmenupolicy"]: 1,
        E["bootstatuspolicy"]: 1,             # IgnoreAllFailures (as on the Toshiba and today's SSD)
        E["testsigning"]: True,               # our drivers are test-signed
        # no nocrashautoreboot (1.1): a stop error restarts by itself and the warm restart loops back into Windows
    }
    if o.get("firmware_pci", True):
        loader[E["usefirmwarepcisettings"]] = True
    return {
        BOOTMGR: (0x10100002, {
            E["device"]: boot_dev, E["path"]: "\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
            E["description"]: "Windows Boot Manager", E["locale"]: o["locale"], E["inherit"]: [GLOBALSETTINGS],
            E["default"]: o["loader_guid"], E["displayorder"]: [o["loader_guid"]], E["timeout"]: 0}),
        o["loader_guid"]: (0x10200003, loader),
        GLOBALSETTINGS: (0x20100000, {E["inherit"]: [DBGSETTINGS, EMSSETTINGS, BADMEMORY]}),
        DBGSETTINGS: (0x20100000, {E["debugtype"]: 4}),
        EMSSETTINGS: (0x20100000, {E["bootems"]: False}),
        BADMEMORY: (0x20100000, {}),
        BOOTLOADERSETTINGS: (0x20200003, {E["inherit"]: [GLOBALSETTINGS, HYPERVISORSETTINGS]}),
        HYPERVISORSETTINGS: (0x20200003, {E["hypervisorlaunchtype_f3"]: 0, E["hypervisor_f4"]: 1,
                                          E["hypervisor_f5"]: 0x1C200}),
    }


def build(container, out, objects):
    """container: a hive file to reuse as the empty shell (the image's own BCD-Template); everything in it is
    removed first. out: written as a copy, the container is not changed."""
    data = open(container, "rb").read()
    open(out, "wb").write(data)
    with Hive(out, write=True) as h:
        root = L.hivex_root(h.h)
        for name, c in children(h, root):
            if L.hivex_node_delete_child(h.h, c) != 0:
                raise RuntimeError(f"cannot clear {name}")
        h.set("Description", "KeyName", REG_SZ, "BCD00000000")
        h.set("Description", "System", REG_DWORD, 1)
        h.set("Description", "TreatAsSystem", REG_DWORD, 1)
        h.node("Objects", create=True)
        for guid, (otype, elems) in objects.items():
            h.set(f"Objects\\{guid}\\Description", "Type", REG_DWORD, otype)
            h.node(f"Objects\\{guid}\\Elements", create=True)
            for etype, value in elems.items():
                t, v = encode(etype, value)
                h.set(f"Objects\\{guid}\\Elements\\{etype:08x}", "Element", t, v)
        h.commit()


def read(path):
    """-> {guid: {"type": int, "elements": {etype: value}}}, plus "_description": {name: value}."""
    store = {"_description": {}}
    with Hive(path) as h:
        for keypath, vals in walk(h):
            parts = keypath.split("\\")
            if parts == ["Description"]:
                for n, t, raw in vals:
                    store["_description"][n] = (raw.decode("utf-16-le").split("\0")[0] if t == REG_SZ
                                                else int.from_bytes(raw[:4], "little"))
            elif len(parts) == 3 and parts[0] == "Objects" and parts[2] == "Description":
                for n, t, raw in vals:
                    if n == "Type":
                        store.setdefault(parts[1], {"elements": {}})["type"] = int.from_bytes(raw[:4], "little")
            elif len(parts) == 4 and parts[0] == "Objects" and parts[2] == "Elements":
                et = int(parts[3], 16)
                for n, t, raw in vals:
                    if n == "Element":
                        store.setdefault(parts[1], {"elements": {}})["elements"][et] = decode(et, t, raw)
    return store


def check(store):
    """Structural checks a boot manager relies on. Returns a list of problems (empty = fine)."""
    probs = []
    if store["_description"].get("KeyName") != "BCD00000000":
        probs.append("Description\\KeyName is not BCD00000000")
    objs = {k: v for k, v in store.items() if not k.startswith("_")}
    if BOOTMGR not in objs:
        return probs + ["no {bootmgr} object"]
    for guid, o in objs.items():
        if "type" not in o:
            probs.append(f"{guid}: no Description\\Type")
        for et, v in o["elements"].items():
            refs = v if isinstance(v, list) and fmt_of(et) == "objectlist" else [v] if fmt_of(et) == "object" else []
            for r in refs:
                if r.lower() not in objs:
                    probs.append(f"{guid} element {et:08x} points at missing object {r}")
    bm = objs[BOOTMGR]["elements"]
    d = bm.get(E["default"])
    if not d or d not in bm.get(E["displayorder"], []):
        probs.append("{bootmgr}: default missing or not in displayorder")
    elif d.lower() not in objs:
        probs.append("{bootmgr}: default points at a missing object")
    elif objs[d]["type"] != 0x10200003:
        probs.append("{bootmgr} default is not an OS loader")
    else:
        ld = objs[d]["elements"]
        for need in ("device", "osdevice", "path", "systemroot"):
            if E[need] not in ld:
                probs.append(f"OS loader: no {need}")
        if ld.get(E["device"]) != ld.get(E["osdevice"]):
            probs.append("OS loader: device and osdevice differ")
        bd = bm.get(E["device"], {})
        if isinstance(bd, dict) and isinstance(ld.get(E["device"]), dict) and bd.get("disk") != ld[E["device"]].get("disk"):
            probs.append("boot manager and Windows are on different disks")
    return probs
