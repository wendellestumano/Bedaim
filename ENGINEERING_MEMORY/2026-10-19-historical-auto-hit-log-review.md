# Historical AutoHit log review — 2026-10-19

## Scope and freshness

Reviewed `bedaim_diagnostics_total(20261019-095421).json`: 13,312 entries across 45 sessions, spanning 2026-10-17 18:22:50 UTC through 2026-10-19 09:52:52 UTC. This is the latest large total log currently available in Library, but it is not the newer test visible in `259256.mp4` (on-screen diagnostics save around 10:53). The phone's Remote Desktop session was offline and no post-video JSON was attached. Do not describe historical counts as counts from the new test.

## Historical aggregate

- `verified_local_player=vtable_getter_returned_null: slot=32`: 1,651
- `verified_local_player=resolution_failed`: 1,647
- Target selected: 299; no eligible target: 284
- Target snapshots fresh: 159; stale/missing: 167
- `native_attack_accepted`: 37; `native_attack_rejected`: 0
- `target_out_of_range`: 33; quarantine messages: 11
- Heuristic `useItem_slot=12, attack_slot=16, anchored_by_useItem_and_neighbor_slots`: 8
- 24 of 37 accepted entries had hurt-time telemetry, and all 24 showed no transition (0 to 0). Thirteen accepted entries had no hurt-time fields. A wrapper true return is not proof of damage.

## Last historical session

Session `1792403323314` had 645 entries (09:48:43–09:52:52 UTC): 115 slot-32-null reports, 114 local-player resolution failures, target selection flapping (44 selected vs 43 none), 2 fresh vs 3 stale snapshots, and two accepted AutoHit entries. Both were close range (1.065 and 1.353 blocks; range 3.0) with hurtTime 0 to 0. Later GameHooks logged `No validated public 3-argument attack route` and `Could not validate Player-owned GameMode` for SDK offset `0x9E8`. That points to route/object identity resolution as the main historical blocker, not CPS or reach.

The same session logged a public virtual call at slot 16 returning true with `internal_observer=not_reached`, then inferred attack slot 16 from neighboring `useItem` slot 12. The current branch's `publicAttackVirtualEntry()` explicitly forbids that inference and requires symbol/adapter argument-shuffle/control-flow validation or a guarded four-argument thunk. The text `anchored_by_useItem_and_neighbor_slots` is absent from current `GameHooks.cpp`, indicating a different/older native core or build. The historical export also says no-feedback quarantine pauses dispatch, while current `autohit.cpp` only warns and explicitly continues. The historical file therefore does not establish current-source behavior.

## Video review

The `259256.mp4` review shows Auto Hit disabled in the menu around 5 seconds; the following visible HP decrease overlaps nonzero LMB CPS, so it can be explained by manual attack input and does not prove AutoHit landed damage.

## Next decisive check

No C++ change was made based solely on this older export. Current source has fail-closed route identity validation and the HitboxExpander bounds guard. The decisive input is the fresh post-Build-894 `bedaim_diagnostics_auto.json` and/or `bedaim_diagnostics_total.json` from around 10:53, or reconnect Desktop Commander to read those exact files. Check for `validated public three-argument attack route`, `validated guarded four-argument attack thunk`, `four_argument_native_attack_validated`, or `native_attack_route_unavailable` before selecting another code change.