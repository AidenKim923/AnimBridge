"""Locate Blender and run blender/uab_blender.py with a job file."""
from __future__ import annotations

import glob
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

from . import process, runtime

PROJECT_ROOT = Path(os.environ.get("UAB_BACKEND_DIR") or Path(__file__).resolve().parents[2]).resolve()
BLENDER_SCRIPT = PROJECT_ROOT / "blender" / "uab_blender.py"
SRC_PATH = PROJECT_ROOT / "src"


class BlenderError(Exception):
    """`layer` is set (stage | job) when the failure was a timeout."""

    def __init__(self, message, layer=None, stage=None):
        super().__init__(message)
        self.layer, self.stage = layer, stage


def default_data_root() -> Path:
    """Writable user storage; backend resources may live in a read-only install."""
    return runtime.current_context().data_dir


def _blender_version(path) -> tuple:
    match = re.search(r"(\d+)\.(\d+)", str(path))
    return (int(match.group(1)), int(match.group(2))) if match else (0, 0)


def find_blender(explicit: str | None = None) -> Path:
    try:
        context = runtime.current_context()
    except runtime.RuntimeContextError as error:
        raise BlenderError(str(error)) from error
    try:
        if context.bundled:
            return runtime.bundled_blender(context, explicit)
        candidates = []
        if explicit:
            candidates.append(Path(explicit))
        else:
            if os.environ.get("UAB_BLENDER"):
                candidates.append(Path(os.environ["UAB_BLENDER"]))
            if context.manifest:
                candidates.append(context.component_path("blender", "executablePath"))
            candidates.append(PROJECT_ROOT.parent / "runtime" / "blender" / runtime.blender_binary_name(context.platform))
        for candidate in candidates:
            candidate = runtime.normalize_app_bundle(candidate, "Blender")
            if candidate.is_file():
                runtime.check_executable(candidate, context.platform)
                return candidate
        if explicit:
            raise BlenderError(f"Blender not found at {explicit}")
        # Discover installed tools only after all authoritative/private candidates
        # were checked. Discovery may touch unavailable drives or platform APIs.
        discovered = (sorted((Path(p) for p in glob.glob(r"C:\Program Files\Blender Foundation\Blender *\blender.exe")),
                             key=_blender_version, reverse=True) if context.platform == "windows"
                      else runtime.system_blender_candidates(context.platform))
        for candidate in discovered:
            candidate = runtime.normalize_app_bundle(candidate, "Blender")
            if candidate.is_file():
                runtime.check_executable(candidate, context.platform)
                return candidate
        raise BlenderError("Blender not found; pass --blender or set UAB_BLENDER")
    except (runtime.EngineError, runtime.RuntimeContextError) as error:
        raise BlenderError(str(error)) from error


def run_job(job: dict, blender: Path, timeout: float | None = None,
            log_dir: Path | None = None) -> dict:
    """Run one Blender job as a declared stage. `timeout` is the stage budget; the job deadline is separate."""
    context = runtime.current_context()
    log_dir = log_dir or runtime.scratch_dir() or (context.work_dir if os.environ.get("UAB_WORK_DIR") else None)
    if log_dir is not None:
        log_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="uab_", dir=log_dir))
    with process.stage("write-blender-job", timeout):
        job = dict(job, src_path=str(SRC_PATH), result=str(work / "result.json"))
        job_path = work / "job.json"
        job_path.write_text(json.dumps(job), encoding="utf-8")
    log_path = work / "blender.log"
    cmd = [str(blender), "-b", "--factory-startup", "--python-use-system-env", "--python-exit-code", "1",
           "--python", str(BLENDER_SCRIPT), "--", str(job_path)]
    try:
        with open(log_path, "w", encoding="utf-8", errors="replace") as log:
            proc = process.run_process(cmd, stage="blender", budget=timeout, env=runtime.blender_env(context, temp=work),
                                       stdout=log, stderr=subprocess.STDOUT)
    except process.ProcessTimeout as error:
        raise BlenderError(f"{error}; log: {log_path}", error.layer, error.stage) from error
    except (OSError, ValueError) as error:
        raise BlenderError(f"could not start Blender: {error}") from error
    result_path = Path(job["result"])
    if not result_path.is_file():
        raise BlenderError(f"Blender exited with code {proc.returncode} without a result; log: {log_path}")
    result = json.loads(result_path.read_text(encoding="utf-8"))
    result["log"] = str(log_path)
    return result
