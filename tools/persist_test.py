#!/usr/bin/env python3
"""Two-boot persistence proof under QEMU.

Boot 1 writes persist.txt through the shell; boot 2 reads it back after a
full reset. An optional --gui pass then opens the desktop on the same
image so File Explorer / Notepad can be eyeballed.

    python3 tools/persist_test.py
    python3 tools/persist_test.py --gui
"""
from __future__ import print_function

import os
import struct
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ISO = os.path.join(ROOT, "build", "myos.iso")
IMG = os.path.join(ROOT, "build", "persist.img")
SHOTS = os.path.join(ROOT, "build", "shots-persist")
SERIAL = os.path.join(SHOTS, "serial.log")
MARKER = "hello-disk"
MB = 8

GUI = "--gui" in sys.argv
GUI_ONLY = "--gui-only" in sys.argv


def qemu_cmd(serial_path, extra=None):
    cmd = [
        "qemu-system-i386",
        "-cdrom", ISO,
        "-boot", "order=d",
        "-display", "none",
        "-no-reboot",
        "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % IMG,
        "-serial", "file:" + serial_path,
        "-monitor", "stdio",
    ]
    if extra:
        cmd.extend(extra)
    return cmd


def start_qemu(serial_path, extra=None):
    os.makedirs(os.path.dirname(serial_path), exist_ok=True)
    open(serial_path, "w").close()
    err = open(os.path.join(SHOTS, "qemu-stderr.log"), "a")
    return subprocess.Popen(
        qemu_cmd(serial_path, extra),
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=err,
        cwd=ROOT,
    )


def mon(qemu, cmd, wait=0.12):
    qemu.stdin.write((cmd + "\n").encode())
    qemu.stdin.flush()
    time.sleep(wait)


def serial_text(path=SERIAL):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def wait_serial(needle, path=SERIAL, timeout=120):
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


def boot_to_prompt(serial_path, extra=None):
    qemu = start_qemu(serial_path, extra)
    if not wait_serial("type 'help' for commands", serial_path, timeout=120):
        mon(qemu, "quit")
        qemu.wait(timeout=10)
        return None
    time.sleep(0.6)
    return qemu


def zero_image():
    os.makedirs(os.path.dirname(IMG), exist_ok=True)
    with open(IMG, "wb") as f:
        f.truncate(MB * 1024 * 1024)


def fail(msg, qemu=None):
    print("FAIL: " + msg)
    if qemu:
        try:
            mon(qemu, "quit")
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
    sys.exit(1)


def boot1():
    print("=== boot 1: format + write persist.txt ===")
    serial = os.path.join(SHOTS, "boot1.log")
    qemu = boot_to_prompt(serial)
    if qemu is None:
        fail("boot 1 did not reach the prompt")
    text = serial_text(serial)
    if "formatted disk" not in text and "loaded " not in text:
        fail("boot 1 did not report a disk filesystem", qemu)
    type_line(qemu, "write persist.txt hello-disk")
    if not wait_serial("wrote ", serial, timeout=30):
        fail("write did not complete", qemu)
    type_line(qemu, "cat persist.txt")
    if not wait_serial(MARKER, serial, timeout=20):
        fail("cat did not show the marker on boot 1", qemu)
    type_line(qemu, "disk")
    wait_serial("model", serial, timeout=10)
    mon(qemu, "quit")
    qemu.wait(timeout=10)
    print("boot 1 serial captured")
    return True


def boot2():
    print("=== boot 2: reload persist.txt after reset ===")
    serial = os.path.join(SHOTS, "boot2.log")
    qemu = boot_to_prompt(serial)
    if qemu is None:
        fail("boot 2 did not reach the prompt")
    text = serial_text(serial)
    if "loaded " not in text:
        fail("boot 2 did not load the filesystem from disk:\n" + text[-800:], qemu)
    type_line(qemu, "cat persist.txt")
    if not wait_serial(MARKER, serial, timeout=20):
        fail("persist.txt missing after reboot", qemu)
    type_line(qemu, "ls")
    wait_serial("persist.txt", serial, timeout=10)
    mon(qemu, "quit")
    qemu.wait(timeout=10)
    print("boot 2: persist.txt survived")
    return True


def boot_corrupt():
    print("=== boot 3: valid-looking but inconsistent superblock ===")
    with open(IMG, "r+b") as f:
        # Keep TAZ1 magic and version, set file_count to 99.
        f.seek(0)
        magic = f.read(8)
        if magic[:4] != b"TAZ1":
            fail("expected TAZ1 magic before corrupt test, got %r" % magic)
        f.seek(8)
        f.write(struct.pack("<I", 99))

    serial = os.path.join(SHOTS, "boot-corrupt.log")
    qemu = boot_to_prompt(serial)
    if qemu is None:
        fail("corrupt boot did not reach the prompt")
    text = serial_text(serial)
    if "formatted disk" not in text:
        fail("corrupt superblock should have been reformatted:\n" + text[-800:], qemu)
    if "0 failed" not in text:
        fail("self-test failed after reformatting a corrupt disk", qemu)
    mon(qemu, "quit")
    qemu.wait(timeout=10)
    print("boot 3: corrupt disk reformatted, tests passed")


