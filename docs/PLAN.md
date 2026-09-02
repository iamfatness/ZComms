# ZComms — standalone intercom on the Zoom Meeting SDK

> **Status 2026-09-02: Phase 1 is shipped — v0.1.14 is the current release**
> (installer on GitHub Releases). This document is kept for the reasoning;
> the living state — architecture as built, invariants, and the live-found
> platform laws — is in the repo's `CLAUDE.md`, which is the source of truth
> when the two disagree. Phase 1 status is marked through §8. Still
> open: code signing, ZComms' own Marketplace identity, Spikes B–D
> remainders, Phase 2 (party lines and the admin portal, none of §7
> is built), macOS.

Status: **design, post-Spike-A.** Written 2026-08-25; reworked 2026-08-26
after Spike A ran against a live meeting and its result reshaped the
architecture (§9 carries the numbers). ZComms is a **standalone product**: its
own repo, its own Zoom Marketplace identity, its own codebase, no dependency
on any other product. The purpose of this document is to decide *what* to
build and *in what order*, and to track the spikes that de-risk it.

The one-sentence version of the rework: **Zoom now has channel routing inside
a single meeting** — the talkback controller — and it was measured working at
165 ms median. That kills this document's original central premise, and most
of what follows is simpler than it was.

## 1. The strategic read

A Zoom-transport intercom that competes with Unity Intercom head-on is still
a bad bet — 165 ms is not "feels like a wire", and §9's numbers say Zoom's
floor is Zoom's, not ours to lower. But the shape of what *is* buildable
changed underneath this document, and changed in our favour.

The original premise here was that channel routing — the entire point of an
intercom — does not exist inside a Zoom meeting: one virtual mic, one meeting,
everyone hears the same mix, and the only pure-Zoom fix was a separate crew
meeting per channel, one engine process per meeting. **That premise is dead.**
Zoom's talkback controller (`IMeetingTalkbackController`, Meeting SDK ≥ 7.0,
the API behind ZoomISO's talkback) provides private audio channels *inside a
single meeting*: up to 16 of them, up to 10 listeners each, audio delivered
over each listener's ordinary Zoom connection with the main meeting duckable
underneath, inaudible to everyone else. "Talk to Camera but not Talent" is
now one API call to a channel Camera is in and Talent is not.

Spike A measured this path live (§9): **median 165 ms, p95 194 ms** one-way
into a plain Zoom client, on an ordinary account, under public-app-key auth,
with co-host role as the only gate. That is inside the ~250 ms bar for the
product this now is:

**ZComms joins the client's own meeting as one co-host participant and becomes
its talkback panel.** A director talks to all panelists, one panelist, or any
named subset — from a Stream Deck, a control surface, or a key — without the
audience hearing a word, and without the talent installing anything. Today
that job is done with ZoomISO plus an operator who understands it, or not at
all. The product is the checkbox version.

What survives from the original read:

- **Latency is Zoom's, not ours.** 165 ms is IFB-class, not wire-class. Cue a
  guest, brief a panel, yes; a camera operator tracking a fast director wants
  better. The native fabric (Phase 3) remains the answer for wire-class
  crew-to-crew, with the Zoom leg as one endpoint on it — and it remains the
  thing an incumbent cannot copy without rebuilding a Zoom stack.
- **The virtual mic still matters, demoted.** `setExternalAudioSource()` is
  the party-line-to-everyone case — audio into the meeting's main mix. It is
  no longer the product's core TX path, and §2 records the auth wall in front
  of it.
- **The wedge stays the same customers.** Productions already running remote
  guests over Zoom, for whom the alternative is virtual audio cables, a spare
  machine, and a person who understands mix-minus.

## 2. What the SDK actually gives us

Confirmed in the vendored SDK (`third_party/zoom-sdk/h/`, 7.1.5), and — where
marked — **live-verified by Spike A** rather than read off a header:

