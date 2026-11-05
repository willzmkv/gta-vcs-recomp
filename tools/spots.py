#!/usr/bin/env python3
"""Reference spots for pictures and measurements (the player's choice, 2026-11-05): where the city's population is in
view - on the pavement, facing the street, never on a roof or at a wall.

    import spots
    spots.put(g, "police")          # the player there, the camera behind him looking along the street
    spots.check(g, "police")        # again before a picture / a measurement: the game may have moved him

The spots:
  police  Downtown, ten steps out from the police station's doors (the game's "you will be taken to the local police
          station" help shows at them, -657.4 762.3), facing the street and the busy pavement across it
  havana  Little Havana, on the pavement facing along the street (people far down it: crowds, distant walkers)

The camera is the rig's free camera (`campose`) 6 m behind the player and 2.5 m up, looking along his heading 6 degrees
down: the game's own camera does not turn with a teleport's heading. A save loaded by the rig puts the player back at
the save house some seconds after the load, later than a first check sees - so put() checks again and teleports again,
and callers check before each picture.
"""
import math
import time

# name: (x, y, heading in degrees - GTA's: 0 north (+y), 90 west)
SPOTS = {
    "police": (-655.5, 755.6, 196.0),
    "havana": (-979.9, -571.2, 7.0),
}


def put(g, name, settle=4.0, camera=True):
    """The player at the spot (teleported again while the game moves him away), the camera behind him; (x, y) where he
    stands."""
    x, y, heading = SPOTS[name]
    qx = qy = qz = 0.0
    for _ in range(8):
        _, (qx, qy, qz), _ = g.player()
        if math.hypot(qx - x, qy - y) < 20:
            break
        g.teleport(x, y, -100.0, heading=heading)
        time.sleep(settle)
    _, (qx, qy, qz), _ = g.player()
    if camera:
        yaw = (heading + 90.0) % 360.0  # campose's yaw: from +x toward +y
        ux, uy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        g.cmd("campose %f %f %f %f %f" % (qx - ux * 6.0, qy - uy * 6.0, qz + 2.5, yaw, -6.0))
    return qx, qy


def check(g, name):
    """put() again only when the player is no longer there (before a picture or a measurement)."""
    return put(g, name)


def wait_after_load(seconds=20.0):
    """The save house's warp after the rig loads a save: spots are taken after it."""
    time.sleep(seconds)
