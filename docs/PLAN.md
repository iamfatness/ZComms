# ZComms — standalone intercom on the Zoom Meeting SDK

Status: **design, pre-commit.** Nothing here is built yet. Written 2026-08-25
ZComms is a **standalone product**: its own repo, its own
Zoom Marketplace identity, its own codebase, no dependency on any other
product. The purpose of this document is to decide *whether* and *in what
order* to build it, and to name the four spikes that make that decision
cheaply.

## 1. The strategic read

A Zoom-transport intercom that competes with Unity Intercom head-on is a bad
bet. A **native intercom fabric with a first-class Zoom leg** is a good one,
and it is the only version of this product that Unity structurally cannot copy
without rebuilding a Zoom media stack from scratch.

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

### 3.2 One SDK client per meeting is still a process boundary

The simplification is not total. The Meeting SDK is a **process-level
singleton**: one initialised SDK, one authenticated client, one meeting. Since
a Zoom meeting is a channel (§1), multi-channel means multiple SDK clients,
which means multiple processes no matter how the app is written.

So the shape is:

- **Phase 1, one channel:** a single process. UI and SDK together, no IPC.
- **Phase 2, N channels:** the UI process plus one small `zcomms-channel`
  worker per channel.

Those workers are ours and purpose-built: audio only, mono 48 kHz PCM, one
stream in and one out. No video, no screen share, no recording. That is a far
smaller bridge than a general-purpose media helper, and it should be designed
as the narrow thing it is rather than as a general one.

**Verify the singleton before building on it** — it is an assumption inherited
from prior art, not something this project has measured. Spike C (§9) does
that, and it also produces the per-channel RAM and CPU numbers that set the
per-machine channel ceiling and therefore the pricing model.

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

Being unrelated means building, not inheriting:

- The full SDK integration: auth, join, meeting lifecycle, raw-audio subscribe
  and the virtual-mic send path.
- A sample-derived audio clock, drift handling, and gap accounting.
- OAuth sign-in and token storage.
- The app shell, the control surface, packaging, code signing, an updater.

None of it is exotic and all of it is weeks. §8's Phase 1 sizing reflects that
honestly rather than assuming a head start.

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

Phase 1 — one channel, one process, no IPC:

```
┌──────────────────────────────────────────────┐
│  ZComms (single process, one per operator)   │
│  ┌────────────────────────────────────────┐  │
│  │ UI · PTT/latch · faders · monitor mix  │  │
│  ├────────────────────────────────────────┤  │
│  │ audio engine · capture · AEC · limiter │  │
│  ├────────────────────────────────────────┤  │
│  │ Zoom Meeting SDK (linked in-process)   │  │
│  │  RX one-way raw audio · TX virtual mic │  │
│  └────────────────────────────────────────┘  │
└───────────────────┬──────────────────────────┘
                    │  one meeting = one channel
              ┌─────▼──────┐
              │  Meeting A │
              └────────────┘
```

Phase 2 — N channels, because the SDK is a per-process singleton:

```
┌───────────────────────────────┐   ┌──────────────────────┐
│ ZComms UI                     │◄─►│ Admin backend        │
│ mixer · matrix · presence     │   │ orgs · users · groups│
└──┬─────────────┬──────────────┘   │ channels · matrix    │
   │ audio-only bridge (mono 48k)   │ presence · audit log │
┌──▼──────────┐ ┌▼─────────────┐    └──────────────────────┘
│zcomms-      │ │zcomms-       │
│channel #1   │ │channel #2    │  … one worker per channel
│SDK · RX/TX  │ │SDK · RX/TX   │
└──┬──────────┘ └┬─────────────┘
┌──▼───────┐  ┌──▼───────┐
│ Meeting A│  │ Meeting B│
└──────────┘  └──────────┘
```

Phase 3 adds a native transport beside the Zoom leg; the mixer does not care
which leg a channel arrives on.

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
  nobody after a rejoin and at the wrong person once ids get reused.

## 6. Components

### 6.1 The virtual mic — `ZoomMicSource`

A `IZoomSDKVirtualAudioMicEvent` implementation. It holds the
`IZoomSDKAudioRawDataSender*` between `onMicInitialize` and
`onMicUninitialized`, and a `can_send` flag between `onMicStartSend` and
`onMicStopSend`. Nothing may call `send()` outside that window.

**A fixed-cadence TX thread, not an event-driven one.** A dedicated thread
wakes every 20 ms, takes one frame from the capture queue and calls `send()`.
This is deliberate on three counts:

- Zoom wants a *steady* stream. A virtual mic that streams continuously and
  goes quiet — rather than starting and stopping — is the behaviour the SDK
  handles best.
- A paced puller needs **no wakeup protocol at all**, which removes an entire
  class of consumed-wakeup bugs (§5) before it can exist.
- Underrun becomes a normal, countable condition rather than a silence bug: no
  frame ready at the tick, send silence, increment `mic_underrun`. Prime 2–3
  frames (~40–60 ms) so ordinary jitter never underruns.