| Path | API | Status |
| --- | --- | --- |
| **TX per-channel** | `IMeetingTalkbackController::SendAudioDataToChannel(channelID, pcm, len, rate, ch)` | **Live-verified.** 16-bit PCM, 48 kHz mono, paced at 20 ms. The product's core TX path. **MONO ONLY: `ZoomSDKAudioChannel_Stereo` returns `SDKERR_SUCCESS` and delivers nothing audible** (CoreVideo, live). The header's "mono or stereo" claim lies; a stereo source must be downmixed before the SDK boundary. |
| Channel lifecycle | `CreateChannel(count)` / batch invite / batch remove / `SetChannelBackgroundVolume` | Live-verified. Max **16 channels**, max **10 listeners per channel**, all mutations asynchronous with per-item response callbacks. |
| Gates | `IsMeetingSupportTalkBack()`, co-host role | Live-verified: supported on an ordinary account; a guest gets `SDKERR_NO_PERMISSION` (12) and creation works seconds after co-host promotion. No entitlement beyond role was needed. |
| TX to everyone | `IZoomSDKAudioRawDataHelper::setExternalAudioSource(...)` | Header-verified only. **Its send window never opened under public-app-key auth** (`HasRawdataLicense()` false); expect it to require JWT auth. The party-line case, when it matters. |
| RX per-participant | `IZoomSDKAudioRawDataDelegate::onOneWayAudioRawDataReceived` | Header-verified. Expect recording-privilege friction. |
| RX mixed | `onMixedAudioRawDataReceived` | Header-verified. Program feed for IFB. |

Talkback delivery facts that shape the product, all observed live:

- **A plain Zoom client hears channel audio with nothing installed.** The
  product premise. The listener is invited by user id; their join is confirmed
  by callback; no acceptance step was observed on the receiving client.
- **`SetChannelBackgroundVolume` is real ducking**, delivered by Zoom: the
  main meeting lowers under the channel voice for channel members only. And
  **Zoom ducks channel members BY DEFAULT** — merely being placed in a channel
  reduces their meeting volume; talent notices on assignment (CoreVideo, live,
  same production). It is a channel-scoped 0.0–2.0 gain (1.0 = unity), so one
  call covers late joiners. Product policy (`DuckPlanner`): unity the moment a
  channel is ready, duck only while that channel is actually keyed.
- **The receiving client renders talkback to the default-communications
  endpoint**, not necessarily its configured speaker device. Harmless on
  ordinary machines (they are the same device); on multi-bus interfaces it
  puts talkback somewhere unexpected. A support-doc item, and the reason the
  spike harness grew a tap-every-endpoint mode.
- **The stream is voice-activity treated.** Send succeeds regardless, but
  near-silence is not delivered continuously; do not design anything that
  depends on a sub-audible keepalive reaching the far end.

What the SDK does **not** give us, and which each cost real work:

- **No echo cancellation on externally-supplied audio.** Raw PCM into either
  TX path bypasses Zoom's AEC. An operator on speakers echoes their monitor
  back into the channel. Mandate headsets with device detection, or carry an
  AEC (WebRTC APM or speexdsp). Ship-blocking, not polish.
- **Channel limits are product limits.** 16 channels; **10 listeners per
  channel** — a party line for a 25-person crew cannot be one talkback
  channel. The matrix (§7) must treat channel capacity as a first-class
  constraint, and the all-hands case belongs to the virtual mic or the native
  fabric.
- **Mute policy quirks remain, in a new costume.** A host's "ask to unmute"
  reaches an SDK client as a consent request (`onHostRequestStartAudio`) that
  something must `Accept()` — silently ignoring it looks exactly like a dead
  mic. Live-verified the expensive way.
- **No UI.** Every failure state is ours to surface. The waiting room, the
  co-host prompt, the meeting ending underneath us — Spike A hit each one, and
  each needs a loud, named surface in the product.

