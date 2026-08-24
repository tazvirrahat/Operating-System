#!/usr/bin/env python3
"""Kernel Lab sequential vs overlapping threads screenshots.

    python3 tools/threads_shot.py
"""
from __future__ import print_function

import os
import struct
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ISO = os.path.join(ROOT, "build", "myos.iso")
IMG = os.path.join(ROOT, "build", "tazos.img")
SHOTS = os.path.join(ROOT, "build", "shots-threads")
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


def chrome_from_desktop():
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
    return chrome_h, taskbar_h


def lab_geometry(chrome_h, taskbar_h):
    title_h = chrome_h + 12
    border = 2
    dw = (SCR_W * 62) // 100
    dh = ((SCR_H - taskbar_h) * 82) // 100
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
    return {
        "cx": cx, "cy": cy, "row_h": row_h, "list_w": list_w,
        "pane_x": pane_x, "btn_y": btn_y, "btn_h": btn_h,
        "win_x": win_x, "win_y": win_y, "dw": dw, "dh": dh,
    }


def click_row(qemu, geo, idx):
    x = geo["cx"] + 40
    y = geo["cy"] + idx * geo["row_h"] + geo["row_h"] // 2
    print("click row %d at %d,%d" % (idx, x, y))
    click(qemu, x, y, 0.4)


def click_run(qemu, geo):
    x = geo["pane_x"] + 70
    y = geo["btn_y"] + geo["btn_h"] // 2
    print("click run at %d,%d" % (x, y))
    click(qemu, x, y, 0.3)


def run_pair(qemu, geo, seq_needle, ov_needle):
    # 5 = Threads, sequential; 6 = Threads, overlapping (after Scheduler rows)
    click_row(qemu, geo, 5)
    click_run(qemu, geo)
    if not wait_serial(seq_needle, timeout=30):
        return False
    click_row(qemu, geo, 6)
    click_run(qemu, geo)
    if not wait_serial(ov_needle, timeout=30):
        return False
    return True


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

    chrome_h, taskbar_h = chrome_from_desktop()
    rh = chrome_h + 16
    menu_h = 8 * rh + 16
    menu_y = SCR_H - taskbar_h - menu_h

    click(qemu, 20, SCR_H - taskbar_h // 2, 0.4)
    click(qemu, 80, menu_y + 8 + 4 * rh + rh // 2, 1.2)

    geo = lab_geometry(chrome_h, taskbar_h)
    print("lab", geo)

    before = serial_text()
    if not run_pair(qemu, geo, "threads sequential:", "threads overlapping:"):
        fail("first pair did not finish", qemu)
    shot(qemu, "threads-run1", 0.8)

    # Second pair: needles must be new occurrences. Append a marker by
    # matching a second copy of the overlapping line after the first pair.
    n_ov = serial_text().count("threads overlapping:")
    click_row(qemu, geo, 5)
    click_run(qemu, geo)
    t0 = time.time()
    while time.time() - t0 < 30:
        if serial_text().count("threads sequential:") >= 2:
            break
        time.sleep(0.2)
    else:
        fail("second sequential did not finish", qemu)

    click_row(qemu, geo, 6)
    click_run(qemu, geo)
    t0 = time.time()
    while time.time() - t0 < 30:
        if serial_text().count("threads overlapping:") > n_ov:
            break
        time.sleep(0.2)
    else:
        fail("second overlapping did not finish", qemu)

    shot(qemu, "threads-run2", 0.8)

    mon(qemu, "quit")
    qemu.wait(timeout=10)
    print("thread shots in " + SHOTS)
    print(serial_text().split("graphical mode")[-1][-800:])


if __name__ == "__main__":
    main()
