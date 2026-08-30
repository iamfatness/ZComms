# ZComms

Talkback / IFB station for Zoom. Push a key, your voice lands in one
panelist's ear — the meeting, the recording, and the room hear nothing.

Built for live production: a director briefing talent inside the client's
Zoom meeting, with the visual language and fail-closed discipline of a
hardware intercom panel. No virtual audio cables, no spare machine, no bot
account — ZComms joins the meeting as you.

**Status: v0.1.0 shipped.** Grab the installer from
[Releases](https://github.com/iamfatness/ZComms/releases) — per-user, no
admin rights needed. The build is not yet code-signed, so SmartScreen warns
on first run ("More info → Run anyway").

## What it does

- **Sign in with Zoom once** (browser PKCE); every join happens as your
  account. Paste any meeting link or ID into the panel.
- **A named key per panelist.** Each capable participant automatically gets
  their own standing talkback channel — their key wears their name, and a
  press is a private line. **ALL CALL** spans the whole panel; **latch**
  makes presses stick; **Edit Talent** builds group channels.
- **The room stays clean.** The meeting mic is auto-suppressed: open (Zoom
  requires that for talkback delivery) but silent to the meeting — pure SDK
  API, no audio driver installed.
- **Honest keys.** A key is red only while someone is actually hearing you.
  Keyed with nobody in the channel reads amber with "nobody in channel";
  someone in another breakout room shows dark with "in <room>" and refuses
  the press, because Zoom talkback cannot cross rooms. The station can move
  itself between rooms (Settings → Station Room).
- **A real capture chain**: input gain, look-ahead limiter, ramped PTT
  envelope, echo cancellation (speexdsp), sidetone, a built-in test tone,
  and device pickers that switch live.
- **Local control surface**: the panel is served on `127.0.0.1:7350` with an
  SSE state stream and a one-line action API — the same seam a Stream Deck /
  Companion module drives.

## What panelists need

A native Zoom client (desktop or mobile). The Zoom **web** client cannot
receive talkback; the panel marks such people `NO TALKBACK`. Nothing to
install on their side — a plain Zoom client hears channel audio natively.

## Platform truths (the SDK's rules, surfaced honestly)

- Channel creation needs ZComms promoted to **host or co-host**; as host it
  also admits your panelists from the waiting room.
- Talkback **does not cross breakout rooms** — the panel says so per person
  instead of pretending.
- Meetings hosted by Zoom accounts that never authorized the app are refused
  by Zoom at join; the panel names it.
- Your account can't be hosting a meeting on another device while ZComms
  joins as you — ZComms declines to end it and says why.

## Building from source

Windows x64, Visual Studio 2022, CMake ≥3.20.

```
cmake -S . -B build -A x64
cmake --build build --config Release --target zcomms
```

The Zoom Meeting SDK is **not redistributable in a source tree**:
`third_party/zoom-sdk/` is gitignored and must be populated with the Windows
Meeting SDK (7.1.5+) before the `zcomms` target builds. Without it, the
audio engine, tests, and tools still build. The WebView2 SDK fetches itself
at configure time. `tools/release.ps1` builds, tests, stages, zips, and
produces the NSIS installer.

`CLAUDE.md` is the living engineering state — architecture, invariants, and
the live-found platform behaviors with their receipts. `docs/PLAN.md` is the
original architecture plan, kept for the reasoning.

## Roadmap

Code signing (pending a Microsoft developer account), ZComms' own Zoom
Marketplace identity (guest join + raw-data entitlement — both requirements
now proven), live verification of breakout awareness, and a macOS port (the
panel is one HTML file; the shell is thin).
