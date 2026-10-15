#!/usr/bin/env python3
"""Steers a regress run toward the game's objective marker for a while: closed-loop pad input.

    python3 tools/regress/follow.py RUNDIR SECONDS [--fight SECONDS] [--shoot PIXELS] [--keep N]

tools/regress.sh runs it for a schedule's `follow SECONDS [options]` line, with the run's folder.
A fixed schedule replays a route by the clock, and a route through a level drifts off it within a
minute: an enemy that stands in the way a second longer, a door that opens later. This one looks
at every frame instead: it asks the presenter for a dump (RUNDIR/shot, which the presenter removes
once it took it), finds the objective marker in it and writes the pad file the way regress.sh
does, swapped in by rename.

The marker is found by its colour, the yellow Call of Duty draws the objective marker, its label
("Follow", "Enter", "Breach") and the off-screen arrows in. On screen, the pad walks forward
turning toward it; an arrow at a side or at the bottom turns in place; an arrow at the top (the
objective is ahead, out of sight above or behind a wall) walks on. When the frames stop changing
while walking, the player is blocked: the pad holds square (a door to breach, a car to enter) and
jumps; blocked again soon after, it backs off and strafes, first toward the side of the view with
more detail (a wall up close is a blur), then the other way, further each time, and past four of
those turns away and walks off to find another way round.

--fight N: every N seconds the pad aims (L2: the aim assist turns to the nearest enemy) and fires
a burst, and every second time taps square to reload; right after holding square at an obstacle
it aims and fires eight times, for the slowed time of a breached door. --shoot PIXELS is for an
objective drawn over an enemy ("Target" on a gunner the aim assist does not turn to): with the
marker on screen the pad aims down the sights at PIXELS (of 1080 lines) below the marker nearest
the crosshair, in pulses of the right stick sized to the distance, and fires once it is there
instead of walking to it, tapping square every fourth shot to reload; the marker shows through
rocks and wrecks, so after six shots that did not clear it the pad walks toward it for three
seconds to see it from closer. --keep N keeps every Nth
dump (default 10) as a record of the route; the others are deleted once read. Python's standard
library only.
"""
import argparse, os, re, sys, time
from pathlib import Path

# BGRA bytes: blue low, green and red high; marker_pixels drops the oranges of fire and lamps.
MARKER = re.compile(rb'[\x00-\x50][\x90-\xff][\xa0-\xff]\xff', re.S)
DUMP_NAME = re.compile(r'_(\d+)x(\d+)_bgra\.raw$')
# Fewer marker pixels than this are stray sparks, not the marker; an off-screen arrow has about a
# hundred, the marker with its label from a few hundred up.
NOISE_PIXELS = 20
ARROW_PIXELS = 300
# Mean change of the 64x36 thumbnail's green channel under which two frames show the same view.
STILL = 5
STILL_STEPS = 3


class Run:
    def __init__(self, folder, keep):
        self.folder, self.keep = Path(folder), keep
        self.c_dumps = 0

    def pad(self, tokens):
        new = self.folder / 'pad.new'
        new.write_text(tokens + '\n')
        os.replace(new, self.folder / 'pad')

    def dump(self):
        """The next presented frame as (bytes, width, height), or None when none came in 3 s."""
        shots = self.folder / 'shots'
        before = set(shots.glob('*.raw'))
        (self.folder / 'shot').touch()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            time.sleep(0.05)
            for path in set(shots.glob('*.raw')) - before:
                size = DUMP_NAME.search(path.name)
                if not size:
                    continue
                width, height = int(size[1]), int(size[2])
                # The presenter writes the dump after the trigger is gone: wait for all of it.
                if path.stat().st_size < width * height * 4:
                    continue
                data = path.read_bytes()
                self.c_dumps += 1
                if self.c_dumps % self.keep:
                    path.unlink()
                return data, width, height
        return None


