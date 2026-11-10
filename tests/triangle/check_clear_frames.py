import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def check(executable: str, output_root: Path, configuration: str) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="clear-frames-", dir=output_root) as directory:
        run = subprocess.run(
            [executable, "--frames", "60", "--hidden", "--output", directory],
            capture_output=True, text=True, timeout=40, check=False,
        )
        print(run.stdout, end="")
        print(run.stderr, end="", file=sys.stderr)
        if run.returncode != 0 or "presented_frames=60" not in run.stdout.splitlines():
            raise RuntimeError(f"expected 60 presented frames and exit 0, got exit {run.returncode}")

        log = next(Path(directory).iterdir())
        summary = json.loads((log / "summary.json").read_text(encoding="utf-8"))
        if (
            summary["result"] != "success"
            or summary["configuration"] != configuration
            or not summary["diagnostics_complete"]
            or not summary["resources_released"]
        ):
            raise RuntimeError(f"incomplete sample run: {summary}")

        events = [
            json.loads(line)
            for line in (log / "events.jsonl").read_text(encoding="utf-8").splitlines()
        ]
        ids = {event["event_id"] for event in events}
        if not {"rhi.presentation.ready", "rhi.close.complete"} <= ids:
            raise RuntimeError(f"missing presentation or cleanup diagnostics: {ids}")
        if configuration == "Debug" and "rhi.validation.enabled" not in ids:
            raise RuntimeError("Debug did not enable Vulkan Validation")
        errors = [event for event in events if event["event_id"] == "rhi.validation.error"]
        if errors:
            raise RuntimeError(f"Vulkan Validation errors: {errors}")
        print("validation_errors=0")


if __name__ == "__main__":
    check(sys.argv[1], Path(sys.argv[2]), sys.argv[3])
