# ZComms — standalone intercom on the Zoom Meeting SDK

Status: **design, pre-commit.** Nothing here is built yet. Written 2026-08-25
against CoreVideo v0.1.44. ZComms is a **separate product in its own repo**,
sharing CoreVideo's engine. The purpose of this document is to decide *whether*
and *in what order* to build it, and to name the four spikes that make that
decision cheaply.

## 1. The strategic read

A Zoom-transport intercom that competes with Unity Intercom head-on is a bad
bet. A **native intercom fabric with a first-class Zoom leg** is a good one,
and it is the only version of this product that Unity structurally cannot copy
without doing everything CoreVideo has already done.

The reason is in Zoom's model, not in our code:

- **Zoom gives one SDK client exactly one microphone into exactly one
  meeting.** `setExternalAudioSource()` installs a single virtual mic on a
  single meeting connection. Everyone in that meeting hears the same mix.
  Channel routing — the entire point of an intercom — does not exist inside a
  Zoom meeting. "Talk to Camera but not Talent" cannot be expressed.
- **Everything you say is heard by everyone in the meeting**, audience
  included. That is the opposite of talkback. The only pure-Zoom fix is a
  separate crew meeting, at which point the Zoom meeting *is* the channel.
- **Latency is Zoom's, not ours.** Zoom's audio path runs a jitter buffer we
  do not control. Unity's pitch is "it feels like a wire" at LAN latencies.
  We cannot beat, tune, or bypass Zoom's transport.

So the honest mapping is **one Zoom meeting == one channel**, implemented as
one engine process per channel. That is a real product for Zoom-centric
productions, and it ships fast because the engine already does 90% of it. It
is not, on its own, a Unity competitor.

The competitive wedge is the other direction: build the low-latency fabric for
crew-to-crew, and use the existing headless engine to put a **remote Zoom guest
on the party line natively** — RX through the raw-data path we already ship, TX
through the SDK's virtual mic. Today, getting a remote Zoom guest onto a Unity
party line means virtual audio cables, a spare machine, and a person who
understands mix-minus. We can make it a checkbox, for the customers we already
have.

## 2. What the SDK actually gives us

Confirmed present in the vendored SDK (`third_party/zoom-sdk/h/`):

| Direction | API | Notes |
| --- | --- | --- |
| RX per-participant | `IZoomSDKAudioRawDataDelegate::onOneWayAudioRawDataReceived` | Already shipping. `EngineAudio`, `ZoomAudioRouter`. |
| RX mixed | `onMixedAudioRawDataReceived` | Already shipping. Program feed for IFB. |
| **TX** | `IZoomSDKAudioRawDataHelper::setExternalAudioSource(IZoomSDKVirtualAudioMicEvent*)` | **Never called by this codebase.** The whole talkback feature hangs off it. |
| TX write | `IZoomSDKAudioRawDataSender::send(char*, len, sample_rate, channel)` | 16-bit PCM. Mono or stereo. 48 kHz supported. |

`IZoomSDKVirtualAudioMicEvent` is a four-callback lifecycle:
`onMicInitialize(pSender)` hands us the sender, `onMicStartSend()` /
`onMicStopSend()` bracket the window in which `send()` is legal, and
`onMicUninitialized()` revokes the pointer. We own the cadence — the SDK does
not pull from us.

What the SDK does **not** give us, and which each cost real work:

- **No echo cancellation on externally-supplied audio.** Feeding raw PCM to the
  virtual mic bypasses Zoom's AEC entirely. An operator on speakers will echo
  into the meeting for everyone. Either mandate headsets in the product (and
  detect/warn when the output device is not a headset), or carry an AEC
  (WebRTC APM or speexdsp) in the app. This is ship-blocking, not a polish item.
- **No private audio to one participant.** No IFB to a single guest inside a
  shared meeting.
- **No control over Zoom's mute policy.** A host who mutes all, or a meeting
  configured "participants cannot unmute", silences the talkback client with no
  fix on our side. Must be detected and surfaced, not silently swallowed.
- **No UI.** The SDK's raw-data mode launching silently is a *feature* here —
  we draw our own app — but it means every failure state is ours to surface.
  The plugin already learned this expensively: `join-watchdog.h`,
  `zoom-join-decision.h`, `awaiting_admission`. Reuse them; do not re-derive.

## 3. Repo layout and the shared engine

