# ZComms

Standalone intercom for live production, built on the Zoom Meeting SDK.
Channels, groups, push-to-talk, and — the part nobody else has — a native
Zoom leg, so a remote Zoom guest sits on the party line without virtual audio
cables or a spare machine.

**Status: design. No code yet.** Read [`docs/PLAN.md`](docs/PLAN.md) first —
it argues what the Meeting SDK will and will not let this product be, and
gates the engineering behind four cheap spikes.

## Relationship to CoreVideo

ZComms is its own product and its own repo, sharing the **engine** with
[CoreVideo](https://github.com/iamfatness/CoreVideo) — the headless process
that links the Zoom Meeting SDK and moves media over shared memory. The plugin
and the intercom are two front ends over one engine.

The rule that makes the split safe: **exactly one audio-send implementation**,
living in the engine. See [§3 of the plan](docs/PLAN.md) for how the shared
surface is consumed, and why it starts as a pinned submodule rather than an
extracted core repo.

## Before the first build

Two things must be settled or the first CI run fails:

1. **Zoom SDK access.** The SDK is fetched at build time from a private
   release asset on the CoreVideo repo. ZComms CI needs cross-repo read access
   to those releases, or its own copy of the asset. (Plan §3.4)
2. **A second Zoom Marketplace identity.** Own client id, public app key,
   redirect URI and broker route — a review cycle with lead time. (Plan §3.5)

## Layout

```
docs/PLAN.md    the architecture and phasing decision
```

Everything else arrives with Phase 1.
