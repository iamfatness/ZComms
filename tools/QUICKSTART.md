# ZComms — Quickstart

ZComms is a talkback station for Zoom: it joins the meeting as you, gives
every panelist their own named talk key, and lets you speak to one person,
a group, or everyone — without the audience, the recording, or the room
hearing a word.

## First run

1. **Install and start ZComms** (Start menu → ZComms). Windows SmartScreen
   warns because the build is not yet code-signed — "More info → Run anyway".
2. **SIGN IN WITH ZOOM** — your browser opens for a one-time approval.
   ZComms joins meetings as your Zoom account from then on.
3. **Paste a meeting link or ID** into the JOIN card and hit CONNECT.
4. In the meeting, make **"ZComms" host or co-host** (Participants → More).
   It needs that role to create channels — it keeps retrying while you do —
   and as host it also admits your panelists from the waiting room.

## The desk

- Every panelist lands on **their own channel** — their key wears their
  name. **Hold a key to talk** to that person alone; digits 1–9 work too.
- **ALL CALL** (or SPACE while the panel is focused) talks to everyone.
- **LATCH** mode makes presses stick — press again to release.
- **EDIT TALENT** shows the channel chips for building groups.
- **SETTINGS** holds the microphone and sidetone pickers, gain, echo
  cancel, and a test tone that runs through the whole transmit chain.

A key's color is the truth: **red = someone is hearing you**, amber =
keyed but nobody is in the channel yet, dark with `in <room>` = that
person is in another breakout room, where talkback cannot reach (move the
station there: SETTINGS → STATION ROOM).

The rail lamps: **LINK** (in the meeting), **MTG MIC** (meeting audio open
— required for talkback delivery; ZComms keeps it open and auto-suppressed
so the room hears silence), **CHANNEL** (channels up), **TX** (on air).

## Things worth knowing

- **Panelists must be on the Zoom desktop or mobile app.** The web client
  cannot receive talkback — such people are marked `NO TALKBACK`.
- Talkback **cannot cross breakout rooms** (a Zoom rule). The panel shows
  who is where and refuses keys that cannot deliver.
- Your Zoom account cannot be hosting a meeting on another device while
  ZComms joins as you — leave it there first, or join a meeting you are
  not hosting.
- Meetings hosted by accounts that have not authorized this app are
  refused by Zoom; the panel says so.
- Headsets are still the professional choice.

## Troubleshooting

- *"close <name> (it holds the Zoom SDK)"* — another app's Zoom engine is
  running (OBS plugins, ZoomISO). Close it, connect again.
- *"your Zoom account is in a meeting on another device"* — see above.
- *A key stays amber* — that person's channel invite is still landing
  (seconds), or they left. Red means they hear you.
- *A panelist hears nothing on a red key* — have them check the device
  their OS uses for **communications** audio; Zoom plays talkback there.
- The status bar at the bottom explains every failure in plain language.