ZComms lives in its own repository. That is the right call for a separate
product — its own issues, releases, CI, versioning and branding, with no
explaining that the intercom is an OBS plugin add-on. It does put a repo
boundary through the middle of the one constraint this document cares about
most, so the boundary needs a deliberate answer rather than a default one.

### 3.1 What is actually shared

Not much, and it is already a clean seam. CoreVideo's engine is a separate
process talking a documented wire protocol, so the shared surface is:

- `engine/` — `main.cpp`, `engine-audio`, `engine-video`, `engine-share`,
  `engine-writer`. The process that links the Zoom SDK.
- The headers both sides must agree on byte-for-byte: `src/engine-ipc.h` (the
  wire format and every shared struct), `src/shm-generation.h`,
  `src/audio-timeline.h`, `src/audio-silence-fade.h`, `src/media-event-queue.h`.

Everything else in CoreVideo — the OBS source types, docks, tile compositor,
ISO recorder — is not shared and never will be.

### 3.2 How to share it, by phase

**Now, through Phase 1: pin CoreVideo as a submodule.** ZComms carries
CoreVideo at `third_party/corevideo` on a pinned commit and builds `engine/`
from that tree. No refactor of a shipping product, one source of truth for the
TX path, and it works on day one. This matters most precisely while the TX path
is new and changing daily: a ZComms engineer needs to change the engine and
test it from ZComms in a single step.

**End of Phase 1: extract `corevideo-core`.** Once the TX path stops moving,
lift `engine/` plus the five shared headers into a third repo that both
products consume. The extraction boundary already exists — it is
`engine-ipc.h` — which is why this is a move, not a redesign.

**Not yet: consuming a prebuilt engine binary** from a CoreVideo release. It is
the cleanest coupling and the right long-run option for a stable engine, but it
makes the edit-test loop cross a release boundary, which is the wrong trade
while the send path is being written.

### 3.3 The rule that has to survive the split

**Exactly one `engine-talkback` implementation**, living in the engine, in
whichever repo owns the engine at the time. A ZComms-local copy of the send
path is the single failure this document is most worried about, and a repo
boundary makes that copy easier to justify, not harder. If a ZComms change
needs the engine to behave differently, the change goes into the engine and
both products get it.

### 3.4 The SDK fetch breaks first

`third_party/zoom-sdk/` is gitignored and fetched at build time. CI pulls it
from a **release asset on the CoreVideo repo itself** —
`repos/${GITHUB_REPOSITORY}/releases/assets/...` in `.github/workflows/build.yml`,
served by the private `sdk-assets` release. A default `GITHUB_TOKEN` in the
ZComms repo cannot read another repository's release assets, so **the first
ZComms CI run fails at SDK fetch** unless one of these is in place first:

- a token or GitHub App installation on ZComms CI with read access to
  CoreVideo's releases, or
- a second copy of the SDK asset published to a private ZComms release.

Decide this before the first CI run rather than during it. Note also that the
SDK *headers* are committed (32 files under `third_party/zoom-sdk/h/`) while the
binaries and framework are fetched — so a submodule of CoreVideo gets ZComms
the headers for free and leaves only the binary fetch to solve.

### 3.5 A second Zoom Marketplace identity

CoreVideo's app identity is baked at CMake configure time from
`src/zoom-credentials.h.in` (`ZOOM_EMBED_OAUTH_CLIENT_ID`,
`ZOOM_EMBED_MEETING_SDK_PUBLIC_APP_KEY`) and brokered through the Cloudflare
Worker at `corevideo.iamfatness.us`. ZComms needs its own Marketplace app,
client id, public app key, redirect URI, and either its own broker route or its
own worker. That is a review cycle with its own lead time: start it *before*
Phase 1 code lands, not after.

### 3.6 The name needs a branding check

A Z-prefixed name on a Meeting SDK app invites the implied-affiliation
question, and Zoom reviews app naming and branding at Marketplace approval.
Raise `ZComms` explicitly during that review — fold it into the Spike D
conversation (§9), which is already scheduled with Zoom about this product's
licensing shape.

### 3.7 Region and pipe prefixes stop being CoreVideo's

