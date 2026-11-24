"""Deterministically generate binary geometry and PNG textures for the showroom scene.

The glTF documents, scene.json, and LICENSE.txt live as plain UTF-8 text in
examples/triangle/content/showroom/. This script generates the four binary assets
(unit_box.bin, unit_sphere.bin, white.png, floor_grid.png) into the CMake build
deployment directory so no opaque binary blobs need to be committed to Git.
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path


def _pack_mesh(
    positions: list[tuple[float, float, float]],
    uvs: list[tuple[float, float]],
    normals: list[tuple[float, float, float]],
    indices: list[int],
) -> bytes:
    assert len(positions) == len(uvs) == len(normals)
    assert len(indices) % 3 == 0
    buf = bytearray()
    for x, y, z in positions:
        buf.extend(struct.pack("<3f", x, y, z))
    for u, v in uvs:
        buf.extend(struct.pack("<2f", u, v))
    for nx, ny, nz in normals:
        length = math.sqrt(nx * nx + ny * ny + nz * nz)
        assert abs(length - 1.0) < 1e-5
        buf.extend(struct.pack("<3f", nx, ny, nz))
    for idx in indices:
        buf.extend(struct.pack("<H", idx))
    return bytes(buf)


def build_unit_box() -> bytes:
    """1x1x1 cube centered at the origin, 24 vertices, 12 outward CCW triangles (840 bytes)."""
    faces = [
        # (normal, four CCW corners looking from outside)
        ((0.0, 0.0, 1.0), ((-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5))),
        ((0.0, 0.0, -1.0), ((0.5, -0.5, -0.5), (-0.5, -0.5, -0.5), (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5))),
        ((1.0, 0.0, 0.0), ((0.5, -0.5, 0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5))),
        ((-1.0, 0.0, 0.0), ((-0.5, -0.5, -0.5), (-0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (-0.5, 0.5, -0.5))),
        ((0.0, 1.0, 0.0), ((-0.5, 0.5, 0.5), (0.5, 0.5, 0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5))),
        ((0.0, -1.0, 0.0), ((-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5))),
    ]
    quad_uvs = ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0))
    positions: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    normals: list[tuple[float, float, float]] = []
    indices: list[int] = []
    for normal, corners in faces:
        base = len(positions)
        for corner, uv in zip(corners, quad_uvs):
            positions.append(corner)
            uvs.append(uv)
            normals.append(normal)
        indices.extend((base, base + 1, base + 2, base, base + 2, base + 3))
    data = _pack_mesh(positions, uvs, normals, indices)
    assert len(data) == 840
    return data


def build_unit_sphere() -> bytes:
    """Diameter-1 UV sphere (24x16), 425 vertices, 720 outward CCW triangles (17920 bytes)."""
    slices = 24
    stacks = 16
    radius = 0.5
    positions: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    normals: list[tuple[float, float, float]] = []
    for stack in range(stacks + 1):
        v = stack / stacks
        phi = math.pi * v
        sin_phi = math.sin(phi)
        cos_phi = math.cos(phi)
        for sl in range(slices + 1):
            u = sl / slices
            theta = 2.0 * math.pi * u
            nx = sin_phi * math.cos(theta)
            ny = cos_phi
            nz = sin_phi * math.sin(theta)
            positions.append((radius * nx, radius * ny, radius * nz))
            uvs.append((u, v))
            normals.append((nx, ny, nz))

    ring = slices + 1
    indices: list[int] = []
    for stack in range(stacks):
        for sl in range(slices):
            a = stack * ring + sl
            b = a + 1
            c = a + ring
            d = c + 1
            # Winding chosen so cross(p1 - p0, p2 - p0) aligns with outward normal.
            if stack != 0:
                indices.extend((a, b, c))
            if stack != stacks - 1:
                indices.extend((b, d, c))
    data = _pack_mesh(positions, uvs, normals, indices)
    assert len(data) == 17920
    return data


def _encode_png_rgba(width: int, height: int, rows: list[bytes]) -> bytes:
    assert len(rows) == height
    raw = bytearray()
    for row in rows:
        assert len(row) == width * 4
        raw.append(0)  # Filter type 0 (None)
        raw.extend(row)

    def chunk(chunk_type: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(chunk_type + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + chunk_type + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw), level=9)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def build_white_png() -> bytes:
    return _encode_png_rgba(1, 1, [bytes((255, 255, 255, 255))])


def build_floor_grid_png() -> bytes:
    """64x64 subtle architectural checkerboard tile with crisp tile seams."""
    size = 64
    half = size // 2
    rows: list[bytes] = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            border = x in (0, half, size - 1) or y in (0, half, size - 1)
            if border:
                pixel = (182, 186, 192, 255)
            elif ((x // half) + (y // half)) % 2 == 0:
                pixel = (224, 226, 230, 255)
            else:
                pixel = (206, 210, 216, 255)
            row.extend(pixel)
        rows.append(bytes(row))
    return _encode_png_rgba(size, size, rows)


def generate_all(output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    artifacts = {
        "unit_box.bin": build_unit_box(),
        "unit_sphere.bin": build_unit_sphere(),
        "white.png": build_white_png(),
        "floor_grid.png": build_floor_grid_png(),
    }
    for name, payload in artifacts.items():
        (output_dir / name).write_bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True, help="Target assets directory")
    args = parser.parse_args()
    generate_all(args.output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
