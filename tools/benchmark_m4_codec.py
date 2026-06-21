#!/usr/bin/env python3

from __future__ import annotations

import json
import struct
import time


def page_payload(kind: str) -> bytes:
    output = bytearray()
    state = 1
    for z in range(19):
        for y in range(19):
            for x in range(19):
                if kind == "smooth":
                    density = float(x + y + z - 27)
                    material = (x // 4 + y // 4 + z // 4) % 8
                elif kind == "flat":
                    density = -1.0
                    material = 1
                else:
                    state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
                    density = ((state >> 8) & 0xFFFF) / 257.0 - 127.5
                    material = state & 0xFFFF
                output.extend(struct.pack("<fH", density, material))
    return bytes(output)


def rle_encode(data: bytes) -> bytes:
    output = bytearray()
    index = 0
    while index < len(data):
        end = index + 1
        while end < len(data) and data[end] == data[index] and end - index < 65535:
            end += 1
        output.extend(struct.pack("<H", end - index))
        output.append(data[index])
        index = end
    return bytes(output)


def rle_decode(data: bytes) -> bytes:
    output = bytearray()
    index = 0
    while index < len(data):
        if index + 3 > len(data):
            raise RuntimeError("Truncated RLE record.")
        count = struct.unpack_from("<H", data, index)[0]
        value = data[index + 2]
        if count == 0:
            raise RuntimeError("Zero RLE count.")
        output.extend(bytes([value]) * count)
        index += 3
    return bytes(output)


def throughput(operation, value: bytes, iterations: int = 100) -> float:
    start = time.perf_counter()
    for _ in range(iterations):
        operation(value)
    elapsed = time.perf_counter() - start
    return (len(value) * iterations) / elapsed / (1024 * 1024)


def main() -> None:
    results: dict[str, dict[str, float | int]] = {}
    for kind in ("smooth", "noise", "flat"):
        payload = page_payload(kind)
        encoded = rle_encode(payload)
        if encoded != rle_encode(payload) or rle_decode(encoded) != payload:
            raise RuntimeError(f"RLE determinism or round trip failed for {kind}.")
        results[kind] = {
            "none_bytes": len(payload),
            "rle_bytes": len(encoded),
            "rle_ratio": len(encoded) / len(payload),
            "none_copy_mib_s": throughput(bytes, payload),
            "rle_encode_input_mib_s": throughput(rle_encode, payload),
            "rle_decode_output_mib_s": throughput(rle_decode, encoded)
            * len(payload)
            / len(encoded),
        }
    if results["smooth"]["rle_ratio"] <= 1.0:
        raise RuntimeError("RLE unexpectedly improved the locked smooth fixture.")
    if results["noise"]["rle_ratio"] <= 1.0:
        raise RuntimeError("RLE unexpectedly improved the locked noise fixture.")
    print(json.dumps(results, sort_keys=True))
    print("M4_CODEC_BENCH_PASS codec=none rejected_candidate=byte_rle")


if __name__ == "__main__":
    main()
