import hashlib
import os
import unittest

from firmware_image import (
    esp_image_sha256,
    firmware_hash_kiss_frame,
    firmware_reset_kiss_frame,
)


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVICE = os.path.join(ROOT, "Device.h")


def make_image(payload: bytes, *, hash_appended: bool) -> bytes:
    header = bytearray(24)
    header[0] = 0xE9
    header[23] = int(hash_appended)
    content = bytes(header) + payload
    if hash_appended:
        return content + hashlib.sha256(content).digest()
    return content


class EspImageSha256Tests(unittest.TestCase):
    def test_hashes_complete_image_without_appended_hash(self):
        image = make_image(b"firmware", hash_appended=False)

        self.assertEqual(esp_image_sha256(image), hashlib.sha256(image).digest())

    def test_returns_valid_appended_hash(self):
        image = make_image(b"firmware", hash_appended=True)

        self.assertEqual(esp_image_sha256(image), image[-32:])

    def test_rejects_invalid_appended_hash(self):
        image = make_image(b"firmware", hash_appended=True)
        damaged_image = image[:-1] + bytes([image[-1] ^ 0xFF])

        with self.assertRaisesRegex(ValueError, "invalid appended SHA-256"):
            esp_image_sha256(damaged_image)

    def test_rejects_non_esp_image(self):
        with self.assertRaisesRegex(ValueError, "not an ESP application image"):
            esp_image_sha256(bytes(24))


class FirmwareHashKissFrameTests(unittest.TestCase):
    def test_frames_and_escapes_firmware_hash(self):
        firmware_hash = bytes([0xC0, 0xDB]) + bytes(range(30))

        frame = firmware_hash_kiss_frame(firmware_hash)

        self.assertEqual(frame[:2], bytes([0xC0, 0x58]))
        self.assertEqual(frame[2:6], bytes([0xDB, 0xDC, 0xDB, 0xDD]))
        self.assertEqual(frame[-1], 0xC0)

    def test_rejects_incorrect_hash_length(self):
        with self.assertRaisesRegex(ValueError, "exactly 32 bytes"):
            firmware_hash_kiss_frame(bytes(31))

    def test_reset_frame_matches_rnode_protocol(self):
        self.assertEqual(firmware_reset_kiss_frame(), bytes([0xC0, 0x55, 0xF8, 0xC0]))


class FirmwareHashPersistenceTests(unittest.TestCase):
    def test_esp32_stages_complete_hash_before_single_commit(self):
        with open(DEVICE, "r", encoding="utf-8") as handle:
            source = handle.read()
        save = source[source.index("void device_save_firmware_hash()"):
                      source.index("void device_validate_partitions()")]
        esp32 = save[save.index("#if HAS_EEPROM && MCU_VARIANT == MCU_ESP32"):
                     save.index("#else")]
        self.assertIn("EEPROM.write(address, dev_firmware_hash_target[i])", esp32)
        self.assertEqual(1, esp32.count("EEPROM.commit()"))
        self.assertNotIn(
            "eeprom_update(dev_fwhash_addr(i), dev_firmware_hash_target[i])",
            esp32)


if __name__ == "__main__":
    unittest.main()
