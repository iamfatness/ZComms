# ZComms

Talkback / IFB station for Zoom. Hold a key, your voice lands in one
panelist's ear — the meeting, the recording, and the room hear nothing.

Built for live production: a director briefing talent inside the client's
own Zoom meeting, with the visual language and fail-closed discipline of a
hardware intercom panel. No virtual audio cables, no spare machine, no bot
account — ZComms joins the meeting as you, and the talent installs nothing.

It works because Zoom's Meeting SDK carries private talkback channels
*inside* a single meeting: up to 16 of them, up to 10 listeners each,
delivered over each listener's ordinary Zoom connection. A plain Zoom
client hears channel audio with nothing installed. Measured one-way latency
on that path is **165 ms median / 194 ms p95** — IFB-class and
cueing-class, not wire-class.

## Status

**v0.1.14**, shipped 2026-09-02. This is early software: fourteen releases
in a week, several of them fixing something the release before had just
shipped broken. Read [What is not verified yet](#what-is-not-verified-yet)
before putting it on a show that matters.

Installer:
[latest release](https://github.com/iamfatness/ZComms/releases/latest).
It installs per-user (`%LOCALAPPDATA%\ZComms\app`), so it never asks for
admin rights. The Zoom Meeting SDK ships inside it; there is nothing else
to install. The build is **not code-signed**, so Windows SmartScreen warns
on first run — "More info" then "Run anyway" is the way past it. Signing
waits on a Microsoft developer account.

## Sign in

There are no anonymous joins. On first launch the panel is a SIGN IN card:
your browser opens for a one-time Zoom approval, and from then on every
join happens as your own Zoom account, using your own access to the
meeting. Tokens are stored encrypted (DPAPI) under `%APPDATA%\ZComms`, and
a revoked or rotated token re-asks rather than retrying.

Then paste a meeting link or ID and hit CONNECT. In the meeting, make
"ZComms" host or co-host — Zoom requires that role to create talkback
channels, and ZComms keeps retrying while you arrange it. As host it also
admits your panelists out of the waiting room itself.

## The desk

The panel is an intercom grid, and the unit on it is a **person**, not a
channel number. Every participant who can receive talkback is provisioned
onto their own channel at bring-up, so their key wears their name.

- **Hold a cell to talk** to that person alone. Digits 1–9 key the first
  nine directly; SPACE is **ALL CALL**, which is also a button above the
  grid.
- **LATCH** makes presses stick — press again to release. The same gesture
  on the ALL CALL bar latches everyone.
- **EDIT TALENT** opens the roster with all sixteen channel chips beside
  each person: put anyone on any channel, including one somebody else is
  already on (a channel takes ten). That is how group lines get built.
- **A key's colour is the truth.** Red means someone is actually hearing
  you. Amber means keyed with nobody in the channel. A cell goes dark with
  `in <room>` and refuses the press when that person is in a breakout room
  the station is not in, because Zoom talkback does not cross breakout
  rooms; SETTINGS then STATION ROOM moves the station.
- The rail lamps are **LINK** (in the meeting), **MTG MIC**, **CHANNEL**
  (channels up) and **TX** (on air).

**MTG MIC deserves a note**, because it is the most confusing thing about
this platform. Zoom delivers talkback only while the sending client's own
meeting audio is open. Muted, the send is *accepted* — zero errors,
members confirmed — and every listener hears silence. So ZComms holds its
meeting mic open and points it at a never-fed SDK virtual source: open to
Zoom, silent to the room. No audio driver is installed to do it.

The capture chain is a real one: input gain, look-ahead limiter, ramped PTT
envelope, echo cancellation (speexdsp — raw PCM into Zoom bypasses Zoom's
own AEC, so ZComms carries its own), sidetone, a test tone that runs
through the whole live chain, and device pickers that switch while running.
Use a headset anyway.

## Extern feeds

A feed latches one channel — or a stereo pair, downmixed — of any
multichannel capture device into a talkback channel. Dante, a console bus,
another intercom's mix: Zoom becomes the last mile of a comms system that
is bigger than Zoom.

SETTINGS holds the feed rows. Pick a SOURCE, a CHANNEL (a real picker,
built from the device's own native channel count, with a free-text fallback
for drivers that will not report one), and **HEARD BY** — which names the
person on the destination channel rather than the slot number, because that
is the question you are actually answering. Feeds persist in
`%APPDATA%\ZComms\feeds.env` and are restored at launch.

Every feed row carries an input meter, sat before the gain keys and tapped
*pre-latch*, so it reads whether or not the feed is on air. It runs
−60…0 dBFS over 12 segments, with the −50 dBFS signal gate tick-marked
on it. That line matters: below it, audio is silence as far as the system
is concerned — it will not duck the room, and the lamp reads
`latched · silent` even with the feed latched. One dim segment means
present but not yet counted; ride the gain up until you are clear of the
tick.

Ducking is signal-gated throughout, never state-gated: a latched-but-silent
feed leaves everyone's meeting audio at unity, and keying over a feed ducks
the feed under your voice only while your voice actually carries audio.

## What panelists need

A native Zoom client — desktop or mobile. The Zoom **web** client cannot
receive talkback at all; that is a Zoom property, not a ZComms one, and the
panel says so on the person's cell (`no talkback · web`) with the one fix
in the tooltip: rejoin in the desktop app. Nothing to install otherwise.

One more thing worth telling a panelist who hears nothing on a red key:
Zoom renders talkback to their operating system's **communications**
device, which on a multi-bus interface is often not the speaker their Zoom
settings name.

## What is not verified yet

This section is the point of the README. The following are built and
unit-tested but have not been proven in front of an audience.

- **Extern feeds in a live meeting.** Feeds are verified on one machine —
  device opened, latch and gain applied, config persisted and restored —
  and the meter and gate behaviour are pinned by tests. Not yet verified
  live: that a feed is audible to channel members, that a latched-silent
  feed really leaves the meeting at unity, that the barge duck sounds
  right, and the hours-long latched soak.
- **Breakout awareness** was live-verified for room truth, dark cells and
  key refusal against four running breakout rooms, but ZComms creating and
  staffing its *own* breakout rooms has not been run live.
- **Chat signaling** desk-to-desk needs a second desk and has not been
  exercised.
- **One hang is undiagnosed.** Two occurrences of an AppHangB1 — Windows
  killing the process for not pumping messages — neither reproduced.
  v0.1.11 did not fix it; it made it *diagnosable*. The log is now always
  written and readable while running, and two watchdogs report out of band,
  so the next occurrence should name itself.

Zoom's talkback API entitlement is also undocumented policy. It worked on
an ordinary account, but it is the API behind a paid Zoom product, and
nothing stops Zoom gating it by tier.

## Help

The status strip at the bottom of the panel explains failures in plain
language; click it for the scroll-back. Every run writes a log to
`%APPDATA%\ZComms\logs\`, readable while the app is running, size-capped,
keeping the last ten runs — the app prints its path at startup.

Bugs and questions: [Issues](https://github.com/iamfatness/ZComms/issues).
Attach the log.

## Building from source

Windows x64, Visual Studio 2022, CMake ≥3.20.

```
cmake -S . -B build -A x64
cmake --build build --config Release --target zcomms
```

The Zoom Meeting SDK is **not redistributable in a source tree**:
`third_party/zoom-sdk/` is gitignored and must be populated with the
Windows Meeting SDK (7.1.5+) before the `zcomms` target builds. Without it
the audio engine, the tests and the tools still build. The WebView2 SDK
fetches itself at configure time. `tools/release.ps1` builds, tests,
stages, zips and produces the NSIS installer.

`zcomms --list-devices` prints every capture and playback device with its
native channel count, which is what a feed needs. `zcomms --help` covers
the headless flags.

The panel is also a local control surface: it is served on
`127.0.0.1:7350` with an SSE state stream and a one-line action API — the
same seam a Stream Deck / Companion module drives.

`CLAUDE.md` is the living engineering state: architecture, invariants, and
the live-found platform behaviours with their receipts. `docs/PLAN.md` is
the architecture plan, kept for the reasoning.

## Roadmap

Code signing (pending a Microsoft developer account); ZComms' own Zoom
Marketplace identity; closing out the live gates above; named party-line
channels, shared with the CoreVideo intercom model; a macOS port (the panel
is one HTML file and the shell is thin).