# ElytraMacing — first implementation

## Intended flow

ElytraMacing is a new controller module, not a wrapper around the previous Mace Swapper implementation.

1. It requires Aim Assist to be enabled and uses its fresh player-target snapshot and gliding-state query.
2. When gliding and farther than the configured engagement distance, it selects a firework rocket from the hotbar and triggers the configured Firework Key Mapper keymap at a bounded interval.
3. If falling and not gliding, it requests the configured Start Glide keymap. That keymap should contain the user's two-jump Elytra activation sequence. The controller checks the real gliding state before requesting the recovery boost.
4. When a player target is close, Aim Assist confirms the target in the crosshair, a mace is in the hotbar, AutoHit is enabled, and the exact `minecraft:mace` AutoHit profile is enabled, it selects the mace and triggers the Stop Gliding keymap. AutoHit remains responsible for attack input and its existing target/range checks.
5. The controller watches player-height changes during the attack window. A detected upward impulse requests re-gliding and then a rapid firework boost.
6. If downward motion is followed by several stable ticks while not gliding, it starts ground recovery: optionally suspend Aim Assist, trigger the look-up keymap, wait for the configured delay, trigger the double-jump keymap, and wait for confirmed gliding before boosting again.

## Key Mapper setup

Open **ElytraMacing Keymapper UIDs** in the Bedaim settings page and paste the UUIDs of these keymaps:

- **Start glide keymap**: a macro that taps Jump twice with a short release interval, to attempt to start Elytra gliding.
- **Firework use keymap**: activates/uses the selected hotbar item once. ElytraMacing selects a hotbar slot containing `firework_rocket` first.
- **Recovery look-up keymap**: a short upward camera swipe. During this action Aim Assist can be temporarily suspended so it does not immediately countersteer the swipe.
- **Stop gliding keymap**: a keymap that taps the user's chosen glide-stop/Space input. Its default UUID is the previously configured Unglide UID `e2225852-1df2-4232-bb26-606f11fc8745`; replace it if that keymap differs on the device.

Empty UIDs disable their associated actions. The controller will not guess keymap identifiers. A Key Mapper broadcast being accepted is not proof that the key sequence ran or that the Minecraft state changed; the runtime checks gliding and height changes separately.

## Requirements and safeguards

- Enable Aim Assist and AutoHit. Raise Aim Assist's target range enough to acquire the player during the rocket approach; the controller can only use the target Aim Assist currently exposes.
- Enable the `minecraft:mace` profile in AutoHit and keep the mace and firework rockets in the hotbar.
- Disable the older Mace Swapper while validating ElytraMacing, so two independent systems do not attempt to schedule the same mace attack.
- The controller only engages player targets, only stops gliding after its range/crosshair/item/profile gates pass, and bounds glide retries and rocket cadence.
- Slot restoration is conditional: on disable it restores the prior slot only if the currently selected slot is still the slot ElytraMacing last selected. If the user manually changes slots, that selection is preserved.
- Recovery is heuristic. Stationary ticks after a fall are used as a landing signal; they are not a native ground-contact API. Wind Burst is inferred from an upward position delta during the attack window; neither signal alone proves that the mace hit registered.

## Validation status

Host policy tests and Android ARM64 packaging are wired into CI. In-game behavior still needs to be verified on the exact Minecraft/LeviLauncher/Key Mapper setup, especially the configured keymap UUIDs, rocket consumption, glide start/stop behavior, and AutoHit's mace profile.
