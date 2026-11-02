import concurrent.futures
import struct
import unittest
from types import SimpleNamespace
from unittest.mock import Mock
from server import Bridge, verified_c5_identity
from baud_negotiation import negotiate
from host_connection import Op

class XiaoSingleUsbTests(unittest.TestCase):
    def diagnostics(self, count):
        bridge = Bridge.__new__(Bridge)
        bridge.update = Mock()
        bridge.model = SimpleNamespace(log=Mock())
        future = concurrent.futures.Future()
        payload = struct.pack('<BB', 1, count)
        payload += struct.pack('<BBIIIIIIII', 2, 0, 10, 1, 1, 100, 200, 0, 0, 3) * count
        payload += struct.pack('<BII', 1, 2048, 0)
        future.set_result(payload)
        bridge.host_link_diagnostics(future)
        return bridge

    def test_native_usb_has_one_diagnostic_endpoint(self):
        bridge = self.diagnostics(1)
        self.assertEqual(len(bridge.update.call_args.kwargs['device_link_diagnostics']['endpoints']), 1)
        bridge.model.log.assert_not_called()

    def test_waveshare_keeps_two_diagnostic_endpoints(self):
        bridge = self.diagnostics(2)
        self.assertEqual(len(bridge.update.call_args.kwargs['device_link_diagnostics']['endpoints']), 2)
        bridge.model.log.assert_not_called()

    def test_invalid_endpoint_count_is_rejected(self):
        bridge = self.diagnostics(3)
        bridge.update.assert_not_called()
        bridge.model.log.assert_called_once()

    def test_native_usb_never_attempts_uart_baud_negotiation(self):
        client = Mock(limits={'endpoint': 0, 'operations': [Op.BAUD_PREPARE]})
        bridge = SimpleNamespace(host_client=client)
        negotiate(bridge)
        client.request.assert_not_called()

    def test_no_baud_capability_never_attempts_negotiation(self):
        client = Mock(limits={'endpoint': 1, 'operations': []})
        bridge = SimpleNamespace(host_client=client)
        negotiate(bridge)
        client.request.assert_not_called()

class C5IdentityTests(unittest.TestCase):
    def test_known_board_profile_pairs(self):
        for profile, hardware in [('xiao-c5-usb-3m1664k-v1', 'Seeed Studio XIAO ESP32-C5'), ('waveshare-c5-rgb-usb-4m6m-v1', 'ESP32-C5 DevKit')]:
            self.assertTrue(verified_c5_identity(dict(build_profile=profile, hardware=hardware)))
    def test_mismatched_or_unknown_profile_rejected(self):
        for profile in ['xiao-c5-usb-3m1664k-v1', 'unknown']:
            self.assertFalse(verified_c5_identity(dict(build_profile=profile, hardware='ESP32-C5 DevKit', firmware='v1.18.0')))
    def test_legacy_identity_stays_supported(self):
        self.assertTrue(verified_c5_identity(dict(hardware='ESP32-C5 DevKit', firmware='v1.18.0')))
