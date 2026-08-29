# ZComms — Quickstart

ZComms is a talkback panel for Zoom meetings: it joins the meeting as its own
participant, puts your panelists on private audio channels, and lets you talk
to them — one channel, several, or all — without the audience or the
recording hearing a word.

## Start a session

1. **Double-click `zcomms.exe`** — the panel opens in its own window. Paste
   the meeting link into the JOIN A MEETING card and hit CONNECT (or run
   `zcomms --meeting <link>` from a terminal).
2. In the meeting, **admit "ZComms"** from the waiting room, then make it
   **Host or Co-Host** (Participants → More). It needs that role to create
   channels — it will keep retrying while you do this. As host it also admits
   your panelists for you.
3. If a join fails, the reason appears in the ticker at the bottom and the
   join card comes back — fix the cause and connect again. Closing the
   window quits the app.

## Use the panel

- **Hold a TALK key** (or digit 1–9) to speak to that channel; **SPACE** is
  all-call. **LATCH** for hands-free.
- Panelists appear in the left column and land on **CH 1** automatically.
  Click the numbered chips on a person to move them between channels.
- Meeting audio ducks under your voice for people in a channel; everyone
  else hears nothing at all.

## Things worth knowing

- **Panelists must be on the Zoom desktop or mobile app.** The browser
  (web) Zoom client cannot receive talkback — the panel marks such people
  `NO TALKBACK`.
- Echo cancellation is built in and on by default. Headsets are still the
  professional choice.
- More options: `zcomms --help` (channels count, devices, gain, ports).
- The panel is served only on this machine (127.0.0.1). A Stream Deck /
  Companion integration can drive the same API.

## Troubleshooting

- *"another app's Zoom engine is running (error 14)"* — close other apps
  that embed the Zoom SDK (e.g. OBS plugins), then relaunch.
- *Stuck at "promote ZComms…"* — the host must grant Host or Co-Host.
- *A panelist can't hear talkback* — check they're not on the web client,
  and that they appear `ON CH` in the roster.
