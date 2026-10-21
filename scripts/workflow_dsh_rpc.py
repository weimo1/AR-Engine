"""Bounded NDJSON client for an owned DSH SDK runtime; never attaches to GUI."""
from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time


class StartupFailure(ValueError):
    def __init__(self, stopped: bool):
        super().__init__('DSH startup failed; inspect owned runtime evidence')
        self.runtime_stopped = stopped


class Client:
    def __init__(self, command: list[str], cwd: Path, env: dict, directory: Path,
                 seconds: int, secret: str = ''):
        self.deadline = time.monotonic() + seconds
        self.secret = secret
        self.sequence = 0
        self.frames = queue.Queue(maxsize=256)
        self.error = None
        self.output = (directory / 'events.jsonl').open('w', encoding='utf-8', newline='\n')
        self.job = None
        try:
            self.stderr = (directory / 'stderr.log').open('w', encoding='utf-8', newline='\n')
            if os.name == 'nt':
                from workflow_dsh_job import Job
                self.job = Job()
            self.process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=0x08000004 if os.name == 'nt' else 0,
                start_new_session=os.name != 'nt')
            if self.job:
                self.job.bind_resume(self.process, directory)
        except BaseException as error:
            stopped = self.job.stop() if self.job else True
            if hasattr(self, 'process'):
                try:
                    if self.process.poll() is None:
                        self.process.kill()
                    self.process.wait(timeout=5)
                except (OSError, subprocess.TimeoutExpired):
                    stopped = False
            self.output.close()
            if hasattr(self, 'stderr'):
                self.stderr.close()
            if not stopped:
                raise StartupFailure(False) from error
            raise
        self.threads = [threading.Thread(target=self.read_stdout, daemon=True),
                        threading.Thread(target=self.read_stderr, daemon=True)]
        try:
            for thread in self.threads:
                thread.start()
        except BaseException as error:
            raise StartupFailure(self.stop()) from error

    def redact(self, text: str) -> str:
        return text.replace(self.secret, '[REDACTED]') if self.secret else text

    def read_stdout(self):
        try:
            size = 0
            for _ in range(50000):
                line = self.process.stdout.readline(2_000_001)
                if not line:
                    return
                size += len(line)
                if len(line) > 2_000_000 or size > 32_000_000 or not line.endswith(b'\n'):
                    raise ValueError('DSH protocol output exceeded bounds or was truncated')
                frame = json.loads(self.redact(line.decode('utf-8')))
                if not isinstance(frame, dict) or frame.get('jsonrpc') != '2.0':
                    raise ValueError('DSH stdout is not JSON-RPC 2.0')
                self.frames.put_nowait(frame)
            raise ValueError('DSH protocol event limit exceeded')
        except (ValueError, UnicodeError, queue.Full, OSError) as error:
            self.error = f'Protocol reader failed: {type(error).__name__}'

    def read_stderr(self):
        try:
            size = 0
            while True:
                line = self.process.stderr.readline(65536)
                if not line:
                    return
                size += len(line)
                if size > 2_000_000:
                    self.error = 'DSH stderr exceeded bound'
                    return
                self.stderr.write(self.redact(line.decode('utf-8', errors='replace')))
                self.stderr.flush()
        except (OSError, ValueError):
            self.error = 'DSH stderr reader failed'

    def call(self, method: str, params: dict, observer=None):
        self.sequence += 1
        identity = self.sequence
        frame = {'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': params}
        data = (json.dumps(frame, ensure_ascii=False) + '\n').encode('utf-8')
        # Input is bounded by task package export; never publish credentials in a request log.
        sent = threading.Event()
        failures = []
        def send():
            try:
                self.process.stdin.write(data)
                self.process.stdin.flush()
            except (OSError, ValueError) as error:
                failures.append(type(error).__name__)
            finally:
                sent.set()
        sender = threading.Thread(target=send, daemon=True)
        sender.start()
        if not sent.wait(max(0, self.deadline - time.monotonic())):
            raise ValueError('DSH request pipe deadline exceeded')
        if failures:
            raise ValueError('DSH request pipe failed: ' + failures[0])
        while True:
            if self.error:
                raise ValueError(self.error)
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise ValueError('DSH runtime deadline exceeded; in-flight cost may be unknown')
            try:
                item = self.frames.get(timeout=min(remaining, 0.2))
            except queue.Empty:
                if self.process.poll() is not None:
                    raise ValueError('DSH exited without the expected response')
                continue
            self.output.write(json.dumps(item, ensure_ascii=False) + '\n')
            self.output.flush()
            if 'id' in item:
                if type(item['id']) is not int or item['id'] != identity or ('result' in item) == ('error' in item):
                    raise ValueError('Unexpected DSH RPC response')
                if 'error' in item:
                    raise ValueError(f'DSH rejected {method}: {self.redact(str(item["error"]))[:1000]}')
                return item['result']
            if 'method' not in item:
                raise ValueError('Invalid DSH notification')
            if observer:
                observer(item)

    def stop(self) -> bool:
        """No process-name killing; own runtime only, wait before writer may hand off."""
        try:
            if self.process.poll() is None:
                self.deadline = time.monotonic() + 5
                try:
                    self.call('shutdown', {})
                    self.process.wait(timeout=3)
                except (ValueError, OSError, subprocess.TimeoutExpired):
                    if self.process.poll() is None:
                        if os.name == 'nt':
                            subprocess.run(['taskkill', '/PID', str(self.process.pid), '/T', '/F'],
                                capture_output=True, timeout=10, creationflags=0x08000000)
                        else:
                            import signal
                            os.killpg(self.process.pid, signal.SIGKILL)
                        self.process.wait(timeout=5)
            tree_stopped = self.job.stop() if self.job else True
            self.process.wait(timeout=5)
            return tree_stopped and self.process.poll() is not None
        except (OSError, subprocess.TimeoutExpired):
            return False
        finally:
            for thread in self.threads:
                if thread.ident is not None:
                    thread.join(timeout=1)
            # Keep streams open if a reader is still running rather than racing a write.
            if not any(thread.is_alive() for thread in self.threads):
                self.output.close()
                self.stderr.close()
                for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                    stream.close()