Every shared name is hardcoded to `ZoomObsPlugin_` today — pipes
(`_P2E`/`_E2P`) and SHM regions (`_video`/`_audio`/`_share`). A second product
writing regions under the plugin's prefix is precisely the ghost-writer
condition that cost ~92% of audio on 2026-08-17. The owner-id namespacing in
§6.5 is already required for engine-vs-engine coexistence; make the prefix part
of that same change so ZComms engines are provably distinct from plugin
engines, rather than bolting a second scheme on later. **That change lands in
the CoreVideo repo**, and it is the one piece of this plan that is a
prerequisite there rather than here.

**Status (2026-08-25).** The testable core has landed in CoreVideo as
`src/engine-owner.h`, pinned by `CoreVideoEngineOwnerTest` — one owner id that
both namespaces the names and decides what the stale-engine sweep may kill.
Two of its properties matter to this repo directly: the default owner
reproduces every shipping CoreVideo name byte-for-byte (so an existing
plugin/engine pair keeps working), and **an untagged engine belongs to the
plugin** — a binary predating `--owner` is what a half-updated CoreVideo
install produces, so ZComms must never claim one or it terminates the user's
OBS session mid-show.

The *wiring* is still outstanding there and nothing in that header is reachable
from production code yet: `engine-ipc.h`'s name constants, the engine's
`--owner` argument, and replacing the sweep's image-name scan with a per-owner
pid record. That last one is the awkward part — reading another process's
command line on Windows needs `NtQueryInformationProcess` or WMI, neither of
which is a sound basis for a live-show guarantee, so the pid-to-owner mapping
should be recorded at launch instead. **ZComms cannot run alongside OBS until
that wiring lands.**

## 4. Reference architecture

```
┌─────────────────────────────────────────────┐
│  ZComms (Qt6 app, one per operator)         │
│  capture · PTT/latch · per-channel faders   │
│  monitor mix · AEC · presence               │
└───────┬──────────────────────┬──────────────┘
        │ mic SHM ring         │ TCP line-JSON (control API)
        │ + P2E/E2P pipes      │
┌───────▼──────────┐    ┌──────▼───────────────┐
│ ZoomObsEngine ×N │    │ Admin backend        │
│ one per channel  │    │ orgs · users · groups│
│ RX one-way audio │    │ channels · matrix    │
│ TX virtual mic   │    │ presence · audit log │
└───────┬──────────┘    └──────────────────────┘
        │ Zoom Meeting SDK
   ┌────▼─────┐  ┌──────────┐
   │ Meeting A│  │ Meeting B│   ← one meeting per channel
   └──────────┘  └──────────┘
```

Phase 3 adds a native transport alongside the Zoom leg; the app's mixer does
not care which leg a channel arrives on.

## 5. What we reuse from CoreVideo

This is the argument for sharing CoreVideo's engine rather than going greenfield. Every item
below is production code that has already survived a live show:

- **`src/engine-ipc.h`** — SHM ring, seqlock slots, free-running indices,
  edge-triggered notify. The mic ring is the same struct with the roles
  reversed.
- **`src/audio-timeline.h`** — sample-derived master clock, asymmetric drift
  clamp. Needed on every RX channel.
- **`src/audio-silence-fade.h`** — written because Zoom's one-way callback
  delivers true-zero PCM between utterances and the resume clicked. A PTT
  release is *exactly* that transition, deliberately. Applies on TX, unchanged.
- **`src/media-event-queue.h`** — the dispatch-lane split. The rule that the
  pipe reader thread never runs media callbacks inline holds identically here.
- **`src/shm-generation.h`**, `shm_region_name` — generation-suffixed regions.
- **`src/zoom-control-server.cpp`** — the TCP line-JSON control API. Talkback
  commands slot straight in, and the Companion module already speaks it, so a
  Stream Deck PTT button is nearly free.
- **`sidecar/`** — an existing standalone Qt6 app with a sidebar shell, control
  client, settings page and its own control server. ZComms is its own repo (see
  §3), but the sidecar is the closest working model for its shell, control
  client and release machinery — read it before designing the app layer.
- **`src/zoom-auth.*`, `src/zoom-oauth.*`, the Cloudflare OAuth broker** —
  sign-in already works with no user-entered credentials.

## 6. New components

### 6.1 Engine: `engine/src/engine-talkback.{h,cpp}`

`EngineTalkback : IZoomSDKVirtualAudioMicEvent`. Holds the sender pointer
between `onMicInitialize` and `onMicUninitialized`, and a `can_send` flag
between `onMicStartSend` and `onMicStopSend`.

