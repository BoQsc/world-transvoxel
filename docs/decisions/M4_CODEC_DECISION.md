# M4 Storage Codec Decision

Status: accepted

Decision: schema 1 admits only codec `none`. No compressed codec is selected
for M4.

## Reason

The authoritative `wtchunk` payload is an interleaved sequence of finite
float32 density and uint16 material records. The required baseline already
provides:

- deterministic bytes;
- direct range reads;
- zero-copy section views;
- no decode allocation;
- no third-party codec dependency;
- identical debug/release output.

A dependency-free byte-run-length candidate was implemented as an isolated
benchmark, not production code. On locked 41,154-byte 19-cubed payloads its
deterministic stored-size ratios are:

| Fixture | RLE / `none` |
| --- | ---: |
| smooth density/material regions | 1.7972 |
| deterministic noisy density/material | 2.9867 |
| flat repeated sample records | 2.0001 |

Byte RLE expands all three because repetition exists at record or field level,
not as long consecutive byte runs in the interleaved payload.

`tools/benchmark_m4_codec.py` also reports local copy, encode, and decode
throughput. Throughput values are informational and hardware-dependent; the
gate is deterministic round-trip plus the locked size result.

## Why another codec is not selected now

Adding a library codec without representative streaming measurements would
create a dependency and format commitment before M5 defines real I/O,
decompression, cache, and random-access budgets. Compression ratio alone is
not sufficient because page latency, worker cost, memory pressure, seek
granularity, and unsupported-platform behavior matter.

The schema already has an explicit codec field. A later optional codec can be
added behind a new known feature/codec value without changing authoritative
sample semantics. `none` remains mandatory for every reader and writer.

## Revisit condition

M5 may evaluate maintained codecs such as block-oriented LZ or entropy
compression only when:

- representative baked worlds exist;
- random page load and cache workloads are measured;
- deterministic cross-platform bytes are tested;
- decode memory and worker latency are bounded;
- unsupported builds retain `none`;
- complete end-to-end behavior beats the baseline.

This resolves the M4 controlled decision without pretending an unmeasured
compressed path is production-ready.
