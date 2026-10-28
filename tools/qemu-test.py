#!/usr/bin/env python3
"""
Headless QEMU driver for testing ZenithOS: boots the ISO, captures the
serial log, takes screenshots and can inject keyboard/mouse input via QMP.

usage: qemu-test.py [--iso build/zenithos.iso] [--smp 4] [--mem 2G]
                    [--script steps.txt] [--out build/test] [--time 20]

A script is a list of steps, one per line:
    wait <seconds>
    shot <name>                 screenshot -> <out>/<name>.png
    key <qcode> [qcode...]      press a key combo (e.g. "key ctrl alt t")
    type <text>                 type ASCII text
    move <x> <y>                absolute mouse move (needs vmmouse/tablet)
    click [left|right] [x y]    click, optionally after moving
    drag <x0> <y0> <x1> <y1>    left-drag
    wheel <n>                   scroll
    link <netdev> on|off        plug/unplug a network cable (netdevs are n0, n1, ...)
    devdel <id>                 hot-unplug a device (e.g. --nic usb-net:id=u0)
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

KEYMAP = {
    ' ': 'spc', '\n': 'ret', '\t': 'tab', '-': 'minus', '=': 'equal', '.': 'dot',
    ',': 'comma', '/': 'slash', ';': 'semicolon', "'": 'apostrophe', '[': 'bracket_left',
    ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent',
}
SHIFTED = {
    '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
    '(': '9', ')': '0', '_': 'minus', '+': 'equal', ':': 'semicolon', '"': 'apostrophe',
    '<': 'comma', '>': 'dot', '?': 'slash', '{': 'bracket_left', '}': 'bracket_right',
    '|': 'backslash', '~': 'grave_accent',
}


# German layout: character -> (qcode, modifiers)
DE = {
    'z': ('y', []), 'y': ('z', []), 'Z': ('y', ['shift']), 'Y': ('z', ['shift']),
    '-': ('slash', []), '_': ('slash', ['shift']), '/': ('7', ['shift']), ':': ('dot', ['shift']),
    ';': ('comma', ['shift']), '"': ('2', ['shift']), '=': ('0', ['shift']), '(': ('8', ['shift']),
    ')': ('9', ['shift']), '<': ('less', []), '>': ('less', ['shift']), '|': ('less', ['alt_r']),
    '+': ('bracket_right', []), '*': ('bracket_right', ['shift']), '#': ('backslash', []),
    "'": ('backslash', ['shift']), '?': ('minus', ['shift']), '!': ('1', ['shift']), '&': ('6', ['shift']),
    '%': ('5', ['shift']), '$': ('4', ['shift']), '{': ('7', ['alt_r']), '}': ('0', ['alt_r']),
    '[': ('8', ['alt_r']), ']': ('9', ['alt_r']), '\\': ('minus', ['alt_r']), '@': ('q', ['alt_r']),
    '~': ('bracket_right', ['alt_r']), '^': ('grave_accent', []),
}


class QMP:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile('rw')
        self.f.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, command, **args):
        self.f.write(json.dumps({'execute': command, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return None
            msg = json.loads(line)
            if 'return' in msg or 'error' in msg:
                return msg

    def keys(self, qcodes, hold=0.05):
        evs = [{'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': k}}} for k in qcodes]
        self.cmd('input-send-event', events=evs)
        time.sleep(hold)
        evs = [{'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': k}}}
               for k in reversed(qcodes)]
        self.cmd('input-send-event', events=evs)
        time.sleep(0.03)

    layout = 'us'          # what the guest's keyboard layout is; steps can switch it with 'layout de'

    def type(self, text):
        for ch in text:
            if self.layout == 'de' and ch in DE:
                k, mods = DE[ch]
                self.keys(mods + [k])
            elif ch.isupper():
                self.keys(['shift', ch.lower()])
            elif ch in SHIFTED:
                self.keys(['shift', SHIFTED[ch]])
            elif ch in KEYMAP:
                self.keys([KEYMAP[ch]])
            else:
                self.keys([ch])

    def move(self, x, y, w, h):
        ax = int(x * 32767 / (w - 1))
        ay = int(y * 32767 / (h - 1))
        self.cmd('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': 'x', 'value': ax}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': ay}}])
        time.sleep(0.05)

    def button(self, btn, down):
        self.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': btn}}])
        time.sleep(0.05)

    def shot(self, path):
        ppm = path + '.ppm'
        self.cmd('screendump', filename=ppm)
        time.sleep(0.3)
        try:
            from PIL import Image
            Image.open(ppm).save(path)
            os.remove(ppm)
        except Exception as e:  # noqa
            print('screenshot conversion failed:', e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', default='build/zenithos.iso')
    ap.add_argument('--smp', default='4')
    ap.add_argument('--mem', default='2G')
    ap.add_argument('--script')
    ap.add_argument('--out', default='build/test')
    ap.add_argument('--time', type=float, default=15)
    ap.add_argument('--res', default='1280x800')
    ap.add_argument('--extra', default='')
    ap.add_argument('--disk', help='raw disk image attached as a SATA drive')
    ap.add_argument('--nic', default='e1000',
                    help='comma separated network adapter models, each on its own user-mode network '
                         '(e.g. rtl8139,pcnet or usb-net); "none" for no adapter')
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    qmp_path = os.path.join(args.out, 'qmp.sock')
    if os.path.exists(qmp_path):
        os.remove(qmp_path)
    vars_path = os.path.join(args.out, 'vars.fd')
    shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', vars_path)
    serial_log = os.path.join(args.out, 'serial.log')
    cmd = ['qemu-system-x86_64', '-machine', 'q35', '-m', args.mem, '-smp', args.smp,
           '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
           '-drive', f'if=pflash,format=raw,file={vars_path}',
           '-display', 'none', '-serial', f'file:{serial_log}',
           '-qmp', f'unix:{qmp_path},server,nowait',
           '-accel', 'tcg,thread=multi']
    nics = [n for n in args.nic.split(',') if n and n != 'none']
    if any(n.startswith('usb-net') for n in nics):
        cmd += ['-device', 'qemu-xhci,id=xhci']
    for i, n in enumerate(nics):
        model, _, opts = n.partition(':')
        cmd += ['-netdev', f'user,id=n{i}', '-device', f'{model},netdev=n{i}' + (',' + opts.replace(':', ',') if opts else '')]
    if args.iso != 'none':
        cmd += ['-cdrom', args.iso]
    if args.disk:
        cmd += ['-drive', f'file={args.disk},format=raw,if=none,id=hd0', '-device', 'ide-hd,drive=hd0,bus=ide.0']
    if args.extra:
        cmd += args.extra.split()
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(0.5)
    qmp = QMP(qmp_path)
    w, h = map(int, args.res.split('x'))
    try:
        steps = open(args.script).read().splitlines() if args.script else [f'wait {args.time}', 'shot final']
        for st in steps:
            st = st.strip()
            if not st or st.startswith('#'):
                continue
            if os.environ.get('QT_VERBOSE'):
                print('step:', st, file=sys.stderr, flush=True)
            op, *rest = st.split(maxsplit=1)
            a = rest[0].split() if rest else []
            if op == 'wait':
                time.sleep(float(a[0]))
            elif op == 'shot':
                qmp.shot(os.path.join(args.out, a[0] + '.png'))
            elif op == 'key':
                qmp.keys(a)
            elif op == 'layout':
                qmp.layout = a[0]
            elif op == 'type':
                qmp.type(rest[0] if rest else '')
            elif op == 'move':
                qmp.move(int(a[0]), int(a[1]), w, h)
            elif op == 'click':
                btn = 'left'
                if a and a[0] in ('left', 'right', 'middle'):
                    btn = a.pop(0)
                if len(a) >= 2:
                    qmp.move(int(a[0]), int(a[1]), w, h)
                qmp.button(btn, True)
                qmp.button(btn, False)
            elif op == 'dclick':
                qmp.move(int(a[0]), int(a[1]), w, h)
                for _ in range(2):
                    qmp.button('left', True)
                    qmp.button('left', False)
            elif op == 'drag':
                x0, y0, x1, y1 = map(int, a)
                qmp.move(x0, y0, w, h)
                qmp.button('left', True)
                for i in range(1, 9):
                    qmp.move(x0 + (x1 - x0) * i // 8, y0 + (y1 - y0) * i // 8, w, h)
                qmp.button('left', False)
            elif op == 'link':
                qmp.cmd('set_link', name=a[0], up=a[1] == 'on')
            elif op == 'devdel':
                qmp.cmd('device_del', id=a[0])
            elif op == 'wheel':
                n = int(a[0])
                btn = 'wheel-up' if n > 0 else 'wheel-down'
                for _ in range(abs(n)):
                    qmp.button(btn, True)
                    qmp.button(btn, False)
            else:
                print('unknown step', st)
    finally:
        qmp.cmd('quit')
        try:
            q.wait(5)
        except subprocess.TimeoutExpired:
            q.kill()
    print(open(serial_log, errors='replace').read())


if __name__ == '__main__':
    main()