## 3. Standalone from the ground up

ZComms depends on nothing but the Zoom Meeting SDK. That is a decision with
real consequences in both directions, and the biggest one is a simplification
rather than a cost.

### 3.1 No host process means no IPC layer

The common prior art for "a desktop app that consumes Zoom raw media" is a
two-process design: a headless helper that links the SDK, talking to the app
over pipes and shared memory. That architecture exists wherever the consumer is
a *plugin* inside somebody else's application — the SDK cannot safely live in
the host's process, so media has to cross a boundary.

ZComms has no host. It is its own executable, so it links the Meeting SDK
**directly**. No pipes, no shared-memory rings, no seqlocks, no wire format,
no generation-suffixed region names, no notify protocol. The entire IPC layer
and its whole failure surface simply do not exist here, and every millisecond
that layer costs is not spent. In a product whose thesis is latency, that is
not a tidiness argument.

### 3.2 One SDK client now carries every channel

The talkback pivot deletes the multi-process problem for the core product.
Channels live *inside* one meeting, so **one SDK client — one process — serves
all 16 of them.** The worker-per-channel model this section previously
described existed only because a channel used to require its own meeting; it
no longer describes Phase 1 or Phase 2.

The Meeting SDK is still a process-level singleton (live-observed: `InitSDK`
returns `SDKERR_OTHER_SDK_INSTANCE_RUNNING` while *any other application's*
SDK engine is running — a stronger claim than per-process, and one with a
product consequence: ZComms and other SDK apps, including CoreVideo, cannot
run on the same machine simultaneously until proven otherwise). That matters
now only for the cases that genuinely span meetings:

- an operator serving **two different clients' meetings at once**, and
- the Phase 3 fabric bridging a Zoom leg per meeting.

Both are worker-per-*meeting*, not worker-per-channel, and both are deferred
until a customer actually asks. Spike C (§9) is re-scoped accordingly: its
urgent question is no longer the per-channel cost model but the
per-*machine* exclusivity of the SDK.

### 3.3 Naming is ours from day one

Every OS object ZComms creates — process names, any local sockets, any shared
sections — carries a `ZComms` prefix and belongs to this product alone.

This is worth stating explicitly because the failure it avoids is real and
expensive: two products writing OS objects under one name, where one silently
corrupts the other's state with no error surfaced on either side. Independence
removes that by construction, and it also means no other product's
process-cleanup logic can reach ZComms — provided nothing here is ever named to
look like something else's binary.

### 3.4 What independence costs

Being unrelated means building, not inheriting — though two items on this
list stopped being future work when the Spike A harness was deliberately
built as the seed rather than as throwaway:

- ~~The SDK integration~~ — auth, join, meeting lifecycle, the talkback
  channel lifecycle and both TX paths exist and ran against a live meeting
  (`spikes/a-tx-latency/`, promoted pieces in `src/audio`). Raw-audio RX
  remains unbuilt.
- ~~The audio engine~~ — capture, gain, look-ahead limiter, PTT ramps, the
  paced TX thread and its clock: built, unit-tested, verified on hardware.
- ~~OAuth sign-in and token storage~~ — built: browser PKCE through the
  CoreVideo broker, ZAK joins, DPAPI at rest.
- ~~The app shell, the control surface, packaging~~ — built: a WebView2
  panel in a Windows-subsystem exe, an HTTP/SSE control surface, a per-user
  NSIS installer. **Code signing and an updater remain.**

What remains is weeks, not the original weeks-more. §8 reflects the new
sizing.

### 3.5 The Zoom SDK is ZComms' to obtain

ZComms downloads and vendors its own Meeting SDK against its own Marketplace
credentials. The SDK is not redistributable in a public repo, so CI fetches it
at build time from a private release asset **on this repository**, and
`third_party/zoom-sdk/` stays gitignored. Settle the asset and the fetch step
before the first CI run rather than during it.

