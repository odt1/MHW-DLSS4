"""Reads the game's sampler table cache and the scene's sampler bank (MHW:I 15.23, D3D12).

cD3DRender = [renderer + 0x78] (renderer = [0x1451c4480]). Its sampler table cache [+0x1370] is made
once, in the device's constructor (0x1423a0950 -> 0x1423a48e0, capacity 0x800): +0 entries
({slot, crc} pairs), +8 tables, +0xc next free slot, +0x10 capacity. The lookup 0x142624870 (from
0x1423a9200, which the per-draw table builders call) appends a missing table at the next slot
without a bound check, and nothing frees slots while the game runs. The slots are descriptors in the
2048-entry shader-visible sampler heap (cD3DRender +0x13c8). D3D11 has no such cache.

The scene's mSamplerLodBias (scene + 0x1a8, scene = [0x1451c4368]) is the bank materials take when
they load (see HoldMipBias in src/game.cpp).

usage: samplerpeek.py                        one reading of the running game
       samplerpeek.py --watch                waits for the game, then prints every change from its start
       samplerpeek.py --watch --early-bank N also writes bank N into the scene as soon as it exists
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE


def find_pid():
    snap = k32.CreateToolhelp32Snapshot(2, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    pid = None
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == "monsterhunterworld.exe":
            pid = e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def creation_time(h):
    times = [wt.FILETIME() for _ in range(4)]
    k32.GetProcessTimes(h, *[ctypes.byref(t) for t in times])
    ft = times[0]
    return ((ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 1e7 - 11644473600.0


h = None


def read(addr, n):
    if not addr:
        return None
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return None
    return buf.raw


def q(addr):
    b = read(addr, 8)
    return struct.unpack("<Q", b)[0] if b else 0


def sample():
    renderer = q(0x1451c4480)
    device = q(renderer + 0x78) if renderer else 0
    cache = q(device + 0x1370) if device else 0
    c = read(cache + 8, 12) if cache else None
    tables, used, cap = struct.unpack("<III", c) if c else (None, None, None)
    scene = q(0x1451c4368)
    b = read(scene + 0x1a8, 4) if scene else None
    bank = struct.unpack("<I", b)[0] if b else None
    f = read(renderer + 0x1f208, 4) if renderer else None
    frame = struct.unpack("<I", f)[0] if f else None
    # The renderer's DLSS state (+0xa5 on, +0xb4 type, +0x204 applied render scale), which the plugin's
    # mip bias hold follows.
    d = read(renderer + 0xa5, 1) if renderer else None
    t = read(renderer + 0xb4, 4) if renderer else None
    sc = read(renderer + 0x204, 4) if renderer else None
    dlss = (d[0], struct.unpack("<I", t)[0], round(struct.unpack("<f", sc)[0], 3)) if d and t and sc else None
    return {"tables": tables, "slots": used, "capacity": cap, "bank": bank, "frame": frame, "dlss": dlss}


def main():
    global h
    watch = "--watch" in sys.argv
    pid = find_pid()
    while watch and not pid:
        time.sleep(0.05)
        pid = find_pid()
    if not pid:
        sys.exit("MonsterHunterWorld.exe is not running")
    early = int(sys.argv[sys.argv.index("--early-bank") + 1]) if "--early-bank" in sys.argv else None
    # VM_READ | QUERY_INFORMATION | QUERY_LIMITED_INFORMATION, plus VM_WRITE | VM_OPERATION for --early-bank
    h = k32.OpenProcess(0x1410 | (0x28 if early is not None else 0), False, pid)
    t0 = creation_time(h)
    if not watch:
        print(sample())
        return
    print(f"pid {pid}, started {time.time() - t0:.2f} s ago")
    last = last_s = None
    last_slots_print = 0.0
    while find_pid() == pid:
        s = sample()
        if early is not None and s["bank"] == 0:
            scene = q(0x1451c4368)
            got = ctypes.c_size_t()
            k32.WriteProcessMemory(h, ctypes.c_void_p(scene + 0x1a8), struct.pack("<I", early), 4, ctypes.byref(got))
            print(f"{time.time() - t0:8.2f} s  wrote bank {early}", flush=True)
            early = None
            continue
        key = (s["bank"], s["capacity"], s["tables"] is None, s["dlss"])
        now = time.time()
        # Every bank, DLSS state and cache appearance change; the slot count at most twice a second.
        if key != last or (s["slots"] != (last_s or {}).get("slots") and now - last_slots_print > 0.5):
            print(f"{now - t0:8.2f} s  frame {s['frame']}  dlss (on, type, scale) {s['dlss']}  bank {s['bank']}  "
                  f"slots {s['slots']}/{s['capacity']}  tables {s['tables']}", flush=True)
            last_slots_print = now
        last, last_s = key, s
        time.sleep(0.005)


if __name__ == "__main__":
    main()
