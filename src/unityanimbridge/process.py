"""Child process runner with stage-lease notifications and distinct timeout layers.

Every internal piece of work (parsing, sampling, engine runs) is declared as a stage with its own
budget (default 600 s). A separate absolute job limit (default 3600 s, UAB_JOB_TIMEOUT_SECONDS)
covers one plan item or one helper request. Heartbeats prove liveness only; they never extend a
deadline. The supervisor timeout layer is enforced by the host, never by this worker.
"""
from __future__ import annotations

import contextlib
import functools
import json
import math
import os
import subprocess
import threading
import time

DEFAULT_STAGE_SECONDS = 600.0
DEFAULT_JOB_SECONDS = 3600.0
HEARTBEAT_SECONDS = 5.0
LAYER_STAGE, LAYER_JOB, LAYER_SUPERVISOR = "stage", "job", "supervisor"


class ProcessTimeout(TimeoutError):
    """`layer` says which budget ran out: stage (this stage), job (whole request) or supervisor (host)."""

    def __init__(self, message, layer, stage=None):
        super().__init__(message)
        self.layer, self.stage = layer, stage


class _StageState:
    def __init__(self, name, deadline):
        self.name, self.deadline, self.active = name, deadline, False


class _Lease:
    """JSONL channel to the host: UAB_STAGE_FD (Unix) or UAB_STAGE_HANDLE (Windows)."""

    def __init__(self):
        self._lock = threading.Lock()
        self._stream = None
        self._opened = False

    def _open(self):
        self._opened = True
        try:
            if os.name == "nt":
                handle = os.environ.get("UAB_STAGE_HANDLE")
                if handle:
                    import msvcrt
                    fd = msvcrt.open_osfhandle(int(handle), os.O_WRONLY)
                    self._stream = os.fdopen(fd, "w", encoding="utf-8", closefd=False)
            else:
                fd = os.environ.get("UAB_STAGE_FD")
                if fd:
                    self._stream = os.fdopen(int(fd), "w", encoding="utf-8", closefd=False)
            if self._stream is not None:
                os.set_inheritable(self._stream.fileno(), False)
        except (ValueError, OSError):
            self._stream = None

    def reset(self):
        with self._lock:
            self._stream, self._opened = None, False

    def send(self, event):
        with self._lock:
            if not self._opened:
                self._open()
            if self._stream is None:
                return
            try:
                self._stream.write(json.dumps(event, separators=(",", ":")) + "\n")
                self._stream.flush()
            except (OSError, ValueError):
                self._stream = None


_LEASE = _Lease()
_STATE = threading.local()


def reset_lease():
    _LEASE.reset()


def _positive_env(name, default):
    try:
        value = float(os.environ.get(name, default))
    except ValueError:
        return default
    return value if math.isfinite(value) and value > 0 else default


def _job_seconds():
    return _positive_env("UAB_JOB_TIMEOUT_SECONDS", DEFAULT_JOB_SECONDS)


def default_stage_seconds():
    """Stage budget the host asked for (UAB_STAGE_TIMEOUT_SECONDS), else 600 s."""
    host = _positive_env("UAB_STAGE_TIMEOUT_SECONDS", DEFAULT_STAGE_SECONDS)
    local = getattr(_STATE, "stage_seconds", None)
    return min(host, local) if local is not None else host


def host_deadline():
    """Absolute item deadline (host steady_clock ms, same monotonic basis), so a restart cannot reset it."""
    raw = os.environ.get("UAB_JOB_DEADLINE_MONOTONIC_MS")
    if not raw:
        return None
    try:
        value = float(raw) / 1000.0
    except ValueError as error:
        raise ValueError("The host item deadline is invalid") from error
    if not math.isfinite(value) or value <= 0:
        raise ValueError("The host item deadline is invalid")
    return value


@contextlib.contextmanager
def job(seconds=None, *, stage_seconds=None):
    """One plan item or one helper request: a single absolute deadline shared by all its stages."""
    previous = getattr(_STATE, "deadline", None)
    previous_stage = getattr(_STATE, "stage_seconds", None)
    if stage_seconds is not None:
        stage_seconds = float(stage_seconds)
        if not math.isfinite(stage_seconds) or stage_seconds <= 0:
            raise ValueError("A stage budget must be positive and finite")
    seconds = _job_seconds() if seconds is None else float(seconds)
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("A job budget must be positive and finite")
    deadline = time.monotonic() + seconds
    absolute = host_deadline()
    if absolute is not None:
        deadline = min(deadline, absolute)
    _STATE.deadline = min(deadline, previous) if previous else deadline
    _STATE.stage_seconds = min(stage_seconds, previous_stage) if stage_seconds is not None and previous_stage is not None else stage_seconds if stage_seconds is not None else previous_stage
    try:
        check_deadlines()
        yield
        check_deadlines()
    finally:
        _STATE.deadline = previous
        _STATE.stage_seconds = previous_stage


def job_remaining():
    deadlines = [d for d in (getattr(_STATE, "deadline", None), host_deadline()) if d is not None]
    return min(deadlines) - time.monotonic() if deadlines else None


