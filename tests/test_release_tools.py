import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import subprocess
import shutil
import contextlib
import io

SPEC = importlib.util.spec_from_file_location('release_tools', Path(__file__).parents[1] / 'scripts' / 'release_tools.py')
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.bin = self.root / 'homehub.bin'
        self.bin.write_bytes(b'board firmware' * 64)
        self.manifest = self.root / 'release.json'
        self.data = {'board': 'WAVESHARE_T5AI_TOUCH_AMOLED_1_75', 'firmware': {
            'file': 'homehub.bin', 'sha256': hashlib.sha256(self.bin.read_bytes()).hexdigest(),
            'bytes': self.bin.stat().st_size, 'start_address': 0}}
        self.save()

    def tearDown(self):
        self.temp.cleanup()

    def save(self):
        self.manifest.write_text(json.dumps(self.data), encoding='utf-8')

    def test_valid_image_returns_verified_path(self):
        self.assertEqual(release.validate_release(self.manifest), self.bin)

    def test_modified_binary_is_refused(self):
        self.bin.write_bytes(b'x' * self.bin.stat().st_size)
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            release.validate_release(self.manifest)

    def test_wrong_board_is_refused(self):
        self.data['board'] = 'ESP32'
        self.save()
        with self.assertRaisesRegex(ValueError, 'board'):
            release.validate_release(self.manifest)

    def test_firmware_path_cannot_escape_release(self):
        self.data['firmware']['file'] = '../outside.bin'
        self.save()
        with self.assertRaisesRegex(ValueError, 'path'):
            release.validate_release(self.manifest)

    def test_image_cannot_cover_authorization_or_rf(self):
        self.data['firmware']['bytes'] = 0x7CD001
        self.save()
        with self.assertRaisesRegex(ValueError, 'protected'):
            release.validate_release(self.manifest)

    def test_nonzero_start_is_refused(self):
        self.data['firmware']['start_address'] = 0x10000
        self.save()
        with self.assertRaisesRegex(ValueError, 'start'):
            release.validate_release(self.manifest)

    def test_official_path_field_selects_flash_port(self):
        ports = [{'path': 'COM7', 'portRole': 'flash_auth'}, {'path': 'COM8', 'portRole': 'log'}]
        self.assertEqual(release.select_flash_port(ports, 'COM7')['path'], 'COM7')
        with self.assertRaisesRegex(ValueError, 'log port'):
            release.select_flash_port(ports, 'COM8')
        with self.assertRaisesRegex(ValueError, 'role'):
            release.select_flash_port([{'path': 'COM9', 'portRole': 'unknown'}], 'COM9')

    def test_real_ch342_metadata_identifies_a_port_without_role_field(self):
        ports = [
            {'path':'COM10','name':'wch.cn USB-Enhanced-SERIAL-A CH342 (COM10)',
             'usbVid':0x1A86,'usbPid':0x55D2,'usbInterface':0},
            {'path':'COM9','name':'wch.cn USB-Enhanced-SERIAL-B CH342 (COM9)',
             'usbVid':0x1A86,'usbPid':0x55D2,'usbInterface':2}]
        self.assertEqual(release.select_flash_port(ports,'COM10')['path'],'COM10')
        with self.assertRaises(ValueError):
            release.select_flash_port(ports,'COM9')
        ports[0]['usbVid']=0x1234
        with self.assertRaises(ValueError):
            release.select_flash_port(ports,'COM10')

    def test_build_overwriting_original_during_backup_cannot_change_written_image(self):
        expected = self.bin.read_bytes()
        written = []
        def fake_tool(tool, arguments, capture=False):
            if arguments[0] == 'list-ports':
                return '[{"path":"COM7","portRole":"flash_auth"}]'
            output = Path(arguments[arguments.index('-f') + 1])
            if arguments[0] == 'read':
                output.write_bytes(b'\0' * 0x800000)
                self.bin.write_bytes(b'x' * len(expected))
            elif arguments[0] == 'write':
                written.append(output.read_bytes())
            return ''
        with mock.patch.object(release, 'ROOT', self.root), mock.patch.object(release, 'run_tool', fake_tool), contextlib.redirect_stdout(io.StringIO()):
            release.flash(self.manifest, 'COM7', Path('unused-in-test.exe'))
        self.assertEqual(written, [expected])

    @unittest.skipUnless(shutil.which('pwsh'), 'PowerShell 7 unavailable')
    def test_audio_generation_refuses_to_replace_existing_human_recording(self):
        target = self.root / 'voices'
        target.mkdir()
        original = target / 'arrive_01.wav'
        original.write_bytes(b'personal human recording')
        script = Path(__file__).parents[1] / 'scripts' / 'generate-test-audio.ps1'
        result = subprocess.run([shutil.which('pwsh'), '-NoProfile', '-File', str(script),
                                 '-OutputDirectory', str(target)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(original.read_bytes(), b'personal human recording')

    def test_card_copy_preserves_config_recordings_and_existing_voice(self):
        source = self.root / 'source'
        (source / 'voices').mkdir(parents=True)
        (source / 'voices' / 'arrive.wav').write_bytes(b'test speech')
        (source / 'config.example.json').write_text('{}')
        card = self.root / 'card'
        (card / 'homehub' / 'messages').mkdir(parents=True)
        (card / 'homehub' / 'voices').mkdir()
        (card / 'homehub' / 'config.json').write_text('{"keep":true}')
        (card / 'homehub' / 'messages' / 'user.wav').write_bytes(b'personal')
        (card / 'homehub' / 'voices' / 'arrive.wav').write_bytes(b'human speech')
        release.prepare_card(source, card)
        self.assertEqual((card / 'homehub' / 'config.json').read_text(), '{"keep":true}')
        self.assertEqual((card / 'homehub' / 'messages' / 'user.wav').read_bytes(), b'personal')
        self.assertEqual((card / 'homehub' / 'voices' / 'arrive.wav').read_bytes(), b'human speech')


if __name__ == '__main__':
    unittest.main()
