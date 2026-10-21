# mod-autofish (AzerothCore)

Server-side, hands-free fishing. Cast Fishing once and the server hooks every
splash, puts the catch in your bags, re-applies lures, opens clams and recasts
until you move.

## Install
1. Copy the `mod-autofish` folder into `azerothcore-wotlk/modules/`.
2. Re-run CMake and rebuild the worldserver (same as any module).
3. Copy `mod_autofish.conf.dist` from your build's `configs/modules/` folder
   to `mod_autofish.conf` (the build copies the .dist there) and edit if you like.
4. Restart the worldserver. `.reload config` picks up later config changes.

## Use
- Equip a pole, face water, cast Fishing once. Auto-fishing starts.
- **Stop:** move, turn, jump, or type `.autofish off`.
- `.autofish on` – start (also casts for you) and re-enable start-on-cast
- `.autofish off` – stop, and don't auto-start on your casts until `.autofish on`
- `.autofish status` – running / catches / casts

It stops by itself on: combat, death, mounting, bags full, no pole, 3 failed
casts in a row (not facing water), or `AutoFish.MaxMinutes`.

Only real players are affected: playerbots (random bots and altbots) are
never taken over, so their own fishing keeps working.

Loot goes through the normal loot code: skill-ups, junk rolls, fishing pools,
quest items and BoP all work as if you clicked.

## Using it alongside the AutoFish client addon
Not needed. If you keep the addon, its auto-loot may print "Already looted"
now and then, because the server empties the loot window first; `/af loot`
turns that off.
