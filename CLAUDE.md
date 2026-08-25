# CLAUDE.md

Project notes for Claude Code sessions working in this repository: **ZComms**,
a standalone intercom built on the Zoom Meeting SDK. Update this file in the
same change as any substantive work — docs-updated is part of done.

## Where things stand

**Nothing is built.** This repo holds one document, `docs/PLAN.md`, which is a
pre-commit design memo. Do not treat anything in it as implemented. Phase 0
(four spikes) has not run, and two of those spikes carry kill criteria that can
still invalidate the architecture.

Before writing code, read `docs/PLAN.md` end to end. It exists specifically to
stop work starting in the wrong place.

## What this project is not

ZComms is a **standalone product with no dependencies on any other codebase**.
It links the Zoom Meeting SDK directly and owns its whole stack: SDK
integration, audio engine, UI, sign-in, packaging.

In particular there is **no helper process and no IPC layer** in Phase 1. The
two-process designs common in this space exist because the media consumer is a
plugin inside somebody else's application; ZComms has no host, so it does not
pay that cost. Do not introduce pipes, shared memory or a wire protocol on the
assumption that this is how Zoom media apps are built — see plan §3.1.

Multi-channel (Phase 2) does add one worker process per channel, but only
because the SDK is a per-process singleton and a Zoom meeting is a channel.
Those workers are audio-only and deliberately narrow: mono 48 kHz PCM, one
stream each way, no video, no share, no recording.

## Design constraints that are load-bearing

- **Feeding raw PCM to the SDK's virtual mic bypasses Zoom's echo
  cancellation.** An operator on speakers echoes into the meeting for everyone.
  This is ship-blocking, not polish: mandate headsets with device detection, or
  carry an AEC. Plan §2.
- **`send()` is legal only between `onMicStartSend` and `onMicStopSend`.** Hold
  the sender pointer from `onMicInitialize` until `onMicUninitialized` and gate
  every call on the window. Plan §6.1.
- **The TX thread runs on a fixed 20 ms clock, not on events.** Zoom wants a
  steady stream, and a paced puller needs no wakeup protocol at all — which
  removes a whole class of consumed-wakeup bugs before it can exist. Underrun
  is a countable condition: send silence, increment a counter. Plan §6.1.
- **PTT edges ramp, they do not gate.** Zoom's one-way callback can carry true
  zero-valued PCM for hundreds of ms and jumping back to full amplitude is
  audible as a click; a PTT release is deliberately that same transition. Plan
  §5.
- **Timestamps derive from sample counts, never from callback arrival.** Reset
  only on re-subscribe, new session, or rate change. Plan §5.
- **Never run media callbacks inline on the thread handling control messages.**
  Under load that starves audio behind control traffic. Plan §5.
- **Zoom user ids are meeting-scoped and recycled.** Store participants by a
  stable identity, never by user id. Plan §5.
- **Every OS object carries the `ZComms` prefix**, and no binary here is ever
  named so another product's process cleanup could mistake it for its own.
  Plan §3.3.

## Known gates

- The Meeting SDK is not redistributable in a public repo:
  `third_party/zoom-sdk/` is gitignored and fetched at build time from a
  private release asset on this repository. The first CI run fails without it.
- ZComms needs its own Zoom Marketplace app identity, baked in at build time,
  with a broker endpoint so end users never enter app credentials. That is a
  review cycle with lead time — it gates shipping, not building.

## Style

Comments state the constraint the code cannot show. When a change is motivated
by a real failure, say what happened, with numbers. Tests pin invariants, not
implementations.
