"""Windows owned Job Object: suspended launch, descendants and kill-on-close."""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import time
import uuid


class Limits(C.Structure):
    _fields_ = [('per_process_time', C.c_int64), ('per_job_time', C.c_int64),
        ('flags', W.DWORD), ('min_ws', C.c_size_t), ('max_ws', C.c_size_t),
        ('active_limit', W.DWORD), ('affinity', C.c_size_t), ('priority', W.DWORD), ('scheduling', W.DWORD)]


class ExtendedLimits(C.Structure):
    _fields_ = [('basic', Limits), ('io', C.c_uint64 * 6),
        ('process_memory', C.c_size_t), ('job_memory', C.c_size_t),
        ('peak_process_memory', C.c_size_t), ('peak_job_memory', C.c_size_t)]


class Accounting(C.Structure):
    _fields_ = [('times', C.c_int64 * 4), ('faults', W.DWORD), ('total', W.DWORD),
                ('active', W.DWORD), ('terminated', W.DWORD)]


class ThreadEntry(C.Structure):
    _fields_ = [(name, W.DWORD) for name in ('size', 'count', 'tid', 'pid')] + [
        ('base_priority', W.LONG), ('delta_priority', W.LONG), ('flags', W.DWORD)]


def kernel():
    k = C.WinDLL('kernel32', use_last_error=True)
    signatures = {
        'CreateJobObjectW': ([C.c_void_p, W.LPCWSTR], W.HANDLE),
        'OpenJobObjectW': ([W.DWORD, W.BOOL, W.LPCWSTR], W.HANDLE),
        'SetInformationJobObject': ([W.HANDLE, C.c_int, C.c_void_p, W.DWORD], W.BOOL),
        'QueryInformationJobObject': ([W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.c_void_p], W.BOOL),
        'AssignProcessToJobObject': ([W.HANDLE, W.HANDLE], W.BOOL),
        'TerminateJobObject': ([W.HANDLE, W.UINT], W.BOOL),
        'CloseHandle': ([W.HANDLE], W.BOOL),
        'CreateToolhelp32Snapshot': ([W.DWORD, W.DWORD], W.HANDLE),
        'Thread32First': ([W.HANDLE, C.POINTER(ThreadEntry)], W.BOOL),
        'Thread32Next': ([W.HANDLE, C.POINTER(ThreadEntry)], W.BOOL),
        'OpenThread': ([W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
        'ResumeThread': ([W.HANDLE], W.DWORD)}
    for name, (arguments, result) in signatures.items():
        method = getattr(k, name)
        method.argtypes, method.restype = arguments, result
    return k


class Job:
    def __init__(self):
        self.k = kernel()
        self.name = 'Local\\AR-DSH-' + uuid.uuid4().hex
        self.handle = self.k.CreateJobObjectW(None, self.name)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.k.SetInformationJobObject(self.handle, 9, C.byref(limits), C.sizeof(limits)):
            self.k.CloseHandle(self.handle)
            raise C.WinError(C.get_last_error())

    def bind_resume(self, process, directory: Path):
        # Popen creates one suspended primary thread; no child code runs before job binding.
        if not self.k.AssignProcessToJobObject(self.handle, int(process._handle)):
            process.kill()
            process.wait(timeout=5)
            raise C.WinError(C.get_last_error())
        import workflow as core
        core.save(directory / 'ownership.json', {'job_name': self.name, 'pid': process.pid,
                                                'bound_before_resume': True})
        snapshot = self.k.CreateToolhelp32Snapshot(4, 0)
        if snapshot == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        try:
            entry = ThreadEntry()
            entry.size = C.sizeof(entry)
            more = self.k.Thread32First(snapshot, C.byref(entry))
            while more:
                if entry.pid == process.pid:
                    thread = self.k.OpenThread(2, False, entry.tid)
                    if not thread:
                        raise C.WinError(C.get_last_error())
                    try:
                        if self.k.ResumeThread(thread) == 0xFFFFFFFF:
                            raise C.WinError(C.get_last_error())
                        return
                    finally:
                        self.k.CloseHandle(thread)
                more = self.k.Thread32Next(snapshot, C.byref(entry))
            raise ValueError('Owned suspended primary thread not found')
        finally:
            self.k.CloseHandle(snapshot)

    def stop(self) -> bool:
        if not self.handle:
            return True
        result = stop_handle(self.k, self.handle)
        self.k.CloseHandle(self.handle)
        self.handle = None
        return result


def stop_handle(k, handle) -> bool:
    if not k.TerminateJobObject(handle, 1):
        return False
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        accounting = Accounting()
        if not k.QueryInformationJobObject(handle, 1, C.byref(accounting), C.sizeof(accounting), None):
            return False
        if accounting.active == 0:
            return True
        time.sleep(0.03)
    return False


def recover(directory: Path) -> bool:
    import workflow as core
    import re
    value = core.load(core.within(directory, directory / 'ownership.json'))
    if value.get('bound_before_resume') is not True or not re.fullmatch(r'Local\\AR-DSH-[0-9a-f]{32}', value.get('job_name', '')):
        raise ValueError('Missing owned Job binding evidence; cannot infer tree exit')
    k = kernel()
    handle = k.OpenJobObjectW(4 | 8, False, value['job_name'])  # QUERY | TERMINATE
    if not handle:
        # Kill-on-close job disappears only when no associated process remains.
        if C.get_last_error() == 2:
            return True
        raise C.WinError(C.get_last_error())
    try:
        return stop_handle(k, handle)
    finally:
        k.CloseHandle(handle)
