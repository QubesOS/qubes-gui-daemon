# -*- encoding: utf-8 -*-
#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2017 Marek Marczykowski-Górecki
#                               <marmarek@invisiblethingslab.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, see <http://www.gnu.org/licenses/>.

'''Microphone control extension'''

import pydbus
from gi.repository import GLib

import qubes.devices
import qubes.ext
import qubes.vm.adminvm

class MicDevice(qubes.devices.DeviceInfo):
    '''Microphone device info class'''
    pass

class MicDeviceExtension(qubes.ext.Extension):
    '''Extension to control microphone access

    '''

    def __init__(self):
        super(MicDeviceExtension, self).__init__()
        self.bus = pydbus.SystemBus()

    def get_device(self, app):
        return MicDevice(app.domains[0], 'mic', 'Microphone')

    @qubes.ext.handler('device-list:mic')
    def on_device_list_mic(self, vm, event):
        '''List microphone device

        Currently this assume audio being handled in dom0. When adding support
        for GUI domain, this needs to be changed
        '''
        return self.on_device_get_mic(vm, event, 'mic')

    @qubes.ext.handler('device-get:mic')
    def on_device_get_mic(self, vm, event, ident):
        '''Get microphone device

        Currently this assume audio being handled in dom0. When adding support
        for GUI domain, this needs to be changed
        '''
        # pylint: disable=unused-argument,no-self-use

        if not isinstance(vm, qubes.vm.adminvm.AdminVM):
            return

        if ident != 'mic':
            return

        yield self.get_device(vm.app)

    @qubes.ext.handler('device-list-attached:mic')
    def on_device_list_attached_mic(self, vm, event, persistent=None):
        '''List attached microphone to the VM'''

        if persistent is True:
            return

        try:
            mic_obj = self.bus.get('org.qubesos.Audio.' + vm.name,
                '/org/qubesos/audio')
        except GLib.Error:
            # no user session bus, or can't access pacat-simple-vchan
            return

        if mic_obj.RecAllowed:
            # (device, options)
            yield (self.get_device(vm.app), {})

    @qubes.ext.handler('device-pre-attach:mic')
    def on_device_pre_attach_mic(self, vm, event, device, options):
        '''Attach microphone to the VM'''

        # there is only one microphone
        assert device == self.get_device(vm.app)
        if options:
            raise qubes.exc.QubesException(
                'mic device does not support options')

        try:
            mic_obj = self.bus.get('org.qubesos.Audio.' + vm.name,
                '/org/qubesos/audio')
        except GLib.Error as err:
            # no user session bus, or can't access pacat-simple-vchan
            raise qubes.exc.QubesException(
                'Failed to attach microphone: {}'.format(err))

        mic_obj.RecAllowed = True

    @qubes.ext.handler('device-pre-detach:mic')
    def on_device_pre_detach_mic(self, vm, event, device):
        '''Attach microphone to the VM'''

        # there is only one microphone
        assert device == self.get_device(vm.app)

        try:
            mic_obj = self.bus.get('org.qubesos.Audio.' + vm.name,
                '/org/qubesos/audio')
        except GLib.Error as err:
            # no user session bus, or can't access pacat-simple-vchan
            raise qubes.exc.QubesException(
                'Failed to attach microphone: {}'.format(err))

        mic_obj.RecAllowed = False