### 3.6 Its own Zoom Marketplace identity

ZComms needs a Marketplace app of its own: a General app with user-managed
OAuth, Meeting SDK / Embed enabled, its own client id and public app key, its
own redirect URI, and a broker endpoint so no end user ever types app
credentials. Bake the identity in at build time so a stale local config cannot
change the published app's identity.

That is a review cycle with its own lead time, and it gates shipping rather
than building. **Start it before Phase 1 code lands.**

### 3.7 The name needs a branding check

A Z-prefixed name on a Meeting SDK app invites the implied-affiliation
question, and Zoom reviews app naming and branding as part of Marketplace
approval. Raise `ZComms` explicitly during that review — fold it into the
Spike D conversation (§9), which is already scheduled with Zoom about this
product's licensing shape.

## 4. Reference architecture

One process, one meeting, all channels — Phase 1 *and* Phase 2:

```
┌──────────────────────────────────────────────────┐
│  ZComms (single process, one per operator)       │
│  ┌────────────────────────────────────────────┐  │
│  │ UI · PTT/latch · matrix · monitor mix      │  │
│  ├────────────────────────────────────────────┤  │
│  │ audio engine · capture · AEC · limiter     │  │
│  │ (built: src/audio, verified on hardware)   │  │
│  ├────────────────────────────────────────────┤  │
│  │ Zoom Meeting SDK (linked in-process)       │  │
│  │  TX SendAudioDataToChannel per channel     │  │
│  │  RX one-way / mixed raw audio              │  │
│  └────────────────────────────────────────────┘  │
└──────────────────────┬───────────────────────────┘
                       │ joins as co-host
              ┌────────▼─────────────────────────┐
              │  The client's meeting            │
              │  ch1: All Talent   (≤10 members) │
              │  ch2: Camera                     │
              │  ch3: Producers    … up to 16    │
              └──────────────────────────────────┘
```

The channel *is* a talkback channel; the matrix (§7) is channel membership
plus per-channel talk/listen state. No pipes, no workers, no second process
anywhere in this picture.

Worker-per-meeting returns only for multi-meeting operation (two clients'
shows at once) and for Phase 3, where the native fabric carries crew-to-crew
and a Zoom leg per meeting hangs off it; the mixer does not care which leg a
channel arrives on.

## 5. Prior art worth not rediscovering

Nothing here is a dependency — ZComms shares no code with anything. These are
lessons from previous production work against this same SDK, each of which was
paid for with a live failure, and each of which a greenfield Zoom audio app
will otherwise meet on its own. They are cheap to design in and expensive to
retrofit.

- **Derive timestamps from samples, never from arrival time.** Callback arrival
  jitters; sample counts do not. Reset the clock only on re-subscribe, a new
  session, or a rate change. Clamp drift asymmetrically — a forward jump and a
  backward burst are not the same event and must not share a threshold.
- **Zoom's one-way audio callback can deliver true-zero PCM for hundreds of
  milliseconds.** It is not a dropped callback; it fires on schedule carrying
  silence. Jumping straight back to full amplitude after such a run is audible
  as a click. Ramp the first buffer in. **A PTT release is deliberately that
  same transition**, which is why this matters more here than it did there.
- **Never run media callbacks inline on the thread that reads control
  messages.** Under real load that starves audio behind control traffic — a
  measured multi-hundred-millisecond stall. Separate lanes: latest-wins for
  anything frame-shaped, drain-fully for anything ring-shaped.
- **A drained-fully reader plus an edge-triggered wakeup is a bug factory.**
  Whoever consumes a wakeup owns the flag until the queue is provably empty,
  and any early return that keeps the flag silences the stream until the next
  keepalive. §6.2 avoids the whole class by pulling on a clock instead — worth
  understanding *why* before deciding you need events.
