#!/usr/bin/python2
# -*- coding: utf-8 -*-
#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2014  Marek Marczykowski-Górecki <marmarek@invisiblethingslab.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
#

from __future__ import absolute_import
import re
import sys
from subprocess import Popen, PIPE, check_call
from qubes.qubes import QubesVmCollection

# "LVDS connected 1024x768+0+0 (normal left inverted right) 304mm x 228mm"
REGEX_OUTPUT = re.compile(r"""
        (?x)                           # ignore whitespace
        ^                              # start of string
        (?P<output>[A-Za-z0-9\-]*)[ ]  # LVDS VGA etc
        (?P<connect>(dis)?connected)[ ]# dis/connected
        (?P<primary>(primary)?)[ ]?
        ((                             # a group
           (?P<width>\d+)x             # either 1024x768+0+0
           (?P<height>\d+)[+]  
           (?P<x>\d+)[+]
           (?P<y>\d+)
         )|[\D])                       # or not a digit
        .*                             # ignore rest of line
        """)

def get_monitor_layout():
    outputs = []

    for line in Popen(['xrandr', '-q'], stdout=PIPE).stdout:
        if not line.startswith("Screen") and not line.startswith(" "):
            output_params = REGEX_OUTPUT.match(line).groupdict()
            if output_params['width']:
                outputs.append("%s %s %s %s\n" % (
                            output_params['width'],
                            output_params['height'],
                            output_params['x'],
                            output_params['y']))
    return outputs

def notify_vm(vm, monitor_layout):
    pipe = vm.run("QUBESRPC qubes.SetMonitorLayout dom0", passio_popen=True, wait=True)

    pipe.stdin.write(''.join(monitor_layout))
    pipe.stdin.close()
    return pipe

def notify_vm_by_name(vmname):
    monitor_layout = get_monitor_layout()

    if len(monitor_layout) == 0:
        return

    qc = QubesVmCollection()
    qc.lock_db_for_reading()
    qc.load()
    qc.unlock_db()

    vm = qc.get_vm_by_name(vmname)
    if not vm:
        print >>sys.stderr, "No such VM!"
        return 1
    if not vm.is_running():
        print >>sys.stderr, "VM not running!"
        return 1
    pipe = notify_vm(vm, monitor_layout)
    pipe.wait()

    return 0

def notify_vms():

    monitor_layout = get_monitor_layout()

    if len(monitor_layout) == 0:
        return

    qc = QubesVmCollection()
    qc.lock_db_for_reading()
    qc.load()
    qc.unlock_db()

    pipes = []
    for vm in qc.values():
        if vm.qid == 0:
            continue
        if vm.is_running():
            pipes.append(notify_vm(vm, monitor_layout))

    for p in pipes:
        p.wait()

def reload_guid():
    check_call(["killall", "-HUP", "qubes-guid"])
                

def main():
    if len(sys.argv) > 1:
        """send monitor layout only to named VM"""
        exit(notify_vm_by_name(sys.argv[1]))
    else:
        reload_guid()
        notify_vms()

if __name__ == "__main__":
    main()
