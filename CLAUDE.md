# CLAUDE.md

Project notes for Claude Code sessions working in this repository: **ZComms**,
a standalone intercom built on the Zoom Meeting SDK. Update this file in the
same change as any substantive work — docs-updated is part of done.

## Where things stand

**Nothing is built.** This repo currently holds one document, `docs/PLAN.md`,
which is a pre-commit design memo written against CoreVideo v0.1.44. Do not
treat anything in it as implemented. Phase 0 (four spikes) has not run.

Before writing code, read `docs/PLAN.md` end to end. It exists specifically to
stop work starting in the wrong place, and its §9 spikes carry kill criteria
that can invalidate the architecture.

## The shared engine

ZComms does not have its own Zoom SDK integration. It shares CoreVideo's
engine — a separate process that links the Meeting SDK and moves media through
named shared memory. The shared surface is `engine/` plus five headers
(`engine-ipc.h`, `shm-generation.h`, `audio-timeline.h`,
`audio-silence-fade.h`, `media-event-queue.h`); see plan §3.1.

**The one rule that must not be broken:** there is exactly one audio-send
(`engine-talkback`) implementation, and it lives in the engine. A ZComms-local
copy of the send path is the specific failure the plan is most worried about —
a repo boundary makes that copy easier to justify, not harder. If ZComms needs
different engine behaviour, change the engine.

## Invariants inherited from CoreVideo

These were each paid for with a live-show defect. They apply to any code here
that touches the engine or its shared memory. The full history is in
CoreVideo's `CLAUDE.md` and in the comments on each header — read those before
changing anything in this area.

- **Audio ring** (`ShmAudioHeader`): 8 slots, per-slot seqlock, free-running
  uint32 indices. Readers drain fully on any wakeup.
- **Edge-triggered notify**: whoever consumes a wakeup owns the flag. Note the
  plan's deliberate departure — the **mic ring is pulled on a fixed 20 ms
  cadence and uses no notify flag at all** (plan §6.1), which removes this
  entire failure class in the app→engine direction.
- **Master clock** (`audio-timeline.h`): timestamps derive from samples, never
  arrival.
- **Silence-resume fade** (`audio-silence-fade.h`): the first buffer after a
  run of true-zero PCM is ramped in. A PTT release is exactly that transition,
  deliberately — a hard gate clicks.
- **Media dispatch lanes**: the pipe reader thread never runs media callbacks
  inline.
- **Process hygiene**: CoreVideo's engine launcher currently kills *every*
  `ZoomObsEngine.exe` on start. Until the owner-id namespacing lands in the
  CoreVideo repo (plan §3.7), a ZComms engine and an OBS-plugin engine on the
  same machine will terminate each other.

## Gotchas that are already known

- The Zoom SDK is fetched at build time from a **private release asset on the
  CoreVideo repo**. ZComms CI needs cross-repo read access or its own asset
  copy — the first CI run fails otherwise (plan §3.4).
- ZComms needs its **own Zoom Marketplace app identity**, baked at CMake
  configure time the way CoreVideo does it, plus its own OAuth broker route
  (plan §3.5).
- Shared names are hardcoded to the `ZoomObsPlugin_` prefix in CoreVideo.
  ZComms must not write regions under it (plan §3.7).
- Feeding raw PCM to the SDK's virtual mic **bypasses Zoom's echo
  cancellation**. An operator on speakers echoes into the meeting for
  everyone. This is ship-blocking, not polish (plan §2).

## Style

Follow CoreVideo's convention: comments state the constraint the code cannot
show, and when a change is motivated by a live failure, say what happened,
with numbers. Tests pin invariants, not implementations.
