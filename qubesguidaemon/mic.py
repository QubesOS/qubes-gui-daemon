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

"""Microphone control extension"""

import subprocess

import qubes.device_protocol
import qubes.ext
import qubes.vm.adminvm


class MicDevice(qubes.device_protocol.DeviceInfo):
    """Microphone device info class"""

    def __init__(self, backend_domain, product, manufacturer):
        super().__init__(
            backend_domain=backend_domain,
            ident="mic",
            devclass="mic",
            product=product,
            manufacturer=manufacturer,
        )
        self._interfaces = [
            qubes.device_protocol.DeviceInterface("******", devclass="mic")
        ]


class MicDeviceExtension(qubes.ext.Extension):
    """
    Extension to control microphone access
    """

    def __init__(self):
        super(MicDeviceExtension, self).__init__()

    @staticmethod
    def get_device(app):
        return MicDevice(
            app.domains[0], product="microphone", manufacturer="build-in"
        )

    @qubes.ext.handler("device-list:mic")
    def on_device_list_mic(self, vm, event):
        """List microphone device

        Currently, this assumes audio being handled in dom0. When adding support
        for GUI domain, this needs to be changed
        """
        return self.on_device_get_mic(vm, event, "mic")

    @qubes.ext.handler("device-get:mic")
    def on_device_get_mic(self, vm, event, ident):
        """Get microphone device

        Currently, this assumes audio being handled in dom0. When adding support
        for GUI domain, this needs to be changed
        """
        # pylint: disable=unused-argument,no-self-use

        if not isinstance(vm, qubes.vm.adminvm.AdminVM):
            return

        if ident != "mic":
            return

        yield self.get_device(vm.app)

    @qubes.ext.handler("device-list-attached:mic")
    def on_device_list_attached_mic(self, vm, event, persistent=None):
        """List attached microphone to the VM"""

        if persistent is True:
            return

        audiovm = getattr(vm, "audiovm", None)

        if audiovm is None or not audiovm.is_running():
            return

        untrusted_audio_input = audiovm.untrusted_qdb.read(
            "/audio-input-config/{}".format(vm.name)
        )
        if untrusted_audio_input == b"1":
            # (device, options)
            yield self.get_device(vm.app), {}

    @qubes.ext.handler("device-pre-attach:mic")
    async def on_device_pre_attach_mic(self, vm, event, device, options):
        """Attach microphone to the VM"""

        # there is only one microphone
        assert device == self.get_device(vm.app)
        if options:
            raise qubes.exc.QubesException(
                "mic device does not support options"
            )

        audiovm = getattr(vm, "audiovm", None)

        if audiovm is None:
            raise qubes.exc.QubesException(
                "VM {} has no AudioVM set".format(vm)
            )

        if not audiovm.is_running():
            raise qubes.exc.QubesVMNotRunningError(
                audiovm, "Audio VM {} isn't running".format(audiovm)
            )

        if audiovm.features.check_with_netvm(
            "supported-rpc.qubes.AudioInputEnable", False
        ):
            try:
                await audiovm.run_service_for_stdio(
                    "qubes.AudioInputEnable+{}".format(vm.name)
                )
            except subprocess.CalledProcessError:
                raise qubes.exc.QubesVMError(
                    vm,
                    "Failed to attach audio input from {!s} to {!s}: "
                    "pulseaudio agent not running".format(audiovm, vm),
                )
        else:
            audiovm.untrusted_qdb.write(
                "/audio-input-config/{}".format(vm.name), "1"
            )

    # pylint: disable=unused-argument
    @qubes.ext.handler("device-pre-detach:mic")
    async def on_device_pre_detach_mic(self, vm, event, device):
        """Detach microphone from the VM"""

        # there is only one microphone
        assert device == self.get_device(vm.app)

        audiovm = getattr(vm, "audiovm", None)

        if audiovm is None:
            raise qubes.exc.QubesException(
                "VM {} has no AudioVM set".format(vm)
            )

        if not audiovm.is_running():
            raise qubes.exc.QubesVMNotRunningError(
                audiovm, "Audio VM {} isn't running".format(audiovm)
            )

        if audiovm.features.check_with_netvm(
            "supported-rpc.qubes.AudioInputDisable", False
        ):
            try:
                await audiovm.run_service_for_stdio(
                    "qubes.AudioInputDisable+{}".format(vm.name)
                )
            except subprocess.CalledProcessError:
                raise qubes.exc.QubesVMError(
                    vm,
                    "Failed to detach audio input from {!s} to {!s}: "
                    "pulseaudio agent not running".format(audiovm, vm),
                )
        else:
            audiovm.untrusted_qdb.write(
                "/audio-input-config/{}".format(vm.name), "0"
            )

    @qubes.ext.handler("property-set:audiovm")
    def on_property_set(self, subject, event, name, newvalue, oldvalue=None):
        if not subject.is_running() or not newvalue:
            return
        if not newvalue.is_running():
            subject.log.warning(
                "Cannot attach mic to {!s}: "
                "AudioVM '{!s}' is powered off.".format(subject, newvalue)
            )
        if newvalue == oldvalue:
            return
        if oldvalue and oldvalue.is_running():
            mic_allowed = oldvalue.untrusted_qdb.read(
                "/audio-input-config/{}".format(subject.name)
            )
            if mic_allowed is None:
                return
            try:
                mic_allowed_value = mic_allowed.decode("ascii")
            except UnicodeError:
                raise qubes.exc.QubesVMError(
                    subject,
                    "Cannot decode ASCII value for '/audio-input-config/{!s}'".format(
                        subject.name
                    ),
                )
            if mic_allowed_value in ("0", "1"):
                newvalue.untrusted_qdb.write(
                    "/audio-input-config/{}".format(subject.name),
                    mic_allowed_value,
                )
            else:
                raise qubes.exc.QubesVMError(
                    subject,
                    "Invalid value '{!s}' for '/audio-input-config/{!s}' from {!s}".format(
                        mic_allowed_value, subject.name, oldvalue
                    ),
                )

    @qubes.ext.handler("domain-qdb-create")
    def on_domain_qdb_create(self, vm, event):
        if vm.audiovm and vm.audiovm.is_running():
            # Remove previous config, status and request entries on audiovm start
            vm.audiovm.untrusted_qdb.rm(
                "/audio-input-config/{}".format(vm.name)
            )
            vm.audiovm.untrusted_qdb.rm("/audio-input/{}".format(vm.name))
            vm.audiovm.untrusted_qdb.rm(
                "/audio-input-request/{}".format(vm.name)
            )
