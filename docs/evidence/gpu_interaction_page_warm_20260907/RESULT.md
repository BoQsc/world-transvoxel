# Interaction page warm checkpoint

Authority base: `3fc91f40041a204dcce0f366858b4147ca040ca7`

Interaction-focus leases now load and decode missing LOD0 source pages through
the reserved interaction storage lane without adding chunk demand or creating
application, collision, or visual publications.

The focused native fixture placed `{2,0,0,0}` outside the visible desired set.
Both debug and release reported one storage admission, one decoded completion,
zero rejections, and no `ChunkDemandAccepted` event for that key:

```text
FOREGROUND_RUNTIME_METRICS updates=3 active=2 support=1 focus=1 changed=1 matched=4 missing=2 warm_requests=2 warm_admissions=1 warm_completions=1 warm_rejections=0
```

Debug and release production streaming retained hash:

```text
39db05c67fc2f4b8d8beaab2e7da927ae968efb3d75118bcd80c5523116d9b3b
```

Debug and release LOD streaming retained hash:

```text
1a59569e2131a7aa07279004a8c2ce304278da658047c3aac8d56bb601ae87a3
```

This checkpoint removes cold page I/O and decode from a later focused edit when
the focus lease has settled. It does not prepublish collision or visual terrain;
balanced viewer demand remains authoritative for those branches.