**A fixed-cadence TX thread, not an event-driven one.** The app writes captured
PCM into the mic ring at capture cadence; a dedicated engine thread wakes every
20 ms, drains one frame, and calls `send()`. This is deliberate:

- Zoom wants a *steady* stream. A bot mic that streams continuously and goes
  quiet rather than stopping is the pattern already documented in the
  CHANGELOG, and `audio-silence-fade.h` exists because of it.
- A paced puller needs **no notify flag and no pipe events in the mic
  direction**, which removes the entire class of reader-wedge bugs that cost us
  the 2026-08-17 investigation. The notify protocol's hardest property — that
  whoever consumes a wakeup owns the flag — simply does not arise.
- Underrun is then a normal, countable condition: no frame ready at the tick,
  send silence, increment `mic_underrun`. Start with a 2–3 slot (~40–60 ms)
  prime so ordinary jitter never underruns.

PTT press and release ramp over `kAudioResumeFadeMs` using the existing fade
helper. A hard gate would click, and we have already diagnosed that click once.

### 6.2 Mic SHM ring (app → engine)

Same `ShmAudioHeader`, same slot layout, opposite direction:
`ZoomObsPlugin_<uuid>_mic`. The notify helpers are direction-agnostic, but per
5.1 the mic ring does not use them — the engine pulls on a clock. 48 kHz, mono,
16-bit, 20 ms slots.

### 6.3 Local audio I/O

