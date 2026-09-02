# ZComms — Quickstart

A talkback station for Zoom. It joins the meeting as you, gives every
panelist their own named talk key, and lets you speak to one person, a
group, or everyone — without the audience, the recording, or the room
hearing a word.

This file ships with the app, so it works with no internet. Version 0.1.14.

## 1. Get in

1. **Start ZComms** (Start menu). The build is not code-signed yet, so
   SmartScreen warns on first run: "More info" then "Run anyway".
2. **SIGN IN WITH ZOOM.** Your browser opens for a one-time approval.
   ZComms joins meetings as your Zoom account from then on. There is no
   anonymous mode.
3. **Paste a meeting link or ID** into the JOIN card and press CONNECT.
   If the meeting has a passcode and it is not in the link, the same card
   asks for it.
4. In the meeting, make **"ZComms" host or co-host** (Participants, More).
   Zoom will not let it create talkback channels without that role. It
   keeps retrying while you do it, and as host it also admits your
   panelists from the waiting room.

Wait for the rail lamps: **LINK** (in the meeting) and **CHANNEL**
(channels up). **MTG MIC** must be lit too — Zoom only delivers talkback
while ZComms' own meeting audio is open, so the app holds it open and
feeds it silence. Nobody in the room hears you through it.

## 2. Get talking

Every panelist who can receive talkback lands on their own channel, so
each key wears their name.

- **Hold a cell** to talk to that person alone.
- **Digits 1–9** key the first nine cells directly.
- **SPACE**, or the **ALL CALL** bar, talks to everyone at once.
- **LATCH** turns presses into toggles: press to open, press again to
  close. Pressing ALL CALL in latch mode latches everyone.
- **EDIT TALENT** lists everybody with all sixteen channel chips. Click a
  chip to put that person on that channel, or take them off. A channel
  someone else is already on is marked busy but still works — a channel
  holds ten. That is how you build a group line.

**Watch the colour, not the button.** Red means someone is genuinely
hearing you. Amber means you are keyed and nobody is in that channel.
Dark with `in <room>` means that person is in a breakout room ZComms is
not in, where Zoom talkback cannot reach — the key refuses the press.
Move the station in SETTINGS, STATION ROOM.

## 3. Latch a feed

An extern feed latches one channel of a multichannel device — a console
bus, a Dante receiver, another intercom's mix — into a talkback channel,
so Zoom carries it the last mile.

SETTINGS, then the **extern feeds** block:

1. **SOURCE** — the capture device.
2. **CHANNEL** — which channel of it. The list is built from the device's
   real channel count. If the driver will not report one, type it
   (`3`, or `3-4` for a pair, which is downmixed to mono; Zoom's talkback
   transport is mono only).
3. **HEARD BY** — who receives it. The list names people first, then
   spare channels nobody is on yet.
4. **SET**, then **LATCH** on the row to put it on air.

Feeds are remembered and come back when you next launch.

## 4. Read the meters

Each feed row has a meter before its gain keys. It is tapped ahead of the
latch, so it reads whether or not that feed is on air — it answers "is
signal arriving?", not "am I transmitting?".

- Scale is **−60…0 dBFS** across 12 segments.
- The first two segments are **below the −50 dBFS gate** and drawn dim,
  with a tick at the line. Audio down there is still silence to the
  system: it will not duck anyone, and the row reads `latched · silent`
  even while latched.
- **One dim segment = present, but not counted yet.** Ride the gain up
  until you are clear of the tick.

The meter on the bottom strip is your own microphone.

## 5. When something is wrong

- **A key stays amber.** Nobody is in that channel. Either the invite is
  still landing (seconds) or they left. Red is the only "they hear you".
- **A cell reads `no talkback · web`.** That person joined on the Zoom web
  client, which physically cannot receive talkback. Ask them to rejoin in
  the desktop or mobile app. Nothing on your side fixes it.
- **A panelist hears nothing on a red key.** Zoom plays talkback to their
  operating system's *communications* device, which is often not the
  speaker their Zoom settings name. Have them check that.
- **"close <name> (it holds the Zoom SDK)".** Another app's Zoom engine is
  running — an OBS plugin, ZoomISO, a second ZComms. Only one Meeting SDK
  can be initialised on a machine. Close it and connect again.
- **"your Zoom account is in a meeting on another device".** You cannot
  host a meeting on your own Zoom client and have ZComms join as the same
  account. Leave it there, or join a meeting you are not hosting.
- **Everything you key is silent, with no errors.** Check MTG MIC. Zoom
  accepts sends from a muted client and delivers nothing.
- **The app stops responding.** It is a known, undiagnosed fault and it is
  rare. Every run writes `%APPDATA%\ZComms\logs\` — readable while the app
  is running — and the last lines will say what it was doing. Send that
  file with the report.

The status strip along the bottom of the panel explains every failure in
plain language. Click it to see the scroll-back.

Use a headset. Audio fed to Zoom this way bypasses Zoom's own echo
cancellation; ZComms carries its own (SETTINGS, ECHO CANCEL) but open
speakers are still open speakers.