@contextlib.contextmanager
def stage(name, budget=None):
    """Declare a unit of work. The host starts a fresh `budget` clock; `heartbeat()` never extends it.

    A nested stage pauses the outer lease (stage-end), runs, and resumes the outer one with whatever
    is left of its original deadline. An inner budget is clamped to the outer remainder and the job
    remainder, so nesting can never extend a deadline. Yields the effective budget.
    """
    budget = float(default_stage_seconds() if budget is None else budget)
    if not math.isfinite(budget) or budget <= 0:
        raise ValueError("A stage budget must be positive")
    check_deadlines()
    outer = getattr(_STATE, "stage", None)
    now = time.monotonic()
    limits = [budget]
    if "UAB_STAGE_TIMEOUT_SECONDS" in os.environ:
        limits.append(_positive_env("UAB_STAGE_TIMEOUT_SECONDS", DEFAULT_STAGE_SECONDS))
    if outer:
        limits.append(outer.deadline - now)
    remaining = job_remaining()
    if remaining is not None:
        limits.append(remaining)
    effective = min(limits)
    if effective <= 0:
        check_deadlines()
        raise ProcessTimeout("The stage has no remaining budget", LAYER_STAGE, name)
    if outer and outer.active:
        _LEASE.send({"event": "stage-end"})
        outer.active = False
    current = _StageState(name, now + effective)
    _STATE.stage = current
    _LEASE.send({"event": "stage-start", "name": name, "budgetSeconds": effective})
    current.active = True
    stopped = threading.Event()
    # CPU-only parsing/sampling also proves liveness; heartbeats do not change
    # either deadline. The outer heartbeat remains active during nested work.
    pulse = None
    if outer is None:
        def send_heartbeat():
            while not stopped.wait(HEARTBEAT_SECONDS):
                heartbeat()
        pulse = threading.Thread(target=send_heartbeat, name="AnimBridge stage heartbeat", daemon=True)
        pulse.start()
    try:
        yield effective
        check_deadlines()
    finally:
        if pulse is not None:
            stopped.set()
            pulse.join()
        if current.active:
            _LEASE.send({"event": "stage-end"})
            current.active = False
        _STATE.stage = outer
        remaining = job_remaining()
        if outer and outer.deadline > time.monotonic() and (remaining is None or remaining > 0):
            _LEASE.send({"event": "stage-start", "name": outer.name,
                         "budgetSeconds": outer.deadline - time.monotonic()})
            outer.active = True


def heartbeat():
    _LEASE.send({"event": "heartbeat"})


def staged(name):
    """Declare synchronous parsing/sampling work without changing its public API."""
    def decorate(function):
        @functools.wraps(function)
        def wrapped(*args, **kwargs):
            with stage(name):
                return function(*args, **kwargs)
        return wrapped
    return decorate


def check_deadlines():
    """Raise ProcessTimeout when the current stage or the job has already run out of time."""
    remaining = job_remaining()
    if remaining is not None and remaining <= 0:
        raise ProcessTimeout("The request exceeded its overall time limit", LAYER_JOB)
    current = getattr(_STATE, "stage", None)
    if current and current.deadline <= time.monotonic():
        raise ProcessTimeout(f"Stage '{current.name}' exceeded its time budget", LAYER_STAGE, current.name)


def _terminate(child):
    child.kill()
    try:
        child.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass


def run_process(command, *, stage="process", budget=None, env=None, cwd=None,
                stdout=None, stderr=None, stdin=subprocess.DEVNULL, check=False):
    """Run an absolute executable without a shell, under a declared stage.

    Control descriptors are never inherited (close_fds). The child is killed when the stage budget or
    the absolute job deadline elapses; the raised ProcessTimeout carries which layer expired.
    """
    command = [str(part) for part in command]
    if not os.path.isabs(command[0]):
        raise ValueError("The executable must be an absolute path")
    name = stage
    kwargs = {"stdin": stdin, "stdout": stdout, "stderr": stderr, "cwd": cwd, "env": env,
              "close_fds": True, "shell": False}
    if os.name == "nt":
        # CREATE_NO_WINDOW gives the child a hidden console that its descendants inherit, so
        # console-subsystem grandchildren never open a visible black window. The supervisor
        # classifies the system conhost.exe as a private job member.
        for key in ("stdin", "stdout", "stderr"):
            if kwargs[key] is None:
                kwargs[key] = subprocess.DEVNULL
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
    with globals()["stage"](name, budget) as effective:
        budget = effective
        started = time.monotonic()
        stage_deadline = _STATE.stage.deadline
        child = subprocess.Popen(command, **kwargs)
        last_beat = started
        try:
            while True:
                now = time.monotonic()
                remaining = job_remaining()
                if remaining is not None and remaining <= 0:
                    _terminate(child)
                    raise ProcessTimeout("The request exceeded its overall time limit", LAYER_JOB, name)
                if now >= stage_deadline:
                    _terminate(child)
                    raise ProcessTimeout(f"Stage '{name}' exceeded its {budget:g} s budget", LAYER_STAGE, name)
                wait = min(1.0, stage_deadline - now, remaining if remaining is not None else 1.0)
                try:
                    code = child.wait(timeout=max(wait, 0.01))
                    break
                except subprocess.TimeoutExpired:
                    if now - last_beat >= HEARTBEAT_SECONDS:
                        heartbeat()
                        last_beat = now
        except BaseException:
            if child.poll() is None:
                _terminate(child)
            raise
    result = subprocess.CompletedProcess(command, code)
    if check and code:
        raise subprocess.CalledProcessError(code, command)
    return result


def stage_budget(request, key="timeout", default=None):
    """Per-request stage budget from a legacy `timeout` field; the job deadline still bounds it."""
    default = default_stage_seconds() if default is None else default
    try:
        value = float(request.get(key, default)) if request else default
    except (TypeError, ValueError):
        value = default
    return value if value > 0 else default


def describe(error):
    if isinstance(error, ProcessTimeout):
        return {"timeoutLayer": error.layer, "stage": error.stage, "reason": str(error)}
    return {"reason": str(error)}
