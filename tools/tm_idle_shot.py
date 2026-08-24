#!/usr/bin/env python3
"""Task Manager idle freeze: shots with no input, then two clicks.

    python3 tools/tm_idle_shot.py [tag]
"""
from __future__ import print_function

import hashlib
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ISO = os.path.join(ROOT, "build", "myos.iso")
IMG = os.path.join(ROOT, "build", "tazos.img")
TAG = sys.argv[1] if len(sys.argv) > 1 else "run"
SHOTS = os.path.join(ROOT, "build", "shots-tm-" + TAG)
SERIAL = os.path.join(SHOTS, "serial.log")
SCR_W, SCR_H = 1920, 1080
COL_BAR = (30, 36, 48)


def qemu_cmd():
    cmd = [
        "qemu-system-i386", "-cdrom", ISO, "-boot", "order=d",
        "-display", "none", "-no-reboot", "-vga", "std",
        "-serial", "file:" + SERIAL, "-monitor", "stdio",
    ]
    if os.path.isfile(IMG):
        cmd.extend(["-drive",
                    "file=%s,format=raw,if=ide,index=0,media=disk" % IMG])
    return cmd


def start_qemu():
    os.makedirs(SHOTS, exist_ok=True)
    open(SERIAL, "w").close()
    err = open(os.path.join(SHOTS, "qemu-stderr.log"), "w")
    return subprocess.Popen(
        qemu_cmd(), stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=err, cwd=ROOT)


def mon(qemu, cmd, wait=0.12):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)


