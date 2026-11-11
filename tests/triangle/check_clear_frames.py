import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def check(executable: str, output_root: Path, configuration: str, mode: str,
          verifier: str | None = None) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=f"{mode}-frames-", dir=output_root) as directory:
        args = [executable, "--frames", "60", "--hidden", "--output", directory]
        if mode == "clear":
            args.append("--clear-only")
        image = Path(directory) / "frame.png"
        if mode == "screenshot":
            args.extend(["--screenshot", str(image)])
        run = subprocess.run(
            args,
            capture_output=True, text=True, timeout=40, check=False,
        )
        print(run.stdout, end="")
        print(run.stderr, end="", file=sys.stderr)
        if run.returncode != 0 or "presented_frames=60" not in run.stdout.splitlines():
            raise RuntimeError(f"expected 60 presented frames and exit 0, got exit {run.returncode}")

        log = next(path for path in Path(directory).iterdir() if path.is_dir())
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
        if ("rhi.presentation.triangle-pipeline" in ids) != (mode != "clear"):
            raise RuntimeError(f"unexpected graphics pipeline state for {mode}: {ids}")
        errors = [event for event in events if event["event_id"] == "rhi.validation.error"]
        if errors:
            raise RuntimeError(f"Vulkan Validation errors: {errors}")
        if mode == "screenshot":
            if not verifier or not image.is_file():
                raise RuntimeError("screenshot or PNG verifier missing")
            subprocess.run([verifier, str(image)], check=True, timeout=5)
        print("validation_errors=0")


if __name__ == "__main__":
    check(sys.argv[1], Path(sys.argv[2]), sys.argv[3], sys.argv[4], *sys.argv[5:])
