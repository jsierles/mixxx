# Mixxx with the QuietVinyl interface

This is an independently maintained fork of Mixxx, based on the official **2.5.6**
tag (`3ebac449e7e5fe2a0186596657696e87ce8b0e56`). The maintained branch is
`codex/quietvinyl-2.5.6`. Mixxx's upstream identity and licensing are retained.

The initial source import reproduces the existing QuietVinyl patch exactly:
43 source/build files, 4,979 additions and 16 deletions. It adds the compact
browser, vinyl browsing, touch decks, Engine DJ USB reading, Tidal helper bridge,
remote browser bridge, fixed deck screens, MIDI identity and audio recovery.
It does not import another streaming fork or change the 2.5.6 baseline.

The companion QuietVinyl project maintains skins, controller mappings, the
Python Tidal helper, focused tests, packaging, hardware and operator notes.
Those resources are needed for a complete QuietVinyl installation; this fork
alone builds Mixxx with the optional interface code. Runtime profiles, media,
credentials, downloaded dependencies and compiled applications are not sources.

## Opt-in SC6000 deck display bridge

With the existing dual-screen presentation active, `QUIETVINYL_DENON_SOCKET`
opts into an owner-only local frame/touch socket. Without that environment
variable no display listener starts. Each client binds one deck, receives the
existing native UI as letterboxed 1280×800 RGB, and sends ordered gestures
through its original widgets. Text/vector controls render directly at player
pixel density. Modern allshader waveforms use a separate framebuffer at that
density, restoring the HDMI renderer and GL target afterwards; older GL types
retain their existing framebuffer capture. Layers keep their stacking order.
Stale view/geometry/sequence, duplicate deck
owners and modal/popup activity are refused; disconnects cancel held gestures.
The opaque frame epoch includes logical deck geometry, so a prefetched resize
cannot authorize input against the previously displayed image.
Frame admission is capped at about 60 requests/second per deck and counts
capture work toward that interval, allowing a fixed-cadence sidecar.

The companion project's `denon/` worker owns USB, zlib compression, changed
rectangles, serial assignments and reconnects outside the audio engine. This
initial implementation mirrors the existing HDMI-backed deck windows. It does
not implement display presentation without backing screens or change audio,
DVS, MIDI mappings or device firmware. The companion's dedicated native tests
and full-app generated-audio smoke cover frame layout, control routing, Browse,
GL pixels and the worker path; Pi hardware acceptance is a separate trial.

## Maintenance

Make source changes here as normal Git commits. The companion build pins a full
commit hash, rather than following this branch's moving tip. Commit and push
completed changes directly to this fork's default branch unless the owner asks
for a pull request. Coordinate source ownership before editing shared files.
Do not deploy to shared hardware as part of a source-only change.

Add the official upstream and the optional streaming reference locally:

```sh
git remote add upstream https://github.com/mixxxdj/mixxx.git
git remote add tidal-reference https://github.com/CurePortal/mixxx.git
git config remote.tidal-reference.fetch '+refs/heads/experimental/streaming:refs/remotes/tidal-reference/experimental/streaming'
```

Upstream release/security fixes are the primary source to assess. Review selected
fixes before backporting to 2.5.6. CurePortal's streaming work is a reference,
not a merge parent or build dependency: our UI uses a separately maintained
Python helper and verified local-file cache. Do not automatically merge either
remote or upgrade the release base. Future contributions upstream should isolate
generally useful fixes from QuietVinyl's product choices.

## Mac preparation application

The optional companion `QuietVinylPreparation` skin uses `[Preparation],enabled`
to expose paused-track analysis/re-analysis, explicit 4/4 downbeat setting,
one-beat bar-phase shifts, downbeat undo and periodic saved-preparation updates.
A confirmed downbeat is stored as cue type **9**, hotcue index -1, with the
standard engine-sample position representation. It is a preparation marker,
not a playable hotcue. Re-analysis clears it; small grid changes snap it back
to the nearest beat. The companion converters validate it against the grid.
Other skins do not enable these preparation operations by default.

A bundle named `QuietVinyl Prepare` defaults to the preparation skin and can
start the separately bundled sync observer for the current profile. The observer
and player-format generators belong to the companion repository. Normal Mixxx
profiles and the Pi playback application retain their existing startup paths.
The companion's preparation guide records native UI and conversion tests.

Popup screen bounds now account for negative display origins; geometry tests
live in the companion repository. Physical multi-display focus/action testing
remains necessary. The macOS bundle installer also quotes application names
containing spaces.

## Touch browser navigation

On the Pi, a two-second hold on a playback deck number opens the companion's
independent Recovery window through its separately supervised launcher. The
owner-only recovery socket reports DVS controls and permits a normal saved-state
exit after the recovery window's deliberate restart hold. It exposes no TCP
listener. The companion owns diagnostics, process supervision and desktop access.

Leaving Waveforms Browse or transferring it to the other deck releases a playing
captured song into Relative DVS. This preserves transport continuity instead of
forcing an Absolute resync to a record that kept advancing during capture.
Normal track loading retains its stopped-to-Absolute handoff, and Minimal
browsing does not capture transport. A fresh needle drop can restore Absolute.

The browser header includes 56-pixel-high Back and Close buttons. They use the
same actions as header swipes: Back returns one level and closes at Music;
Close returns to playback. The buttons move with the browser between fixed
deck windows. Header gestures remain available, and song loading still requires
an explicit stationary hold or controller selection.

## Preparation song lists

The separately named QuietVinyl Prepare app initializes each track model once
with Cover Art, Preview, Title, Album, Artist, Duration, BPM and Key columns.
It starts with 48-pixel rows; ordinary saved header state and Library row-height
preferences retain later user adjustments. The preset does not change playlist
ordering or audio metadata. Cover art fits inside the cell with its full aspect
ratio, centered and uncropped even when the row is shorter than the column.

## Collective Inbox arrivals

When preparation is enabled and the current profile opts into
`inbox_receive_enabled`, Mixxx processes verified local `Inbox/requests` files
on its main thread. It resolves canonical recordings through the normal track
collection manager, preserves embedded tags and existing preparation, and adds
each recording once to `Inbox / New arrivals`. A durable local acknowledgement
lets the companion distinguish a queued arrival from a completed native import.
The native bridge accepts only bounded requests for this profile's registered
collective and canonical hashed Music paths with matching catalogue/size.
The companion verifies audio SHA-256 before queueing. It never writes the active
Mixxx SQLite database externally.

Verification: the companion's 12 Inbox tests and all 40 existing setup/sync tests
pass. A separate sandboxed native profile accepted three requests for two
recordings, including an arrival while open and an exact duplicate, leaving two
tracks and two playlist entries. The production profile was not modified.
Remove the preparation Inbox helper/call to roll back native receiving; local
profiles, shared canonical audio and cloud arrival receipts remain readable.

Collective Mac preparation profiles may set `follow_system_output: true` in
`quietvinyl-sync.json`. Their Main output follows the macOS default device,
including changes while open. Preview joins Main when no separate headphone
output is configured. Ordinary Mixxx and Pi routing retain their existing setup.
