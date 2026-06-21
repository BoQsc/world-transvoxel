# M4 Storage Tooling Contract

Status: normative M4 component

Project-owned storage tooling uses a Python entry point for cross-platform
orchestration and a native executable for authoritative parsing, hashing,
journal validation, and migration.

```console
python tools/wt_storage.py inspect <path>
python tools/wt_storage.py validate <path>
python tools/wt_storage.py migrate-world <input.wtworld> <output.wtworld>
```

The wrapper selects the built release tool first and falls back to the debug
tool. Project orchestration remains cross-platform Python.

## Inspect and validate

The native tool recognizes one `wtworld` container, one `wtchunk` container,
or a concatenated append-only `wtedit` journal.

It executes the same native readers used by the addon. Journal inspection
discovers each exact segment, validates every atomic transaction, then loads
the complete sequence through the bounded journal ordering rules.

Successful output is one JSON object containing stable type-specific fields
and the complete-file SHA-256. Validation uses the same path and exit status;
it does not provide a weaker checksum-only mode.

Unknown magic, truncation, invalid schemas, directory errors, hash mismatch,
page metadata errors, transaction corruption, and journal revision/identity
errors return a nonzero status.

The tool bounds one input file to 1 GiB. Individual common containers retain
their stricter 256 MiB format limit.

## World migration

`migrate-world`:

1. validates the complete input through the current world reader;
2. reconstructs its typed manifest;
3. writes the current canonical schema;
4. writes to a sibling temporary path;
5. renames only after the complete output closes successfully;
6. validates and prints the migrated result.

The command refuses to overwrite either the requested output or its temporary
path. A failed write or rename removes only the tool-owned temporary file.

Schema 1.0 worlds migrate to schema 1.1 with world revision zero. Current
schema inputs canonicalize through the same writer.

Chunk and edit schemas have no older supported version yet, so no migration
command is exposed for them.

## Evidence

`scripts/test_m4_tools.py` generates independent binary fixtures in a
temporary directory and runs both debug and release tools. It proves:

- legacy world inspection;
- chunk inspection and validation;
- corrupted chunk rejection;
- schema 1.0 to 1.1 migration;
- byte-identical debug/release migration output;
- output overwrite refusal;
- Python wrapper invocation.

No test artifacts or archives remain in the repository.

The deterministic command-line/editor baker, codec decision, and milestone
evidence subsequently completed the M4 storage workflow.