def ppm_to_png(name):
    try:
        from PIL import Image
        p = os.path.join(SHOTS, name + ".ppm")
        Image.open(p).convert("RGB").save(os.path.join(SHOTS, name + ".png"))
    except Exception as exc:
        print("png convert failed for %s: %s" % (name, exc))


def gui_pass():
    print("=== GUI: File Explorer after reboot ===")
    serial = os.path.join(SHOTS, "gui.log")
    qemu = boot_to_prompt(serial, extra=["-vga", "std"])
    if qemu is None:
        fail("GUI boot did not reach the prompt")

    type_line(qemu, "gui")
    if not wait_serial("graphical mode", serial, timeout=60):
        fail("did not enter graphical mode", qemu)
    time.sleep(2.5)

    def shot(name, settle=0.8):
        time.sleep(settle)
        mon(qemu, "screendump %s/%s.ppm" % (SHOTS.replace("\\", "/"), name), 1.0)
        print("captured " + name)
        ppm_to_png(name)

    shot("01-desktop")

    # File Explorer is already open on the right. Raise it from Start so
    # it is focused, then double-click persist.txt (slot 2: readme, about,
    # persist) into Notepad.
    try:
        from PIL import Image
    except ImportError:
        print("PIL missing; skipping click sequence")
        mon(qemu, "quit")
        qemu.wait(timeout=10)
        return

    img = Image.open(os.path.join(SHOTS, "01-desktop.ppm")).convert("RGB")
    COL_BAR = (30, 36, 48)
    run = 0
    for y in range(1079, 0, -1):
        if img.getpixel((1500, y)) == COL_BAR:
            run += 1
        else:
            break
    taskbar_h = run + 1
    chrome_h = taskbar_h - 20
    print("taskbar %d px, chrome %d px" % (taskbar_h, chrome_h))

    def pin():
        for _ in range(24):
            mon(qemu, "mouse_move -100 -100", 0.03)

    def move_to(tx, ty, mx=0, my=0):
        dx, dy = tx - mx, ty - my
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            mon(qemu, "mouse_move %d %d" % (sx, sy), 0.03)
            dx -= sx
            dy -= sy
        return tx, ty

    def click(tx, ty, settle=0.5):
        pin()
        move_to(tx, ty)
        time.sleep(0.15)
        mon(qemu, "mouse_button 1", 0.05)
        mon(qemu, "mouse_button 0", 0.05)
        time.sleep(settle)

    def dblclick(tx, ty):
        """Two presses without re-pinning; the kernel's double-click window
        is 40 ticks (400 ms at 100 Hz)."""
        pin()
        move_to(tx, ty)
        time.sleep(0.1)
        mon(qemu, "mouse_button 1", 0.04)
        mon(qemu, "mouse_button 0", 0.04)
        mon(qemu, "mouse_button 1", 0.04)
        mon(qemu, "mouse_button 0", 0.04)
        time.sleep(0.8)

    rh = chrome_h + 16
    menu_h = 8 * rh + 16
    menu_y = 1080 - taskbar_h - menu_h
    click(20, 1080 - taskbar_h // 2, 0.4)          # Start
    shot("02-startmenu", 0.4)
    click(80, menu_y + 8 + 1 * rh + rh // 2, 0.8)  # File Explorer
    shot("03-explorer", 0.8)

    # Explorer is on the right: x ≈ 1920*0.57. Click row 2 (persist.txt).
    title_h = chrome_h + 12
    border = 2
    # Window geometry from gui.c: x = SCR_W - rw - 12, y = 30 + sh + 14
    rw = (1920 * 43) // 100
    sh = 5 * (chrome_h + 5) + title_h + 24
    fx = 1920 - rw - 12
    fy = 30 + sh + 14
    list_y = (fy + title_h + border + 8) + (chrome_h + 14) + 8 + chrome_h + 8
    row_h = chrome_h + 8
    row_x = fx + border + 8 + 40
    row2_y = list_y + 2 * row_h + row_h // 2
    dblclick(row_x, row2_y)
    shot("04-notepad-persist", 1.0)

    mon(qemu, "quit")
    qemu.wait(timeout=10)
    print("GUI shots in " + SHOTS)


def main():
    if GUI_ONLY:
        os.makedirs(SHOTS, exist_ok=True)
        if not os.path.isfile(IMG):
            sys.exit("missing persist.img — run without --gui-only first")
        gui_pass()
        return

    os.makedirs(SHOTS, exist_ok=True)
    if not os.path.isfile(ISO):
        sys.exit("missing %s — build the ISO first" % ISO)

    zero_image()
    boot1()
    boot2()
    boot_corrupt()
    print("=== persistence proof OK ===")
    print("serial: %s and %s" % (
        os.path.join(SHOTS, "boot1.log"),
        os.path.join(SHOTS, "boot2.log")))

    if GUI:
        # Recreate a clean persistent image with the marker for the GUI pass.
        zero_image()
        boot1()
        gui_pass()


if __name__ == "__main__":
    main()