- **Zoom user ids are meeting-scoped and recycled.** Store participants by a
  stable identity, never by user id: a control surface holding an id points at
  nobody after a rejoin and at the wrong person once ids get reused. Doubly
  load-bearing now: **talkback channel membership is keyed by user id**, so a
  rejoin means re-inviting, and the matrix must heal membership on every
  participant change rather than assume it.

Spike A added its own entries to this list, each paid for live:

- **Every talkback mutation is asynchronous with a response callback.** Treat
  the callback as the truth and the call as a request. A design that assumes
  `CreateChannel` returning success means a channel exists will race.
- **A host's "ask to unmute" is a consent request, not an unmute.** Something
  must implement `onHostRequestStartAudio` and `Accept()`, or the client sits
  muted while the operator clicks unmute repeatedly and concludes the app is
  broken.
- **Never trust a configured audio device name; verify where audio actually
  renders.** The far client played talkback to its default-communications
  endpoint while its settings named a different device. Any feature that taps
  or monitors an endpoint needs a verify-by-signal step, not a name match.
- **The SDK's process exclusivity is machine-wide in practice.** Another
  application's SDK engine blocks `InitSDK` outright (error 14). Detect it,
  name the conflicting process, and say so — the raw error reads as a broken
  install.

## 6. Components

### 6.1 The channel sender — `ZoomTalkbackSource`, with `ZoomMicSource` behind it

Both TX paths already exist, built and live-exercised by the Spike A harness,
behind one seam: `FrameSink`. The paced TX thread does not know which Zoom
door the audio leaves through — or that it is Zoom at all (`WavSink` is the
third implementation and is how the engine is verified without a meeting).

**`ZoomTalkbackSource`** (the default): implements
`IMeetingTalkbackCtrlEvent`, owns the channel lifecycle — create, batch
invite, membership tracking from join/leave callbacks, background-volume
ducking — and sends via `SendAudioDataToChannel`. Its send window opens only
once the channel exists *and* has a confirmed listener; audio into an empty
channel is counted, not pretended. The product generalises this from the
spike's one channel to the matrix's N, with membership healed on every
participant change (§5, recycled user ids).

**`ZoomMicSource`** (the party-line case): the four-callback
`IZoomSDKVirtualAudioMicEvent` lifecycle, sender pointer held between
`onMicInitialize` and `onMicUninitialized`, every send gated on the
start/stop window. Kept, but behind the auth caveat in §2.

**A fixed-cadence TX thread, not an event-driven one.** A dedicated thread
wakes every 20 ms on an absolute grid, takes one frame from the capture queue
and sends. Deliberate on three counts:

- Zoom wants a *steady* stream, on both TX paths.
- A paced puller needs **no wakeup protocol at all**, which removes an entire
  class of consumed-wakeup bugs (§5) before it can exist.
- Underrun becomes a normal, countable condition rather than a silence bug: no
  frame ready at the tick, send silence, increment the counter. Prime 2–3
  frames so ordinary jitter never underruns.

Measured on this machine: 6007/6007 ticks sent, 0 underruns, grid held to
0.02 ms mean lateness across a live meeting run.

PTT press and release ramp in and out over a raised-cosine fade rather than
hard-gating — see §5 on why a hard edge is audible. Built and unit-tested in
`src/audio` (envelope, look-ahead limiter, smoothed gains, the pacer itself).

### 6.2 The capture queue

A small bounded ring of 20 ms frames between the capture callback and the TX
thread: 48 kHz, mono, 16-bit. Drop-oldest on overflow, and count the drops.
Built (`src/audio/frame_ring`), with one recorded deviation from the original
spec: mutex-guarded rather than lock-free, because drop-oldest makes the
producer a writer of the read index and a correctly-boring mutex at 50
pushes/second beat a subtly wrong lock-free ring inside the measurement path.
An in-process queue with no OS objects; it crosses a process boundary only if
multi-meeting workers (§3.2) ever exist.

### 6.3 Local audio I/O

