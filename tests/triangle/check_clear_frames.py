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
        if mode in {"screenshot", "set-binding"}:
            args.extend(["--screenshot", str(image)])
        if mode == "set-binding":
            args.append("--set-binding")
        if mode == "set-binding-retirement":
            args = [executable, "--frames", "3", "--hidden", "--output", directory,
                    "--set-binding-retirement"]
        run = subprocess.run(
            args,
            capture_output=True, text=True, timeout=40, check=False,
        )
        print(run.stdout, end="")
        print(run.stderr, end="", file=sys.stderr)
        frames = 3 if mode == "set-binding-retirement" else 60
        if run.returncode != 0 or f"presented_frames={frames}" not in run.stdout.splitlines():
            raise RuntimeError(f"expected {frames} presented frames and exit 0, got exit {run.returncode}")

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
        if mode in {"screenshot", "set-binding"}:
            if not verifier or not image.is_file():
                raise RuntimeError("screenshot or PNG verifier missing")
            verifier_args = [verifier, str(image)]
            if mode == "set-binding":
                verifier_args.append(mode)
            subprocess.run(verifier_args, check=True, timeout=5)
        if mode == "set-binding-retirement" and "rhi.binding.retirement.verified" not in ids:
            raise RuntimeError("missing binding retirement pixel verification")
        if mode == "set-binding-retirement":
            def position(event_id, fragment=""):
                return next(index for index, event in enumerate(events)
                            if event["event_id"] == event_id and fragment in event["message"])

            retired = position("rhi.binding.retirement.request")
            default = position("rhi.binding.publish", "before=stripes_view_1 after=default")
            registered = position("rhi.binding.register", "resource=blue_replacement_view")
            replacement = position("rhi.binding.publish", "before=default after=blue_replacement_view")
            verified = position("rhi.binding.retirement.verified")
            summary = position("rhi.binding.summary", "published=3 retired=2 reused=1 waits=")
            if not retired < default < registered < replacement < verified < summary:
                raise RuntimeError("unexpected binding retirement diagnostic order")
            waits = {}
            for index, event in enumerate(events):
                if event["event_id"] != "rhi.binding.wait":
                    continue
                fields = dict(item.split("=", 1) for item in event["message"].split())
                if not retired < index < default or fields["reason"] != "descriptor_update":
                    raise RuntimeError(f"unexpected binding wait: {fields}")
                waits.setdefault((fields["frame"], fields["submission"]), []).append(fields["phase"])
            if any(phases != ["begin", "complete"] for phases in waits.values()):
                raise RuntimeError(f"unpaired binding waits: {waits}")
            if int(events[summary]["message"].rsplit("=", 1)[1]) != len(waits):
                raise RuntimeError("binding wait count does not match events")
        print("validation_errors=0")


if __name__ == "__main__":
    check(sys.argv[1], Path(sys.argv[2]), sys.argv[3], sys.argv[4], *sys.argv[5:])
