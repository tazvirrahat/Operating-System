#!/usr/bin/env python3
"""Preempt hog/share + till.log file-race screenshots.

    python3 tools/practical_shot.py
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
SHOTS = os.path.join(ROOT, "build", "shots-practical")
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
        elif ch == ".":
            mon(qemu, "sendkey dot", 0.08)
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


def shot(qemu, name, settle=0.6):
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

    click(qemu, 20, SCR_H - taskbar_h // 2, 0.4)
    click(qemu, 80, menu_y + 8 + 4 * rh + rh // 2, 1.2)

    title_h = chrome_h + 12
    border = 2
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

    def click_run():
        click(qemu, pane_x + 70, btn_y + btn_h // 2, 0.3)

    def start_item(idx, settle=1.0):
        click(qemu, 20, SCR_H - taskbar_h // 2, 0.4)
        click(qemu, 80, menu_y + 8 + idx * rh + rh // 2, settle)

    def click_files_pin():
        start_w = taskbar_h + 14
        search_w = 300
        if search_w > SCR_W // 4:
            search_w = SCR_W // 4
        pin_x = start_w + 6 + search_w + 8
        click(qemu, pin_x + 100, SCR_H - taskbar_h // 2, 0.9)

    def explorer_row(row):
        # File Explorer: x = SCR_W - rw - 12, y = 30 + sh + 14
        rw = (SCR_W * 43) // 100
        sh = 5 * (chrome_h + 5) + title_h + 24
        fx = SCR_W - rw - 12
        fy = 30 + sh + 14
        list_y = (fy + title_h + border + 8) + (chrome_h + 14) + 8 + chrome_h + 8
        erow_h = chrome_h + 8
        row_x = fx + border + 8 + 40
        row_y = list_y + row * erow_h + erow_h // 2
        click(qemu, row_x, row_y, 0.4)
        return row_x, row_y

    def dblclick_at(tx, ty):
        pin(qemu)
        move_to(qemu, tx, ty)
        time.sleep(0.1)
        mon(qemu, "mouse_button 1", 0.04)
        mon(qemu, "mouse_button 0", 0.04)
        mon(qemu, "mouse_button 1", 0.04)
        mon(qemu, "mouse_button 0", 0.04)
        time.sleep(0.8)

    def wait_till_line(before, timeout=20):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if serial_text().count("till.log:") > before:
                return True
            time.sleep(0.2)
        return False

    click(qemu, 80, 140, 0.5)  # Terminal
    n0 = serial_text().count("till.log:")
    type_line(qemu, "filerace off")
    if not wait_till_line(n0):
        fail("unlocked file race did not finish", qemu)
    type_line(qemu, "cat till.log")
    time.sleep(0.6)
    shot(qemu, "filerace-explorer-torn", 0.6)
    click_files_pin()
    rx, ry = explorer_row(2)
    shot(qemu, "filerace-torn", 0.8)

    click(qemu, 80, 140, 0.5)  # Terminal
    n1 = serial_text().count("till.log:")
    type_line(qemu, "filerace on")
    if not wait_till_line(n1):
        fail("locked file race did not finish", qemu)
    type_line(qemu, "cat till.log")
    time.sleep(0.6)
    shot(qemu, "filerace-lab-clean", 0.6)

    click_files_pin()
    rx, ry = explorer_row(2)
    shot(qemu, "filerace-explorer-clean", 0.6)
    dblclick_at(rx, ry)
    shot(qemu, "filerace-clean", 0.8)

    copy_docs("filerace-explorer-torn", "filerace-torn.png")
    copy_docs("filerace-clean", "filerace-clean.png")

    mon(qemu, "quit")
    qemu.wait(timeout=10)
    print("practical shots in " + SHOTS)
    print(serial_text().split("graphical mode")[-1][-1200:])


if __name__ == "__main__":
    main()
