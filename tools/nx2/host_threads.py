#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every host thread of a running or frozen emulator: name, CPU use and call stack.

Usage:  py -I tools/nx2/host_threads.py [<pid> | <process name>] [--seconds 1] [--out report.txt]

Without a target it reports every eglantine.exe and eden-cli.exe. Windows only. Nothing is
changed in the emulator: each thread is paused for the moment its stack is read, then resumed.
Function names and line numbers come from the .pdb next to the executable (MinGW Clang builds
write one, see MINGW_PDB); without it only DLL exports have names.

When the emulator freezes, run this before closing its window: the stacks show what every
thread is waiting on.
"""
import argparse
import ctypes
import os
import sys
import time
from collections import defaultdict
from ctypes import wintypes as wt

if sys.platform != "win32":
    sys.exit("host_threads.py is Windows only")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernelbase = ctypes.WinDLL("KernelBase", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)

DEFAULT_TARGETS = ("eglantine.exe", "eden-cli.exe")
MAX_FRAMES = 48
CONTEXT_SIZE = 1232  # sizeof(CONTEXT) on x64, 16-byte aligned
CONTEXT_FULL = 0x10000B
RSP_OFFSET, RBP_OFFSET, RIP_OFFSET = 0x98, 0xA0, 0xF8


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", wt.LONG), ("dwFlags", wt.DWORD),
                ("szExeFile", wt.WCHAR * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG),
                ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", wt.WORD), ("Mode", wt.DWORD)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64),
                ("FuncTableEntry", ctypes.c_void_p), ("Params", ctypes.c_uint64 * 4),
                ("Far", wt.BOOL), ("Virtual", wt.BOOL), ("Reserved", ctypes.c_uint64 * 3),
                ("KdHelp", ctypes.c_uint64 * 14)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG),
                ("Reserved", ctypes.c_uint64 * 2), ("Index", wt.ULONG), ("Size", wt.ULONG),
                ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG), ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64), ("Register", wt.ULONG), ("Scope", wt.ULONG),
                ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_char * 1024)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]


def _declare(function, restype, *argtypes):
    function.restype = restype
    function.argtypes = argtypes


HANDLE = ctypes.c_void_p
_declare(k32.CreateToolhelp32Snapshot, HANDLE, wt.DWORD, wt.DWORD)
_declare(k32.Process32FirstW, wt.BOOL, HANDLE, ctypes.POINTER(PROCESSENTRY32W))
_declare(k32.Process32NextW, wt.BOOL, HANDLE, ctypes.POINTER(PROCESSENTRY32W))
_declare(k32.Thread32First, wt.BOOL, HANDLE, ctypes.POINTER(THREADENTRY32))
_declare(k32.Thread32Next, wt.BOOL, HANDLE, ctypes.POINTER(THREADENTRY32))
_declare(k32.OpenProcess, HANDLE, wt.DWORD, wt.BOOL, wt.DWORD)
_declare(k32.OpenThread, HANDLE, wt.DWORD, wt.BOOL, wt.DWORD)
_declare(k32.CloseHandle, wt.BOOL, HANDLE)
_declare(k32.SuspendThread, wt.DWORD, HANDLE)
_declare(k32.ResumeThread, wt.DWORD, HANDLE)
_declare(k32.GetThreadContext, wt.BOOL, HANDLE, ctypes.c_void_p)
_declare(k32.GetThreadTimes, wt.BOOL, HANDLE, *[ctypes.POINTER(wt.FILETIME)] * 4)
_declare(k32.GetProcessHandleCount, wt.BOOL, HANDLE, ctypes.POINTER(wt.DWORD))
_declare(k32.QueryFullProcessImageNameW, wt.BOOL, HANDLE, wt.DWORD, wt.LPWSTR,
         ctypes.POINTER(wt.DWORD))
_declare(k32.K32GetProcessMemoryInfo, wt.BOOL, HANDLE,
         ctypes.POINTER(PROCESS_MEMORY_COUNTERS_EX), wt.DWORD)
_declare(k32.K32GetModuleFileNameExW, wt.DWORD, HANDLE, HANDLE, wt.LPWSTR, wt.DWORD)
_declare(k32.LocalFree, HANDLE, HANDLE)
_declare(kernelbase.GetThreadDescription, ctypes.c_long, HANDLE, ctypes.POINTER(ctypes.c_void_p))
_declare(dbghelp.SymSetOptions, wt.DWORD, wt.DWORD)
_declare(dbghelp.SymInitializeW, wt.BOOL, HANDLE, wt.LPCWSTR, wt.BOOL)
_declare(dbghelp.SymCleanup, wt.BOOL, HANDLE)
_declare(dbghelp.SymFromAddr, wt.BOOL, HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
         ctypes.POINTER(SYMBOL_INFO))
_declare(dbghelp.SymGetLineFromAddr64, wt.BOOL, HANDLE, ctypes.c_uint64,
         ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINE64))
_declare(dbghelp.SymGetModuleBase64, ctypes.c_uint64, HANDLE, ctypes.c_uint64)
_declare(dbghelp.SymFunctionTableAccess64, ctypes.c_void_p, HANDLE, ctypes.c_uint64)
_declare(dbghelp.StackWalk64, wt.BOOL, wt.DWORD, HANDLE, HANDLE, ctypes.POINTER(STACKFRAME64),
         ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)


def snapshot(flags, entry_type, first, following):
    handle = k32.CreateToolhelp32Snapshot(flags, 0)
    entry = entry_type()
    entry.dwSize = ctypes.sizeof(entry)
    ok = first(handle, ctypes.byref(entry))
    while ok:
        yield entry
        ok = following(handle, ctypes.byref(entry))
    k32.CloseHandle(handle)


def find_processes(target):
    """(pid, executable name) of every process the target names."""
    names = DEFAULT_TARGETS if target is None else (target.lower(),)
    found = []
    for entry in snapshot(0x2, PROCESSENTRY32W, k32.Process32FirstW, k32.Process32NextW):
        name = entry.szExeFile
        if target is not None and target.isdigit():
            if entry.th32ProcessID == int(target):
                found.append((entry.th32ProcessID, name))
        elif name.lower() in names or name.lower() + ".exe" in names or \
                name.lower() in tuple(n + ".exe" for n in names):
            found.append((entry.th32ProcessID, name))
    return found


def filetime_seconds(value):
    return ((value.dwHighDateTime << 32) | value.dwLowDateTime) / 1e7


class Thread:
    def __init__(self, thread_id, priority):
        self.id = thread_id
        self.priority = priority
        # THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION
        self.handle = k32.OpenThread(0x0002 | 0x0008 | 0x0040, False, thread_id)
        self.name = ""
        self.cpu = self.cpu_time()
        self.share = 0.0
        self.stack = ()
        if self.handle:
            text = ctypes.c_void_p()
            if kernelbase.GetThreadDescription(self.handle, ctypes.byref(text)) >= 0 and text:
                self.name = ctypes.wstring_at(text.value)
                k32.LocalFree(text)

    def cpu_time(self):
        times = [wt.FILETIME() for _ in range(4)]
        if not self.handle or not k32.GetThreadTimes(self.handle, *map(ctypes.byref, times)):
            return 0.0
        return filetime_seconds(times[2]) + filetime_seconds(times[3])


class Process:
    def __init__(self, pid, name):
        self.pid = pid
        self.name = name
        # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
        self.handle = k32.OpenProcess(0x0400 | 0x0010, False, pid)
        if not self.handle:
            raise OSError(f"cannot open process {pid}: error {ctypes.get_last_error()}")
        size = wt.DWORD(1024)
        path = ctypes.create_unicode_buffer(size.value)
        k32.QueryFullProcessImageNameW(self.handle, 0, path, ctypes.byref(size))
        self.path = path.value
        # SYMOPT_UNDNAME | DEFERRED_LOADS | LOAD_LINES | FAIL_CRITICAL_ERRORS | NO_PROMPTS
        dbghelp.SymSetOptions(0x2 | 0x4 | 0x10 | 0x200 | 0x80000)
        self.symbols = bool(dbghelp.SymInitializeW(self.handle, os.path.dirname(self.path), True))
        self.names = {}
        self.modules = {}

    def close(self):
        if self.symbols:
            dbghelp.SymCleanup(self.handle)
        k32.CloseHandle(self.handle)

    def threads(self):
        return [Thread(entry.th32ThreadID, entry.tpBasePri)
                for entry in snapshot(0x4, THREADENTRY32, k32.Thread32First, k32.Thread32Next)
                if entry.th32OwnerProcessID == self.pid]

    def summary(self, thread_count):
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(counters)
        k32.K32GetProcessMemoryInfo(self.handle, ctypes.byref(counters), counters.cb)
        handles = wt.DWORD()
        k32.GetProcessHandleCount(self.handle, ctypes.byref(handles))
        return (f"process {self.pid} {self.path}\n"
                f"  private {counters.PrivateUsage / 2**20:.0f} MiB, working set "
                f"{counters.WorkingSetSize / 2**20:.0f} MiB, peak working set "
                f"{counters.PeakWorkingSetSize / 2**20:.0f} MiB, {handles.value} handles, "
                f"{thread_count} threads, symbols {'loaded' if self.symbols else 'unavailable'}")

    def module(self, address):
        base = dbghelp.SymGetModuleBase64(self.handle, address)
        if not base:
            return None, 0
        if base not in self.modules:
            path = ctypes.create_unicode_buffer(1024)
            k32.K32GetModuleFileNameExW(self.handle, base, path, 1024)
            self.modules[base] = os.path.basename(path.value) or f"module@{base:#x}"
        return self.modules[base], base

    def frame_name(self, address):
        """'module!function+offset (file:line)' for a code address."""
        if address in self.names:
            return self.names[address]
        module, base = self.module(address)
        if module is None:
            text = f"{address:#x} (no module: JIT code or data)"
        else:
            symbol = SYMBOL_INFO()
            symbol.SizeOfStruct = 88
            symbol.MaxNameLen = 1023
            displacement = ctypes.c_uint64()
            if dbghelp.SymFromAddr(self.handle, address, ctypes.byref(displacement),
                                   ctypes.byref(symbol)):
                text = (f"{module}!{symbol.Name.decode('utf-8', 'replace')}"
                        f"+{displacement.value:#x}")
                line = IMAGEHLP_LINE64()
                line.SizeOfStruct = ctypes.sizeof(line)
                column = wt.DWORD()
                if dbghelp.SymGetLineFromAddr64(self.handle, address, ctypes.byref(column),
                                                ctypes.byref(line)):
                    file = line.FileName.decode("utf-8", "replace").replace("\\", "/")
                    text += f"  ({'/'.join(file.split('/')[-2:])}:{line.LineNumber})"
            else:
                text = f"{module}+{address - base:#x}"
        self.names[address] = text
        return text

    def walk(self, thread):
        """The thread's call stack, innermost frame first. The thread is paused meanwhile."""
        if not thread.handle or k32.SuspendThread(thread.handle) == 0xFFFFFFFF:
            return ("(cannot pause this thread)",)
        try:
            raw = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
            context = (ctypes.addressof(raw) + 15) & ~15
            ctypes.c_uint32.from_address(context + 0x30).value = CONTEXT_FULL
            if not k32.GetThreadContext(thread.handle, context):
                return ("(cannot read this thread's registers)",)
            frame = STACKFRAME64()
            for field, offset in (("AddrPC", RIP_OFFSET), ("AddrFrame", RBP_OFFSET),
                                  ("AddrStack", RSP_OFFSET)):
                address = getattr(frame, field)
                address.Offset = ctypes.c_uint64.from_address(context + offset).value
                address.Mode = 3  # AddrModeFlat
            addresses = []
            while len(addresses) < MAX_FRAMES and dbghelp.StackWalk64(
                    0x8664, self.handle, thread.handle, ctypes.byref(frame), context, None,
                    dbghelp.SymFunctionTableAccess64, dbghelp.SymGetModuleBase64, None):
                if frame.AddrPC.Offset == 0:
                    break
                addresses.append(frame.AddrPC.Offset)
        finally:
            k32.ResumeThread(thread.handle)
        return tuple(self.frame_name(address) for address in addresses)