Recommend **miniaudio** (single header, WASAPI + CoreAudio, no heavy
dependency) over Qt Multimedia, which is convenient but adds latency on top of
Zoom's — and latency is the thing being sold.

Needs: input and output device selection, gain and a limiter, sidetone control,
per-channel monitor mix, and AEC (§2 — not optional, and the reason is that
feeding raw PCM to the virtual mic bypasses Zoom's own echo canceller
entirely).

### 6.4 Control surface

A local line-oriented control API so hardware panels and automation can drive
talkback without the GUI: `status`, `channels`, `talk {channel, on}`,
`latch {channel, on}`, `listen {channel, level}`, `all_call {on}`.

A Bitfocus Companion module is the cheapest route to Stream Deck support. Two
things bite anyone writing one: dropdown choices are baked in when actions are
built, so a channel-roster change must rebuild the action definitions rather
than only pushing variables; and channels must be keyed by a stable id (§5).

**Built (v0.1.x).** The control surface is HTTP on `127.0.0.1:7350`: an SSE
state stream and a one-line `POST /act` action API, with the panel itself as
its first consumer. Verbs today are `talk`, `latch`, `talkall`, `latchall`,
`assign`, `feed set|latch|gain|off`, `cue`, `notify`, `bo`, `room`, `join`,
`passcode`, `signin`, `signout`, `quit`. The Companion module is not written.

### 6.5 Process model

Phases 1 and 2 are one process and need nothing here (§3.2). If multi-meeting
workers ever exist — one SDK client per *meeting*, for an operator running two
shows — each is launched by the UI, owns exactly one SDK client, and is
tracked by the UI as its own child: the parent knows its workers' pids
because it started them, so cleanup never needs to guess from a process list.
Name every OS object under the `ZComms` prefix (§3.3), and never name a
binary so that another product's cleanup could mistake it for one of its own
— note that the SDK's machine-wide exclusivity (§5) means such workers may
not be able to coexist at all until Spike C says otherwise.

## 7. Admin portal and the group model

**NOT BUILT — none of this section exists in the product.** It is Phase 2,
and it is here as the design to build against, not as a description of
anything that runs today. The panel's routing today is one operator against
a live roster, resolved in-process and forgotten when the meeting ends.

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

The honest limits from §2, updated: **per-guest IFB inside a shared meeting
is now possible** — a talkback channel with one member is exactly that, and
`SetChannelBackgroundVolume` is the program duck under the producer's voice,
delivered by Zoom. The constraint that replaced impossibility is *capacity*:
16 channels and 10 listeners per channel. The matrix resolver must pack
groups into channels, refuse configurations that cannot fit, and route the
all-hands case (more than 10 listeners) to the virtual mic or the native
fabric rather than silently truncating a channel's membership.

### 7.3 Backend

Supabase is the right fit and is already in the toolchain: Postgres for the
model, RLS for org isolation, Realtime for presence and live preset pushes,
Edge Functions for the admin API. The endpoint holds a cached copy of its
resolved matrix and keeps working through a backend outage.

## 8. Phasing

**Phase 0 — Spikes. Spike A DONE and passed.** B overtaken by production
use, C and D still open (§9). Neither blocks shipping; both are things that
should be known before a customer finds them.

**Phase 1 — The talkback panel. SHIPPED.** v0.1.0 through v0.1.14, all
installers on GitHub Releases. What this phase promised and now exists:

- One process, the client's meeting, the whole 16-channel bank provisioned
  in one request; PTT and latch per person, ALL CALL, latch-all.
- Channel setup from the participant list — every capable participant
  auto-assigned their own channel, EDIT TALENT to move anyone anywhere.
- The audio chain: gain, look-ahead limiter, ramped PTT envelope, AEC,
  sidetone, test tone, live device switching.
- Sign-in: Zoom OAuth through the broker, ZAK joins, DPAPI token storage.
  No anonymous joins (`--anon` remains for scripted runs only).
