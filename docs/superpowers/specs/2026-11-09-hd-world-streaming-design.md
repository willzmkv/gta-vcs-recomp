# HD World streaming: safe extension design

## Problem and evidence

The detailed world is not a collection of independently drawable 50 x 50 m
blocks. VCS keeps a small camera-following window of streamed hexagonal cells
(125 x 108.25 world units) in level-owned area records and buffers. The low
detail map remains resident outside that window.

The experimental `vcs_world_hd.cpp` implementation currently makes two unsafe
assumptions:

1. It treats unrelated `CLevel` fields starting at `level + 0x25c` as twelve
   complete area records. Those offsets are part of the stream state, not an
   extension array of `0xa0`-byte area slots.
2. A host worker thread invokes the recompiled guest `RequestArea` routine
   directly with a synthetic CPU context while the game thread is also using
   the runtime and guest RAM. This bypasses the guest scheduler and races the
   stream queue.

It then splices area-owned sector lists into the current area's lists during
draw. The code does not establish ownership, compatible list-node layout,
deduplication, or cycle bounds; it also does not safely complete the equivalent
big-building splice. These are memory-corruption paths and account for the
intermittent hangs/crashes.

The separate extended object-LOD path in `vcs_distance.cpp` can choose a high
detail atomic only if the relevant model was requested by the sector streaming
scan. Extending an atomic LOD range alone cannot make props outside the
resident cell window appear.

## Options considered

1. Move the existing three-area window farther ahead of the camera. This is
   safe once routed through the original chooser, but it trades detail behind
   the player for detail ahead and does not increase the simultaneous radius.
2. Expand the guest level's real area-slot/buffer/queue ownership model. This
   can provide a genuine larger detailed radius, but requires establishing all
   allocation limits, request lifecycle, callbacks, and origin transforms in
   the original engine. It is the selected route for guest-side extension.
3. Decode and submit additional cells natively through the RSL renderer. This
   avoids guest-RAM limits but is a larger renderer feature and requires a
   host-side world-cell lifecycle. It remains the fallback if the guest layout
   proves fixed to the original window.

## Chosen staged design

1. Immediately put the unsafe HD-world experiment behind its default-off
   setting without mutating area records, guest lists, or calling guest code
   from a host thread. Preserve the original game path as the fallback.
2. Add bounded, game-thread-only instrumentation to the original request,
   read, completion, swap, and draw paths. The trace records the real slot,
   cell coordinates, piece count, buffer range, and list heads for each event.
3. Use the existing Ghidra project to trace `FUN_0023C820`, `FUN_002415B0`,
   `FUN_00241278`, `FUN_0023D740`, and `FUN_0023E748`; document confirmed
   structures and call ordering in `docs/game-internals.md`.
4. Build a deterministic regression that drives one high-speed route with
   view/detail sliders at default and maximum, checks for crashes/hangs, and
   captures area/prop streaming evidence.
5. Implement one proven expansion mechanism only after the trace establishes
   real spare slot/buffer capacity; all requests remain tail-called/scheduled
   on the game thread. If no capacity exists, leave guest streaming unchanged
   and pursue the native RSL route rather than fabricate slots.

## Acceptance criteria

- Default settings retain the original area streaming behaviour.
- Maximum draw/detail settings request the models for visible props early
  enough that their high-detail atomics are available when selected.
- No host thread enters a guest recompiled function or mutates guest world
  lists.
- The automated route completes repeatedly without watchdog, guest-branch, or
  memory-corruption errors; evidence and before/after measurements are kept in
  project documentation.
