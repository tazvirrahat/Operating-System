#!/usr/bin/env python3
"""Hog vs Notepad: freeze off, type on, Task Manager ticks.

    python3 tools/hog_notepad_shot.py
"""
from __future__ import print_function

import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ISO = os.path.join(ROOT, "build", "myos.iso")
IMG = os.path.join(ROOT, "build", "tazos.img")
SHOTS = os.path.join(ROOT, "build", "shots-hog")
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
        else:
            mon(qemu, "sendkey " + ch, 0.08)
    mon(qemu, "sendkey ret", 0.25)


def type_keys(qemu, s):
    for ch in s:
        if ch == " ":
            mon(qemu, "sendkey spc", 0.05)
        else:
            mon(qemu, "sendkey " + ch, 0.05)


def wait_count(needle, before, timeout=12):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if serial_text().count(needle) > before:
            return True
        time.sleep(0.15)
    return False


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


def shot(qemu, name, settle=0.5):
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


def copy_docs(name, dest_name):
    src = os.path.join(SHOTS, name + ".png")
    dst = os.path.join(ROOT, "docs", "images", dest_name)
    if os.path.isfile(src):
        shutil.copyfile(src, dst)
        print("copied " + dst)


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
    rh = chrome_h + 16
    menu_h = 8 * rh + 16
    menu_y = SCR_H - taskbar_h - menu_h
    title_h = chrome_h + 12
    border = 2

    def start_item(idx, settle=1.0):
        click(qemu, 20, SCR_H - taskbar_h // 2, 0.4)
        click(qemu, 80, menu_y + 8 + idx * rh + rh // 2, settle)

    dw = (SCR_W * 62) // 100
    win_x = (SCR_W - dw) // 2
    win_y = 36
    cx = win_x + border + 8
    cy = win_y + title_h + border + 8
    cw = dw - 2 * (border + 8)
    list_w = (cw * 32) // 100
    if list_w < 220:
        list_w = 220
    row_h = chrome_h + 8
    pane_x = cx + list_w + 14
    desc_y = cy + chrome_h + 14
    btn_y = desc_y + 4 * (chrome_h + 4) + 8
    btn_h = chrome_h + 14

    def click_row(idx):
        click(qemu, cx + 40, cy + idx * row_h + row_h // 2, 0.4)

    def click_btn(which):
        # 0 = sharing OFF, 1 = sharing ON
        x = pane_x + 80 if which == 0 else pane_x + 320
        click(qemu, x, btn_y + btn_h // 2, 0.3)

    npx, npy = 90, 80
    np_text_x = npx + 80
    np_text_y = npy + title_h + (chrome_h + 12) + 40

    # TM under Notepad under Kernel Lab. Click x=170 misses Kernel Lab
    # (starts at ~365) and hits Notepad, not Task Manager.
    start_item(2, 0.8)   # Task Manager
    start_item(3, 0.8)   # Notepad
    click(qemu, np_text_x, np_text_y, 0.3)
    shot(qemu, "01-notepad-ready")

    start_item(4, 1.0)   # Kernel Lab
    click_row(3)         # Hog vs Notepad

    # --- sharing ON, Task Manager: hog ticks climbing ---
    n300 = serial_text().count("after 300 ticks")
    click_btn(1)
    if not wait_serial("sharing ON:", timeout=10):
        fail("sharing-on hog did not start", qemu)
    # Dump immediately: Kernel Lab stays up, hog is running, Ticks climb.
    shot(qemu, "hog-tm", 0.05)
    start_item(2, 0.8)
    shot(qemu, "hog-tm-raised", 0.1)
    if not wait_count("after 300 ticks", n300, 8):
        fail("sharing-on hog (TM) did not exit", qemu)
    time.sleep(0.3)

    # --- sharing ON, type in Notepad while the hog runs ---
    start_item(4, 0.7)
    click_row(3)
    n300 = serial_text().count("after 300 ticks")
    click_btn(1)
    if not wait_serial("sharing ON:", timeout=10):
        fail("sharing-on hog 2 did not start", qemu)
    start_item(3, 0.6)
    click(qemu, np_text_x, np_text_y, 0.12)
    type_keys(qemu, "hello")
    shot(qemu, "hog-notepad-live", 0.1)
    if not wait_count("after 300 ticks", n300, 8):
        fail("sharing-on hog (Notepad) did not exit", qemu)
    time.sleep(0.3)

    # --- sharing OFF: desktop freezes ~3 s, then recovers ---
    start_item(4, 0.7)
    click_row(3)
    n300 = serial_text().count("after 300 ticks")
    click_btn(0)
    if not wait_serial("sharing OFF:", timeout=10):
        fail("sharing-off hog did not start", qemu)
    t_off = time.time()
    shot(qemu, "hog-frozen", 0.1)
    if not wait_count("after 300 ticks", n300, 8):
        fail("sharing-off hog did not exit", qemu)
    frozen_s = time.time() - t_off
    print("sharing-off hog recovered after %.1fs" % frozen_s)
    shot(qemu, "hog-recovered", 0.4)
    if "after 300 ticks (sharing OFF)" not in serial_text():
        fail("sharing-off hog did not log a 300-tick run", qemu)
    if frozen_s < 2.0:
        print("WARN: freeze was only %.1fs wall (QEMU may run fast)" % frozen_s)

    copy_docs("hog-tm-raised", "hog-tm.png")
    copy_docs("hog-notepad-live", "hog-notepad-live.png")

    text = serial_text()
    if "sharing ON" not in text or "sharing OFF" not in text:
        fail("serial missing hog start lines", qemu)

    mon(qemu, "quit")
    qemu.wait(timeout=10)
    print("hog shots in " + SHOTS)
    print(text.split("graphical mode")[-1][-1500:])


if __name__ == "__main__":
    main()
