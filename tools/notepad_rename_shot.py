#!/usr/bin/env python3
"""Notepad rename + persist screenshots under QEMU.

    python3 tools/notepad_rename_shot.py
"""
from __future__ import print_function

import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ISO = os.path.join(ROOT, "build", "myos.iso")
IMG = os.path.join(ROOT, "build", "rename.img")
SHOTS = os.path.join(ROOT, "build", "shots-rename")
SERIAL = os.path.join(SHOTS, "serial.log")
SCR_W, SCR_H = 1920, 1080
MB = 8


def qemu_cmd(serial_path):
    return [
        "qemu-system-i386", "-cdrom", ISO, "-boot", "order=d",
        "-display", "none", "-no-reboot", "-vga", "std",
        "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % IMG,
        "-serial", "file:" + serial_path, "-monitor", "stdio",
    ]


def start_qemu(serial_path):
    os.makedirs(os.path.dirname(serial_path), exist_ok=True)
    open(serial_path, "w").close()
    err = open(os.path.join(SHOTS, "qemu-stderr.log"), "a")
    return subprocess.Popen(
        qemu_cmd(serial_path), stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL, stderr=err, cwd=ROOT)


def mon(qemu, cmd, wait=0.12):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)


def serial_text(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def wait_serial(needle, path, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in serial_text(path):
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
        elif ch == ".":
            mon(qemu, "sendkey dot", 0.08)
        elif ch == "_":
            mon(qemu, "sendkey shift-minus", 0.08)
        else:
            mon(qemu, "sendkey " + ch, 0.08)
    mon(qemu, "sendkey ret", 0.25)


def type_keys(qemu, s):
    for ch in s:
        if ch == " ":
            mon(qemu, "sendkey spc", 0.07)
        elif ch == ".":
            mon(qemu, "sendkey dot", 0.07)
        elif ch == "-":
            mon(qemu, "sendkey minus", 0.07)
        else:
            mon(qemu, "sendkey " + ch, 0.07)


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


def boot_gui(serial_path):
    qemu = start_qemu(serial_path)
    if not wait_serial("type 'help' for commands", serial_path):
        fail("no prompt", qemu)
    type_line(qemu, "gui")
    if not wait_serial("graphical mode", serial_path, timeout=60):
        fail("no gui", qemu)
    time.sleep(2.0)
    return qemu


def measure_taskbar(ppm):
    from PIL import Image
    img = Image.open(ppm).convert("RGB")
    COL_BAR = (30, 36, 48)
    run = 0
    for y in range(SCR_H - 1, 0, -1):
        if img.getpixel((1500, y)) == COL_BAR:
            run += 1
        else:
            break
    taskbar_h = run + 1
    return taskbar_h, taskbar_h - 20


def main():
    os.makedirs(SHOTS, exist_ok=True)
    if not os.path.isfile(ISO):
        sys.exit("missing ISO")
    with open(IMG, "wb") as f:
        f.truncate(MB * 1024 * 1024)

    serial = os.path.join(SHOTS, "boot1.log")
    qemu = boot_gui(serial)
    shot(qemu, "01-desktop")

    taskbar_h, chrome_h = measure_taskbar(os.path.join(SHOTS, "01-desktop.ppm"))
    rh = chrome_h + 16
    menu_h = 8 * rh + 16
    menu_y = SCR_H - taskbar_h - menu_h
    title_h = chrome_h + 12

    click(qemu, 20, SCR_H - taskbar_h // 2, 0.4)
    click(qemu, 80, menu_y + 8 + 3 * rh + rh // 2, 1.0)  # Notepad
    shot(qemu, "02-notepad-empty", 0.5)

    # New button is the first in the notepad chrome bar.
    npx, npy = 90, 80
    click(qemu, npx + 40, npy + title_h + 16, 0.4)
    type_keys(qemu, "hello from notepad")
    shot(qemu, "03-typed", 0.4)

    # Filename field sits to the right of New/Open/Save.
    click(qemu, npx + 280, npy + title_h + 16, 0.4)
    for _ in range(16):
        mon(qemu, "sendkey backspace", 0.05)
    type_keys(qemu, "hello.txt")
    mon(qemu, "sendkey ret", 0.2)
    shot(qemu, "04-renamed", 0.4)

    # Save is the third button.
    click(qemu, npx + 180, npy + title_h + 16, 0.6)
    shot(qemu, "05-saved", 0.5)

    click(qemu, 20, SCR_H - taskbar_h // 2, 0.4)
    click(qemu, 80, menu_y + 8 + 1 * rh + rh // 2, 1.0)  # File Explorer
    shot(qemu, "06-explorer", 0.6)

    mon(qemu, "quit")
    qemu.wait(timeout=10)

    print("=== reboot, look for hello.txt ===")
    serial2 = os.path.join(SHOTS, "boot2.log")
    qemu = boot_gui(serial2)
    text = serial_text(serial2)
    if "loaded " not in text:
        fail("did not load disk on reboot", qemu)
    type_line(qemu, "cat hello.txt")
    # cat goes to the graphical terminal; also try after a moment
    time.sleep(0.8)
    shot(qemu, "07-after-reboot", 0.5)

    # File Explorer is already visible on the right.
    shot(qemu, "08-explorer-reboot", 0.6)

    mon(qemu, "quit")
    qemu.wait(timeout=10)

    if "hello.txt" not in serial_text(serial2) and "hello from notepad" not in serial_text(serial2):
        # GUI terminal cat may not hit serial; boot2 log should still have loaded files
        print("note: serial may not show GUI cat; inspect 08-explorer-reboot.png")
    print("rename shots in " + SHOTS)


if __name__ == "__main__":
    main()
