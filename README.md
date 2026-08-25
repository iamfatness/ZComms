# ZComms

Standalone intercom for live production, built on the Zoom Meeting SDK.
Channels, groups, push-to-talk, and — the part no incumbent offers — a native
Zoom leg, so a remote Zoom guest sits on the party line without virtual audio
cables or a spare machine.

**Status: design. No code yet.** Read [`docs/PLAN.md`](docs/PLAN.md) first — it
argues what the Meeting SDK will and will not let this product be, and gates
the engineering behind four cheap spikes. The decisive one is TX round-trip
latency measured against a real meeting; above roughly 250 ms one-way, the
Zoom-transport thesis is dead for live crew and the phasing changes.

## Shape

ZComms links the Zoom Meeting SDK directly. There is no helper process and no
IPC layer in Phase 1 — the app *is* the client. Multi-channel adds one small
worker per channel, because the SDK is a per-process singleton and a Zoom
meeting is a channel.

```
Phase 1   one process   → one meeting  = one channel
Phase 2   UI + N workers → N meetings  = N channels
Phase 3   + a native low-latency fabric, Zoom as one leg on it
```

## Before the first build

Two things gate shipping rather than coding, and both have lead time:

1. **A Zoom Marketplace app identity** — a General app with user-managed OAuth
   and Meeting SDK / Embed enabled, its own client id, public app key and
   redirect URI, plus a broker endpoint so no end user ever types app
   credentials. Start the review before Phase 1 code lands.
2. **The Meeting SDK itself** — not redistributable in a public repo, so
   `third_party/zoom-sdk/` is gitignored and CI fetches it at build time from a
   private release asset on this repository. Settle that step before the first
   CI run.

## Layout

```
docs/PLAN.md    the architecture, phasing and admin-model decision
```

Everything else arrives with Phase 1.