def report(process, seconds):
    threads = process.threads()
    time.sleep(seconds)
    for thread in threads:
        thread.share = (thread.cpu_time() - thread.cpu) / seconds if seconds > 0 else 0.0
        thread.stack = process.walk(thread)
    lines = [process.summary(len(threads)), ""]
    # Pool threads parked in the same place are one entry.
    groups = defaultdict(list)
    for thread in threads:
        groups[(thread.name, thread.stack)].append(thread)
    ordered = sorted(groups.items(), key=lambda item: (-sum(t.share for t in item[1]), item[0][0]))
    for (name, stack), members in ordered:
        ids = ", ".join(str(thread.id) for thread in members[:6])
        if len(members) > 6:
            ids += f", ... ({len(members)} threads)"
        lines.append(f"== \"{name or '(unnamed)'}\"  thread {ids}  "
                     f"cpu {100 * sum(t.share for t in members):.1f}% of a core now, "
                     f"{sum(t.cpu_time() for t in members):.1f} s in total, "
                     f"priority {members[0].priority}")
        lines.extend(f"     {frame}" for frame in stack)
        lines.append("")
    for thread in threads:
        if thread.handle:
            k32.CloseHandle(thread.handle)
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("target", nargs="?", help="process id or executable name")
    parser.add_argument("--seconds", type=float, default=1.0,
                        help="interval the per-thread CPU use is measured over")
    parser.add_argument("--out", help="also write the report to this file")
    arguments = parser.parse_args()
    processes = find_processes(arguments.target)
    if not processes:
        sys.exit(f"no process matches {arguments.target or ' / '.join(DEFAULT_TARGETS)}")
    reports = []
    for pid, name in processes:
        process = Process(pid, name)
        try:
            reports.append(report(process, arguments.seconds))
        finally:
            process.close()
    text = "\n".join(reports)
    sys.stdout.reconfigure(errors="replace")
    print(text)
    if arguments.out:
        with open(arguments.out, "w", encoding="utf-8") as file:
            file.write(text)


if __name__ == "__main__":
    main()