- Packaging: per-user NSIS installer, no UAC, Zoom SDK inside, WebView2
  shell, app icon and dark chrome, QUICKSTART shipped alongside.
- Beyond the original scope, because live meetings demanded it: breakout
  awareness, chat signaling, a crash trap, an always-written readable log
  with two watchdogs, and extern feeds (v0.1.9–v0.1.14).

Still owed from this phase: **code signing** (needs a Microsoft developer
account — until then SmartScreen warns every new user), **ZComms' own Zoom
Marketplace identity** (§3.6, a review cycle with lead time), an updater, and
the live gates listed in `CLAUDE.md` that separate "built" from "proven in
front of an audience".

**Phase 2 — Matrix, party lines, admin. NOT STARTED.** ~8–12 weeks.
Nothing in §7 is built: no orgs, users, groups, presets, presence, or audit
log, and no backend at all. The panel today resolves a matrix of one
operator against a live roster, held in the process. What the shared
roadmap with CoreVideo adds ahead of the portal: **named party-line
channels with multiple members**, and **per-person private channels becoming
opt-in rather than automatic** (they cost channel budget out of a hard 16).
The two SDK unknowns at the end of §9 gate both. A Bitfocus Companion module
is the cheapest route to Stream Deck support and the control surface it
would drive already exists (§6.4).

**Phase 3 — Native fabric. NOT STARTED.** ~12+ weeks.
Own low-latency transport (Opus over a self-hosted SFU) for wire-class
crew-to-crew — 165 ms is fine for IFB and cueing, not for a camera operator
tracking a director — with the Zoom talkback leg as one endpoint on it.
This is where the Unity comparison becomes fair.

The go-to-market wedge stays what §1 argues: crews already running remote
guests over Zoom, for whom a native Zoom leg is the thing no incumbent
intercom offers.

## 9. Spikes — status

**Spike A — TX latency. DONE, PASSED (2026-08-26).**
Measured over the talkback transport into a real meeting, observed at a plain
Zoom client on the same machine, one clock end to end, emission timestamped
at the actual send call, arrival recovered by matched-filter correlation with
a least-squares capture timebase. The instrument was proven against known
synthetic delays (45/150/275/600 ms recovered to within 0.1 ms) before any
live figure was believed.

*Result: **median 165.0 ms, p95 193.6 ms** (55 samples, MAD 4.2 ms,
p95−p50 jitter 28.6 ms). Local calibration bias 33.2 ms, so the true
Zoom-path median brackets to (132, 165] ms. Both figures inside the ~250 ms
kill criterion — the Zoom-transport thesis survives.* The harness lives at
`spikes/a-tx-latency/` and re-runs in ~10 minutes; use `--tap-all`.

Not measured: the virtual-mic path, whose send window never opened under
public-app-key auth. No latency figure exists for it; do not quote one.

**Spike B — Mute policy and identity. Overtaken by production use, not
formally closed.**
Answered here: co-host role gates channel creation; a guest SDK client lands
in the waiting room like anyone else; "ask to unmute" is a consent request
the client must accept; the client appears in the participant list under its
display name. Answered since, the expensive way, in live meetings — these
are now the delivery laws in `CLAUDE.md`, not spike questions: talkback
delivers only while this client's meeting audio is open; it does not cross
breakout rooms; a same-account host collision hangs the join; Zoom ducks
channel members by default; `SendAudioDataToChannel` is mono only. Still
open: behaviour under "mute all" / "participants cannot unmute" *while a
channel is live*, and whether co-host demotion mid-show destroys channels.

