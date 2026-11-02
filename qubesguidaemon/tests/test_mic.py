# -*- encoding: utf-8 -*-
#
# The Qubes OS Project, http://www.qubes-os.org
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

"""Tests for the microphone extension"""

import unittest
from unittest import mock

from qubesguidaemon import mic


class TestAudioInputRequest(unittest.TestCase):
    def setUp(self):
        self.ext = mic.MicDeviceExtension()
        self.ext.last_blocked_event.clear()
        self.qdb = {}
        self.audiovm = mock.Mock()
        self.audiovm.untrusted_qdb.read.side_effect = self.qdb.get
        self.vm = mock.Mock()
        self.vm.name = "work"
        self.vm.audiovm = self.audiovm
        self.other_vm = mock.Mock()
        self.other_vm.name = "other"
        self.other_vm.audiovm = mock.Mock()
        domains = {"work": self.vm, "other": self.other_vm}
        self.audiovm.app.domains = domains

    def request(self, name="work"):
        path = "/audio-input-request/" + name
        self.ext.on_audio_input_request(
            self.audiovm, "domain-qdb-change:/audio-input-request/", path
        )

    def test_000_not_attached(self):
        self.qdb["/audio-input-request/work"] = b"1"
        self.request()
        self.vm.fire_event.assert_called_once_with("audio-input-blocked")

    def test_001_attached(self):
        self.qdb["/audio-input-request/work"] = b"1"
        self.qdb["/audio-input-config/work"] = b"1"
        self.request()
        self.vm.fire_event.assert_not_called()

    def test_002_detached(self):
        self.qdb["/audio-input-request/work"] = b"1"
        self.qdb["/audio-input-config/work"] = b"0"
        self.request()
        self.vm.fire_event.assert_called_once_with("audio-input-blocked")

    def test_003_stop(self):
        self.qdb["/audio-input-request/work"] = b"0"
        self.request()
        self.qdb.pop("/audio-input-request/work")
        self.request()
        self.vm.fire_event.assert_not_called()

    def test_004_unknown_vm(self):
        self.qdb["/audio-input-request/nonexistent"] = b"1"
        self.request("nonexistent")
        self.vm.fire_event.assert_not_called()

    def test_005_other_audiovm(self):
        self.qdb["/audio-input-request/other"] = b"1"
        self.request("other")
        self.other_vm.fire_event.assert_not_called()

    def test_006_rate_limit(self):
        self.qdb["/audio-input-request/work"] = b"1"
        with mock.patch("time.monotonic", return_value=100):
            self.request()
            self.request()
        self.assertEqual(self.vm.fire_event.call_count, 1)
        with mock.patch("time.monotonic", return_value=105):
            self.request()
        self.assertEqual(self.vm.fire_event.call_count, 1)
        with mock.patch("time.monotonic", return_value=111):
            self.request()
        self.assertEqual(self.vm.fire_event.call_count, 2)

    def test_007_no_qdb(self):
        self.audiovm.untrusted_qdb = None
        self.request()
        self.vm.fire_event.assert_not_called()

    def test_010_watch(self):
        vm = mock.Mock()
        self.ext.on_domain_init_load(vm, "domain-load")
        vm.watch_qdb_path.assert_called_once_with("/audio-input-request/")
