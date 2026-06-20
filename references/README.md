# References

`manifest.json` is the reviewed source list. `lock.json` records the exact local
downloads and revisions.

Run:

```console
python scripts/download_references.py
```

Downloaded papers and repository checkouts are stored under
`references/downloaded/` and intentionally ignored by Git. This keeps the
project from redistributing third-party publications or source repositories
while preserving a reproducible local research set.