**Spike C — SDK exclusivity. Still open, 1 day.**
`InitSDK` fails with `SDKERR_OTHER_SDK_INSTANCE_RUNNING` while *another
application's* SDK engine runs — machine-wide, not per-process. The product
already treats this as fact: `SdkConflictHint()` names the offending process
by scanning for known SDK hosts, because the raw error reads as a broken
install. What is still unconfirmed is the boundary (two ZComms processes;
ZComms beside another SDK app; whether `sdkPathPostfix` isolates data paths
and changes the answer).
*Kill criterion, unchanged: if the SDK is genuinely one-instance-per-machine
with no isolation escape, ZComms cannot run alongside any other Meeting SDK
product on an operator's machine — including CoreVideo, on the same
operator's rig. That must be known before a customer discovers it.*

**Spike D — Zoom ISV conversation. Still open. Calendar time, not
engineering time.**
The core product is **one SDK session per operator**, not per channel — a
friendlier licensing story. The questions for Zoom: talkback API entitlement
across account tiers (it has worked on ordinary accounts through every live
session so far; is that stable policy?), the `ZComms` name (§3.7), and the
multi-meeting case's session math for Phase 3.

**Two SDK unknowns neither this project nor CoreVideo has probed**, and both
gate the party-line work in Phase 2:

1. Can one user be a member of two channels simultaneously? Needed for a
   party line overlapping a private. Probe: invite one uid into two
   channels, watch both join callbacks, key each, verify delivery.
2. What does the invitee's stock Zoom client *display* on channel invite or
   membership? Audit from the receiving side. If Zoom itself shows a banner,
   that bounds how quiet bring-up can ever be, whatever we do about chat.

## 10. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| ~~Zoom latency unfixable~~ | *Retired: measured 165/194 ms, inside the bar* | Spike A result, 2026-08-26 |
| ~~No AEC on raw TX~~ | *Retired: speexdsp MDF carried in-process, 40.6 dB ERLE unit-proven, on by default* | src/audio/aec |
| ~~Endpoint rendering surprise~~ | *Retired as a surprise: it is now documented operator-facing in README and QUICKSTART* | Support docs shipped |
| **Talkback API entitlement is undocumented policy** | The core transport worked on ordinary accounts across many live sessions; Zoom could gate it by tier or change it — it is the API behind a paid product (ZoomISO) | Spike D asks Zoom directly |
| **The app is unsigned** | SmartScreen warns every new user on first run; a locked-down machine may refuse it outright | Microsoft developer account, then sign in release.ps1 |
| **The undiagnosed hang** | Two AppHangB1 occurrences, neither reproduced; a mid-show wedge is a show-stopper | Not fixed — made diagnosable (always-written readable log, main-loop and UI-thread watchdogs, bounded quit). The next occurrence names itself |
| **Extern feeds unproven in a meeting** | The v0.1.9–v0.1.14 feature set is unit-proven and bench-proven, not audience-proven | The M0 gate list in CLAUDE.md: audible to members, latched-silent leaves unity, barge duck, long soak |
| **Channel capacity (16 × 10)** | An all-hands page to more than 10 people cannot be one channel | Provision the whole bank once and pack honestly; virtual mic / fabric for all-hands |
| **Co-host dependency** | ZComms must be promoted in every client meeting; a host demotion mid-show may kill channels | Surfaced loudly and retried; Spike B still owns the demotion question |
| SDK is one-instance-per-machine | ZComms cannot run beside other SDK apps (incl. CoreVideo) on one machine | Detected and named by SdkConflictHint; Spike C confirms the boundary and the sdkPathPostfix escape |
| Virtual-mic path needs JWT-auth | Party-line case blocked under bare public-app-key | Retired for the auto-suppress case: the broker-JWT tier DOES carry the entitlement, live-verified 2026-08-29. Still open for a real party-line send |
| Marketplace review slips | Built, cannot ship under its own identity | Start the app identity now |
| `ZComms` name rejected at review | Rebrand after build | Raise the name in the Spike D conversation |
| Unity's moat is trust, not features | Slow adoption | Lead with the Zoom leg; sell to crews already on Zoom |