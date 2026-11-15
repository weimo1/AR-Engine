import json
from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import zlib


def make_mesh(root: Path, name: str, colors: bytes) -> str:
    folder = root / name
    folder.mkdir()
    quad = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
    uv = [(0, 1), (1, 1), (1, 0), (0, 0)]
    (folder / "quad.bin").write_bytes(
        b"".join(struct.pack("<3f", *v) for v in quad)
        + b"".join(struct.pack("<2f", *v) for v in uv)
        + struct.pack("<6H", 0, 1, 2, 0, 2, 3)
    )

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    (folder / "colors.png").write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"\0" + colors[:8] + b"\0" + colors[8:]))
        + chunk(b"IEND", b"")
    )
    model = {
        "asset": {"version": "2.0"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                                   "indices": 2, "material": 0}]}],
        "buffers": [{"uri": "quad.bin", "byteLength": 92}],
        "bufferViews": [{"buffer": 0, "byteOffset": start, "byteLength": length}
                        for start, length in [(0, 48), (48, 32), (80, 12)]],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": [-1, -1, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        "textures": [{"source": 0}], "images": [{"uri": "colors.png"}],
    }
    (folder / "quad.gltf").write_text(json.dumps(model), encoding="utf-8")
    return f"{name}/quad.gltf"


def run(executable: str, output_root: Path, config: str, verifier: str) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="static-scene-", dir=output_root) as directory:
        output = Path(directory)
        assets = output / "assets"
        assets.mkdir()
        mesh = make_mesh(assets, "active", bytes([
            255, 0, 0, 255, 0, 255, 0, 255,
            255, 255, 0, 255, 0, 0, 255, 255,
        ]))
        green = make_mesh(assets, "green", bytes([0, 255, 0, 255] * 4))
        yellow = make_mesh(assets, "yellow", bytes([255, 255, 0, 255] * 4))
        camera = {"position": [0, 0, 5], "orientation": [0, 0, 0, 1],
                  "fov_y": 0.785398163, "near": 0.1, "far": 100}

        def write_scene(path: Path, instances: list[tuple[int, float, str]]) -> None:
            objects = [
                {"id": i, "asset": asset, "position": [x, 0, 0],
                 "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}
                for i, x, asset in instances
            ]
            path.write_text(json.dumps({"version": 1, "camera": camera, "objects": objects}),
                            encoding="utf-8")

        active = output / "active.json"
        candidate = output / "candidate.json"
        write_scene(active, [(1, -1.38, mesh), (2, 1.38, mesh)])
        write_scene(candidate, [(3, 20, mesh), (4, -1.38, green), (5, 1.38, yellow)])
        logs = output / "logs"
        completed = subprocess.run(
            [executable, "--hidden", "--frames", "4", "--output", str(logs),
             "--static-scene", str(active), "--asset-root", str(assets),
             "--static-scene-test", str(candidate), "--screenshot", str(output / "after.png")],
            capture_output=True, text=True, timeout=40, check=False,
        )
        print(completed.stdout, end="")
        print(completed.stderr, end="", file=sys.stderr)
        if completed.returncode != 0 or "presented_frames=4" not in completed.stdout.splitlines():
            raise RuntimeError(f"static scene sample returned {completed.returncode} or missed frames")
        for name in ["before", "rollback", "reopened", "after"]:
            mode = "static-scene-retry" if name == "after" else "static-scene"
            subprocess.run([verifier, str(output / f"{name}.png"), mode], check=True, timeout=5)
        log = next(logs.iterdir())
        summary = json.loads((log / "summary.json").read_text(encoding="utf-8"))
        if (summary["configuration"] != config or summary["result"] != "success"
                or not summary["resources_released"] or not summary["diagnostics_complete"]):
            raise RuntimeError(f"incomplete static scene run: {summary}")
        events = [json.loads(line)
                  for line in (log / "events.jsonl").read_text(encoding="utf-8").splitlines()]
        ids = {event["event_id"] for event in events}
        if config == "Debug" and "rhi.validation.enabled" not in ids:
            raise RuntimeError("Debug must enable Vulkan Validation")
        if "rhi.validation.error" in ids:
            raise RuntimeError("static scene Vulkan Validation error")
        changes = [event["message"] for event in events
                   if event["event_id"] in {"rhi.static-scene.ready", "rhi.static-scene.rollback"}]
        if changes != ["draws=2", "injected failure after first candidate mesh upload",
                       "draws=2", "draws=3"]:
            raise RuntimeError(f"unexpected scene transaction order: {changes}")


if __name__ == "__main__":
    run(sys.argv[1], Path(sys.argv[2]), sys.argv[3], sys.argv[4])
