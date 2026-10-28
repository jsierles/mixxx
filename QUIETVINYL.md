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