PTT press and release ramp in and out over a short fade rather than hard-gating
— see §5 on why a hard edge is audible.

### 6.2 The capture queue

A small bounded lock-free SPSC ring of 20 ms frames between the capture
callback and the TX thread: 48 kHz, mono, 16-bit. Drop-oldest on overflow, and
count the drops. In Phase 1 this is an in-process queue with no OS objects at
all; in Phase 2 the same contract crosses to the channel worker, which is the
only reason the boundary is worth naming now.

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

### 6.5 Process model

Phase 1 is one process and needs nothing here. Phase 2's channel workers do:
each is launched by the UI, owns exactly one SDK client, and is tracked by the
UI as its own child — the parent knows its workers' pids because it started
them, so cleanup never needs to guess from a process list. Name every OS object
under the `ZComms` prefix (§3.3), and never name a binary so that another
product's cleanup could mistake it for one of its own.

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

Rough sizing, deliberately coarse, and assuming a greenfield build — there is
no head start to draw on (§3.4).

**Phase 0 — Spikes (§9).** ~1 week. Decides everything below.

**Phase 1 — Single-channel talkback, Zoom transport.** ~8–12 weeks.
One process, one meeting, PTT and latch, monitor mix, AEC, device selection,
sign-in, packaging and signing. Ships as "producer talks to remote guests",
which is genuinely useful on its own and is the smallest thing that proves the
send path against a real meeting.

**Phase 2 — Multi-channel + admin.** ~10–14 weeks.
One worker per channel, the routing matrix, groups, presets, admin portal,
presence, audit log, control surface. This is the first releasable *intercom*.

**Phase 3 — Native fabric.** ~12+ weeks.
Own low-latency transport (Opus over a self-hosted SFU) for crew-to-crew, with
the Zoom leg as one endpoint on it. This is where the Unity comparison becomes
fair, and where per-guest IFB becomes possible.

The go-to-market wedge stays what §1 argues: crews already running remote
guests over Zoom, for whom a native Zoom leg is the thing no incumbent
intercom offers.

## 9. Spikes, with kill criteria

Run these before committing engineering to Phase 1. Each is cheap; together
they decide the architecture.

**Spike A — TX round-trip latency. 1–2 days. The decisive one.**
The smallest possible SDK harness: init, auth, join, `setExternalAudioSource`,
push a 1 kHz tone, and measure the delay to a second Zoom client. Timestamp
both ends against one clock — capture the far end's audio locally and correlate
against the emission time, so the number is real end-to-end latency rather than
a sum of guesses. This harness is also the seed of §6.1, so it is not throwaway
work.
*Kill criterion: if one-way latency exceeds ~250 ms, the Zoom-transport
intercom thesis is dead for live crew use. Phase 1 still ships as
producer-to-guest talkback, but Phase 3 moves to the front of the queue.*

**Spike B — Mute policy and identity. 1 day.**
Can the SDK client unmute reliably? What happens under "mute all" and under
"participants cannot unmute"? How does the talkback client appear in a normal
client's participant list, and is that acceptable to put in front of a client's
audience?

**Spike C — The SDK's process model. 2 days.**
Two questions Phase 2's whole shape rests on, neither of which this project has
measured. First: is the Meeting SDK genuinely a per-process singleton, or can
one process hold two authenticated clients in two meetings? An affirmative
would collapse §3.2's worker model into a single process. Second, whichever
answer: run two channels simultaneously and measure RAM and CPU per channel —
that sets the per-machine channel ceiling and therefore the pricing model.
*Kill criterion: if per-channel cost makes a realistic 6–8 channel operator
station impractical on ordinary hardware, the per-channel-meeting architecture
does not survive Phase 2 and the native fabric has to carry crew-to-crew.*

**Spike D — Zoom ISV conversation. Calendar time, not engineering time.**
An intercom multiplies concurrent Meeting SDK sessions per customer by the
channel count. Get Zoom's position on that licensing shape **before** Phase 2,
not after. It can invalidate the per-channel-meeting architecture outright.

## 10. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Zoom latency unfixable | Cannot serve live crew intercom | Spike A first; Phase 3 native fabric |
| Zoom licensing blocks N sessions | Phase 2 architecture invalid | Spike D before Phase 2 |
| SDK is not a per-process singleton, or is worse than assumed | Phase 2 process model wrong | Spike C measures it before it is built on |
| No AEC on the virtual mic | Echo into the client's meeting | Headset mandate + device detection, or bundle an AEC |
| Greenfield scope underestimated | Phase 1 slips | §3.4 names what must be built; no head start assumed |
| Marketplace review slips | Phase 1 built, cannot ship | Start the app identity before Phase 1 code |
| `ZComms` name rejected at review | Rebrand after build | Raise the name in the Spike D conversation |
| Unity's moat is trust, not features | Slow enterprise adoption | Lead with the Zoom leg; sell to crews already on Zoom |