def serial_text():
    try:
        with open(SERIAL, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except IOError:
        return ""


def wait_serial(needle, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in serial_text():
            return True
        time.sleep(0.2)
    print("TIMEOUT waiting for: " + needle)
    return False


def type_line(qemu, s):
    for ch in s:
        if ch == " ":
            mon(qemu, "sendkey spc", 0.08)
        elif ch == "-":
            mon(qemu, "sendkey minus", 0.08)
        else:
            mon(qemu, "sendkey " + ch, 0.08)
    mon(qemu, "sendkey ret", 0.25)


def ppm_to_png(name):
    ppm = os.path.join(SHOTS, name + ".ppm")
    png = os.path.join(SHOTS, name + ".png")
    try:
        from PIL import Image
        Image.open(ppm).convert("RGB").save(png)
        return
    except Exception:
        pass
    try:
        subprocess.check_call(["pnmtopng", ppm], stdout=open(png, "wb"))
    except Exception as exc:
        print("png convert failed for %s: %s" % (name, exc))


def load_ppm(path):
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P6":
            raise ValueError("not P6")
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = [int(x) for x in line.split()]
        f.readline()
        data = f.read()
    return w, h, data


def pixel(data, w, x, y):
    i = (y * w + x) * 3
    return (data[i], data[i + 1], data[i + 2])


def region_hash(path, x0, y0, rw, rh):
    w, h, data = load_ppm(path)
    hsh = hashlib.md5()
    for y in range(y0, min(y0 + rh, h)):
        i0 = (y * w + x0) * 3
        hsh.update(data[i0:i0 + rw * 3])
    return hsh.hexdigest()


def shot(qemu, name, settle=0.4):
    time.sleep(settle)
    path = os.path.join(SHOTS, name + ".ppm").replace("\\", "/")
    mon(qemu, "screendump " + path, 1.0)
    print("captured " + name)
    ppm_to_png(name)


def pin(qemu):
    for _ in range(24):
        mon(qemu, "mouse_move -100 -100", 0.02)


def move_to(qemu, tx, ty):
    mx = my = 0
    dx, dy = tx - mx, ty - my
    while dx or dy:
        sx = max(-100, min(100, dx))
        sy = max(-100, min(100, dy))
        mon(qemu, "mouse_move %d %d" % (sx, sy), 0.02)
        dx -= sx
        dy -= sy


def click(qemu, tx, ty, settle=0.5):
    pin(qemu)
    move_to(qemu, tx, ty)
    time.sleep(0.12)
    mon(qemu, "mouse_button 1", 0.08)
    mon(qemu, "mouse_button 0", 0.08)
    time.sleep(settle)


def fail(msg, qemu):
    print("FAIL: " + msg)
    try:
        mon(qemu, "quit")
        qemu.wait(timeout=10)
    except Exception:
        qemu.kill()
    sys.exit(1)


def main():
    if not os.path.isfile(ISO):
        sys.exit("missing ISO")

    qemu = start_qemu()
    if not wait_serial("type 'help' for commands"):
        fail("no prompt", qemu)

    type_line(qemu, "gui")
    if not wait_serial("graphical mode", timeout=60):
        fail("no gui", qemu)
    time.sleep(2.0)
    shot(qemu, "00-desktop")

    w, h, data = load_ppm(os.path.join(SHOTS, "00-desktop.ppm"))
    run = 0
    for y in range(h - 1, 0, -1):
        if pixel(data, w, 1500, y) == COL_BAR:
            run += 1
        else:
            break
    taskbar_h = run + 1
    chrome_h = taskbar_h - 20
    print("taskbar %d chrome %d" % (taskbar_h, chrome_h))

    rh = chrome_h + 16
    menu_h = 8 * rh + 16
    menu_y = SCR_H - taskbar_h - menu_h

    click(qemu, 20, SCR_H - taskbar_h // 2, 0.4)
    click(qemu, 80, menu_y + 8 + 2 * rh + rh // 2, 1.2)  # Task Manager

    tmw = (SCR_W * 52) // 100
    tmh = ((SCR_H - taskbar_h) * 58) // 100
    tmx, tmy = 48, 56
    print("tm at %d,%d %dx%d" % (tmx, tmy, tmw, tmh))

    # Park the pointer off the window so it is not in the TM hash.
    pin(qemu)
    time.sleep(0.3)

    # No input: three frames a couple of seconds apart.
    shot(qemu, "01-idle-a", 0.3)
    time.sleep(2.0)
    shot(qemu, "02-idle-b", 0.3)
    time.sleep(2.5)
    shot(qemu, "03-idle-c", 0.3)

    ha = region_hash(os.path.join(SHOTS, "01-idle-a.ppm"), tmx, tmy, tmw, tmh)
    hb = region_hash(os.path.join(SHOTS, "02-idle-b.ppm"), tmx, tmy, tmw, tmh)
    hc = region_hash(os.path.join(SHOTS, "03-idle-c.ppm"), tmx, tmy, tmw, tmh)
    print("tm hash a %s" % ha)
    print("tm hash b %s" % hb)
    print("tm hash c %s" % hc)
    print("idle a==b %s  b==c %s  a==c %s" % (ha == hb, hb == hc, ha == hc))

    # Click two different rows (layout matches tm_layout in gui.c).
    title_h = chrome_h + 12
    row_h = chrome_h + 8
    btn_h = chrome_h + 14
    trace_h = chrome_h + 10
    list_y = tmy + title_h + 2 + 8 + btn_h + 6 + trace_h + 4 + chrome_h + 8
    click(qemu, tmx + 80, list_y + row_h // 2, 0.6)
    shot(qemu, "04-click-1", 0.3)
    click(qemu, tmx + 80, list_y + row_h + row_h // 2, 0.6)
    shot(qemu, "05-click-2", 0.3)

    h1 = region_hash(os.path.join(SHOTS, "04-click-1.ppm"), tmx, tmy, tmw, tmh)
    h2 = region_hash(os.path.join(SHOTS, "05-click-2.ppm"), tmx, tmy, tmw, tmh)
    print("click1 hash %s" % h1)
    print("click2 hash %s" % h2)
    print("clicks differ %s" % (h1 != h2))

    mon(qemu, "quit")
    qemu.wait(timeout=10)
    print("tm idle shots in " + SHOTS)


if __name__ == "__main__":
    main()
