#!/usr/bin/env python3
"""GUI idle / input-lag screenshots under QEMU.

    python3 tools/gui_idle_shot.py
"""
from __future__ import print_function

import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ISO = os.path.join(ROOT, "build", "myos.iso")
IMG = os.path.join(ROOT, "build", "tazos.img")
SHOTS = os.path.join(ROOT, "build", "shots-idle")
SERIAL = os.path.join(SHOTS, "serial.log")
SCR_W, SCR_H = 1920, 1080


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
    except FileNotFoundError:
        return ""


def wait_serial(needle, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in serial_text():
            return True
        time.sleep(0.3)
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
    try:
        from PIL import Image
        p = os.path.join(SHOTS, name + ".ppm")
        Image.open(p).convert("RGB").save(os.path.join(SHOTS, name + ".png"))
    except Exception as exc:
        print("png convert failed for %s: %s" % (name, exc))


def shot(qemu, name, settle=0.8):
    time.sleep(settle)
    mon(qemu, "screendump %s/%s.ppm" % (SHOTS.replace("\\", "/"), name), 1.0)
    print("captured " + name)
    ppm_to_png(name)


def pin(qemu):
    for _ in range(24):
        mon(qemu, "mouse_move -100 -100", 0.02)


def move_to(qemu, tx, ty, mx=0, my=0):
    dx, dy = tx - mx, ty - my
    while dx or dy:
        sx = max(-100, min(100, dx))
        sy = max(-100, min(100, dy))
        mon(qemu, "mouse_move %d %d" % (sx, sy), 0.02)
        dx -= sx
        dy -= sy
    return tx, ty


def click(qemu, tx, ty, settle=0.5):
    pin(qemu)
    move_to(qemu, tx, ty)
    time.sleep(0.12)
    mon(qemu, "mouse_button 1", 0.08)
    mon(qemu, "mouse_button 0", 0.08)
    time.sleep(settle)


def drag(qemu, x0, y0, x1, y1):
    pin(qemu)
    move_to(qemu, x0, y0)
    time.sleep(0.15)
    mon(qemu, "mouse_button 1", 0.1)
    move_to(qemu, x1, y1, mx=x0, my=y0)
    time.sleep(0.15)
    mon(qemu, "mouse_button 0", 0.1)
    time.sleep(0.4)


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
    shot(qemu, "01-desktop")

    from PIL import Image
    img = Image.open(os.path.join(SHOTS, "01-desktop.ppm")).convert("RGB")
    COL_BAR = (30, 36, 48)
    run = 0
    for y in range(SCR_H - 1, 0, -1):
        if img.getpixel((1500, y)) == COL_BAR:
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
    shot(qemu, "02-start", 0.3)
    click(qemu, 80, menu_y + 8 + 2 * rh + rh // 2, 1.0)  # Task Manager
    time.sleep(6.0)  # 3 s panel period; the 2.56 s CPU% ring should be mostly idle
    shot(qemu, "03-tm-rest", 0.4)

    # Keys still reach the shell even when Task Manager is focused.
    type_line(qemu, "bg 3")
    wait_serial("started", timeout=15)
    # Raise Task Manager via its taskbar button (Terminal would cover it).
    start_w = taskbar_h + 14
    search_w = min(300, SCR_W // 4)
    btn_x = start_w + 6 + search_w + 8 + 200 + 2
    btn_w = 190
    tm_x = btn_x + 3 * (btn_w + 2) + btn_w // 2
    click(qemu, tm_x, SCR_H - taskbar_h // 2, 0.6)
    time.sleep(1.5)
    shot(qemu, "04-tm-workers", 0.4)

    # Drag File Explorer by its title bar (right-hand column).
    rw = (SCR_W * 43) // 100
    fx = SCR_W - rw - 12
    title_h = chrome_h + 12
    sh = 5 * (chrome_h + 5) + title_h + 24
    fy = 30 + sh + 14
    # File Explorer is pinned on the taskbar; raise it before dragging.
    pin_x = start_w + 6 + search_w + 8
    click(qemu, pin_x + 100, SCR_H - taskbar_h // 2, 0.5)
    shot(qemu, "05-before-drag", 0.3)
    drag(qemu, fx + 80, fy + title_h // 2, fx + 80 - 180, fy + title_h // 2 - 40)
    shot(qemu, "06-after-drag", 0.4)

    mon(qemu, "sendkey esc", 0.3)
    if not wait_serial("type 'help' for commands"):
        # already printed at boot; look for prompt after exit
        time.sleep(0.5)
    type_line(qemu, "echo still-here")
    if not wait_serial("still-here", timeout=15):
        fail("text-mode shell did not accept input after GUI exit", qemu)

    type_line(qemu, "prodcons")
    if not wait_serial("all items received", timeout=20):
        fail("prodcons did not finish", qemu)
    type_line(qemu, "deadlock ordered")
    if not wait_serial("they should complete", timeout=10):
        fail("deadlock ordered did not start", qemu)
    type_line(qemu, "preempt off")
    if not wait_serial("preemption OFF", timeout=10):
        fail("preempt off", qemu)
    type_line(qemu, "preempt on")
    if not wait_serial("preemption on", timeout=10):
        fail("preempt on", qemu)
    type_line(qemu, "spawn 2")
    if not wait_serial("done.", timeout=40):
        fail("spawn did not finish", qemu)

    mon(qemu, "quit")
    qemu.wait(timeout=10)
    print("idle shots in " + SHOTS)


if __name__ == "__main__":
    main()