Recommend **miniaudio** (single header, WASAPI + CoreAudio, no new heavy
dependency, matches the project's dependency posture) over Qt Multimedia, which
is convenient but adds latency we cannot afford on top of Zoom's.

Needs: input device selection, gain + limiter, sidetone control, per-channel
monitor mix, and AEC (see §2).

### 6.4 Control API additions

`talkback_status`, `talkback_channels`, `talkback_talk {channel, on}`,
`talkback_latch {channel, on}`, `talkback_listen {channel, level}`,
`talkback_all_call {on}`. Extend the Companion module's `buildActions()` — note
that dropdown choices are baked at build time, so a channel-list change must
re-run `setActionDefinitions(buildActions(this))`, and channels must be keyed
by stable id, not by Zoom user id (meeting-scoped, recycled).

### 6.5 Process namespacing — **required before anything else ships**

`terminate_stale_engine_processes()` in `src/zoom-engine-client.cpp` kills
*every* `ZoomObsEngine.exe` on launch. That behaviour is correct today and was
root-caused live on 2026-08-17 (a ghost engine poisoned the notify flag, ~92%
audio loss, no error anywhere). But the standalone app's entire premise is
running **alongside** OBS + CoreVideo on the same box, and one engine per
channel means several engines at once. As written, they will kill each other.

The fix is to scope both the pipe names and the stale-process scan by an owner
id: `ZoomObsPlugin_<owner>_P2E` / `_E2P`, engines tagged with their owner on the
command line, and the kill scan matching only its own tag. This is a change to
shipping plugin code with a live-defect history attached, so it needs its own
test pinning "engine A's launch does not kill engine B".

## 7. Admin portal and the group model

The routing matrix is the product. Everything else is plumbing.

### 7.1 Data model

| Entity | Key fields |
| --- | --- |
| `orgs` | name, plan, sso config |
| `users` | identity (reuse Zoom OAuth), display name, role |
| `endpoints` | an app install: platform, version, last_seen, device name |
| `channels` | name, colour, type (`party_line`/`iso`/`program`), transport (`zoom_meeting`/`native`), zoom ref |
| `groups` | named set of users — "Camera", "Talent", "Producers" |
| `matrix` | (user\|group) × channel × `talk` × `listen` × `latch_allowed` × `priority` |
| `presets` | a named snapshot of the matrix — "Rehearsal", "Show", "Post" |
| `presence` | realtime: online, in-channel, talking |
| `audit_log` | who talked to which channel, when — a genuine enterprise selling point |

**Groups get matrix rows, users get overrides.** Resolution is
group-rows-then-user-overrides, evaluated on the endpoint so a lost admin
connection never mutes a live show.

### 7.2 Admin UX

A grid: groups down the side, channels across the top, each cell cycling
none → listen → talk → both. Plus per-user override rows, plus **presets pushed
live**. Mid-show config changes are the scariest thing an intercom admin does,
so every push is versioned and each endpoint reports "config v14 applied" —
half-applied config must be visible, not inferred.

Beyond the matrix, the features operators will ask for on day one: **ALL CALL**
(priority talk to every channel, overriding listen levels), **IFB** (talent
hears program until a producer talks, ducking program under the voice),
**reply-to-last-talker**, and **call/flash** to get attention on a channel
someone is listening to but not watching.

Note the honest limit from §2: IFB to *one* guest inside a shared Zoom meeting
is impossible. Per-guest IFB requires either a meeting per destination or the
native fabric. Do not promise it on the Zoom-only phases.

### 7.3 Backend

Supabase is the right fit and is already in the toolchain: Postgres for the
model, RLS for org isolation, Realtime for presence and live preset pushes,
Edge Functions for the admin API. The endpoint holds a cached copy of its
resolved matrix and keeps working through a backend outage.

## 8. Phasing

Rough sizing, deliberately coarse.

**Phase 0 — Spikes (see §9).** ~1 week. Decides everything below.

**Phase 1 — Single-channel talkback, Zoom transport.** ~4–6 weeks.
ZComms v1: one engine, one meeting, PTT + latch, monitor mix, AEC,
device picker. Ships as "producer talks to remote guests", which is genuinely
useful on its own. Critically, **the OBS plugin's in-progress talkback must be
built on this same engine TX path** — one implementation, two front ends. If
the plugin work forks its own send path, we pay for it twice and diverge.

**Phase 2 — Multi-channel + admin.** ~8–12 weeks.
One engine per channel, the matrix, groups, presets, admin portal, presence,
audit log, Companion actions. This is the first releasable *intercom*.

**Phase 3 — Native fabric.** ~12+ weeks.
Own low-latency transport (Opus over a self-hosted SFU) for crew-to-crew, with
the Zoom leg as one endpoint on the fabric. This is where the Unity comparison
becomes fair, and where per-guest IFB becomes possible.

Sell Phase 1 and 2 to the existing CoreVideo user base first — people already
running Zoom-sourced shows in OBS are a warm list and exactly the customers for
whom the Zoom leg is the differentiator.

## 9. Spikes, with kill criteria

Run these before committing engineering to Phase 1. Each is cheap; together
they decide the architecture.

**Spike A — TX round-trip latency. 1–2 days. The decisive one.**
Call `setExternalAudioSource` from the existing engine, send a 1 kHz tone into
a real meeting, and measure the delay to a second Zoom client. Use the existing
technique of attaching to the SHM ring read-only from a third process to
timestamp both ends.
*Kill criterion: if one-way latency exceeds ~250 ms, the Zoom-transport
intercom thesis is dead for live crew use. Phase 1 still ships as
producer-to-guest talkback, but Phase 3 moves to the front of the queue.*

**Spike B — Mute policy and identity. 1 day.**
Can the SDK client unmute reliably? What happens under "mute all" and under
"participants cannot unmute"? How does the talkback client appear in a normal
client's participant list, and is that acceptable to put in front of a client's
audience?

**Spike C — N engines on one box. 2 days.**
Two engine processes, two meetings, simultaneously, with the owner-id
namespacing from §6.5 in place. Measure RAM and CPU per channel — this sets the
per-machine channel ceiling and therefore the pricing model.

**Spike D — Zoom ISV conversation. Calendar time, not engineering time.**
An intercom multiplies concurrent Meeting SDK sessions per customer by the
channel count. Get Zoom's position on that licensing shape **before** Phase 2,
not after. It can invalidate the per-channel-meeting architecture outright.

## 10. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Zoom latency unfixable | Cannot serve live crew intercom | Spike A first; Phase 3 native fabric |
| Zoom licensing blocks N sessions | Phase 2 architecture invalid | Spike D before Phase 2 |
| Engine mutual-kill | Standalone app unusable next to OBS | §6.5, with a test |
| No AEC on virtual mic | Echo into the client's meeting | Headset mandate + detection, or bundle an AEC |
| Unity's moat is trust, not features | Slow enterprise adoption | Lead with the Zoom leg, sell to existing users |
| ZComms Marketplace review slips | Phase 1 built, cannot ship | Start the second app identity before Phase 1 code |
| ZComms CI cannot fetch the Zoom SDK | First CI run fails | Cross-repo release read, or a second SDK asset (§3.4) |
| TX path forks across the repo boundary | Two send paths, divergent timing bugs | One `engine-talkback`, in the engine repo (§3.3) |
| `ZComms` name rejected at review | Rebrand after build | Raise the name in the Spike D conversation |