def marker_pixels(data, width):
    for match in MARKER.finditer(data):
        offset = match.start()
        blue, green, red = data[offset], data[offset + 1], data[offset + 2]
        if offset % 4 == 0 and red - green <= 0x40 and green <= red + 8:
            yield (offset // 4) % width, offset // 4 // width


def thumbnail(data, width, height):
    return bytes(data[((y * height // 36) * width + x * width // 64) * 4 + 1]
                 for y in range(36) for x in range(64))


def open_side(thumb):
    """'lx=0' or 'lx=255': the side of the view with more detail. A wall at arm's length is a
    blur of magnified texture; the way past it shows edges."""
    def detail(x0, x1):
        return sum(abs(thumb[y * 64 + x + 1] - thumb[y * 64 + x])
                   for y in range(36) for x in range(x0, x1))
    return 'lx=0' if detail(0, 20) > detail(43, 63) else 'lx=255'


def steer(points, width, height):
    """Pad tokens toward the marker, and whether they walk (for telling a blocked player)."""
    cx_screen = width / 2
    if len(points) < NOISE_PIXELS:
        return 'ly=40', True
    points.sort()
    x, y = points[len(points) // 2]
    at_edge = x < width / 6 or x > width * 5 / 6 or y < height / 7 or y > height * 0.76
    if at_edge and len(points) < ARROW_PIXELS:
        if y < height / 7 and abs(x - cx_screen) < width / 3:
            return 'ly=0', True
        return 'rx=' + ('200' if x > cx_screen else '56'), False
    dx = (x - cx_screen) / cx_screen
    if abs(dx) < 0.06:
        return 'ly=0', True
    return f'ly=0 rx={round(128 + (50 + abs(dx) * 22) * (1 if dx > 0 else -1))}', True


def aim_at_marker(points, width, height, below):
    """(dx, dy) in pixels from the crosshair to the spot `below` lines under the marker nearest
    to it, or None without one on screen; the other markers' pixels are left out."""
    if len(points) < ARROW_PIXELS:
        return None
    cx, cy = width / 2, height / 2
    target_y = cy - below * height / 1080
    nx, ny = min(points, key=lambda p: (p[0] - cx) ** 2 + (p[1] - target_y) ** 2)
    radius = 80 * height / 1080
    near = [p for p in points if abs(p[0] - nx) < radius and abs(p[1] - ny) < radius]
    x, y = sum(p[0] for p in near) / len(near), sum(p[1] for p in near) / len(near)
    return x - cx, y + below * height / 1080 - cy


def pulse(run, held, stick, pixels, height):
    # About 0.9 ms of full stick per pixel down the sights: shorter overshoots less than it falls
    # short, and the next frame corrects it.
    run.pad(f'{held} {stick}')
    time.sleep(min(0.4, max(0.03, abs(pixels) * 1080 / height * 0.0009)))
    run.pad(held)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    parser.add_argument('run')
    parser.add_argument('seconds', type=float)
    parser.add_argument('--fight', type=float, default=0)
    parser.add_argument('--shoot', type=int, default=None, metavar='PIXELS')
    parser.add_argument('--keep', type=int, default=10)
    args = parser.parse_args()
    run = Run(args.run, max(1, args.keep))
    end = time.monotonic() + args.seconds
    last_fight = time.monotonic()
    c_bursts = c_still = c_unstick = c_round = c_shots = 0
    walk_until = 0.0
    last_block = 0.0
    previous = None
    while time.monotonic() < end:
        if args.fight and time.monotonic() - last_fight > args.fight:
            run.pad('l2')
            time.sleep(0.4)
            run.pad('l2 r2')
            time.sleep(0.5)
            c_bursts += 1
            if c_bursts % 2 == 0:
                # A tap reloads; holding square would pick up whatever weapon lies at the feet.
                run.pad('square')
                time.sleep(0.1)
            run.pad('')
            last_fight, previous = time.monotonic(), None
            continue
        frame = run.dump()
        if not frame:
            continue
        data, width, height = frame
        current = thumbnail(data, width, height)
        change = 255.0
        if previous:
            change = sum(abs(a - b) for a, b in zip(current, previous)) / len(current)
        previous = current
        points = list(marker_pixels(data, width))
        if args.shoot is not None and time.monotonic() >= walk_until:
            offset = aim_at_marker(points, width, height, args.shoot)
            if not offset:
                c_shots = 0
            elif c_shots >= 6:
                c_shots, walk_until = 0, time.monotonic() + 3
            else:
                dx, dy = offset
                if abs(dx) < 25 * height / 1080 and abs(dy) < 35 * height / 1080:
                    run.pad('l2 r2')
                    time.sleep(0.4)
                    c_bursts += 1
                    c_shots += 1
                    run.pad('l2')
                    if c_bursts % 4 == 0:
                        run.pad('square')
                        time.sleep(0.1)
                        run.pad('l2')
                elif abs(dx) >= 25 * height / 1080:
                    pulse(run, 'l2', 'rx=255' if dx > 0 else 'rx=0', dx, height)
                else:
                    pulse(run, 'l2', 'ry=255' if dy > 0 else 'ry=0', dy * 1.3, height)
                time.sleep(0.1)
                previous = None
                continue
        tokens, walks = steer(points, width, height)
        c_still = c_still + 1 if walks and change < STILL else 0
        if c_still >= STILL_STEPS:
            c_still, previous = 0, None
            c_unstick += 1
            # Blocks that come close together are one obstacle: go round it ever further.
            c_round = c_round + 1 if time.monotonic() - last_block < 25 else 0
            last_block = time.monotonic()
            if c_round == 0:
                # Held square uses what the player is stopped at: a door to breach, a car.
                run.pad('square')
                time.sleep(1.2)
                if args.fight:
                    # A breached door slows time for a few seconds with enemies (or a hostage
                    # taker) in the room: aim and fire at once, not at the next burst.
                    for _ in range(8):
                        run.pad('l2')
                        time.sleep(0.35)
                        run.pad('l2 r2')
                        time.sleep(0.15)
                    run.pad('')
                run.pad('cross ly=0')
                time.sleep(0.3)
                run.pad('ly=0')
                time.sleep(1.2)
            elif c_round <= 4:
                # Round it sideways, first toward the open side and then the other way, further
                # every second time: strafing keeps the marker in view, where turning loses it.
                run.pad('ly=255')
                time.sleep(0.5)
                side = open_side(current)
                if c_round % 2 == 0:
                    side = 'lx=255' if side == 'lx=0' else 'lx=0'
                run.pad(side)
                time.sleep(1.2 * ((c_round + 1) // 2))
                run.pad('ly=0')
                time.sleep(1)
            else:
                # A wall too long to strafe past: the way round starts elsewhere. Turn away
                # (about 120 degrees, alternating sides) and walk for a while.
                run.pad('rx=200' if c_round % 2 else 'rx=56')
                time.sleep(2.7)
                run.pad('ly=0')
                time.sleep(4)
            continue
        run.pad(tokens)
        if not walks:
            time.sleep(0.5)
            run.pad('')
    run.pad('')
    print(f'follow: {run.c_dumps} frames, {c_bursts} bursts, blocked {c_unstick} times',
          file=sys.stderr)


if __name__ == '__main__':
    main()
