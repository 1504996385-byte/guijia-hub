"""Safety tests use synthetic credentials and an in-memory UART, never a device."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("provision_tuya", ROOT / "scripts/provision_tuya.py")
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)
UUID = "A" * 20
KEY = "b" * 30 + "_-"
SECOND_UUID = "C" * 20
SECOND_KEY = "d" * 32
USAGE = b"Use like: auth uuidxxxxxxxxxxxxxxxx keyxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n"
ACK = b"Authorization write succeeds.\r\n"


class Clock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        self.now += 0.25
        return self.now


class UART:
    def __init__(self, replies):
        self.replies = list(replies)
        self.pending = b""
        self.writes = []
        self.closed = False

    def reset_input_buffer(self):
        self.pending = b""

    def write(self, data):
        self.writes.append(data)
        reply = self.replies.pop(0) if self.replies else b""
        if isinstance(reply, BaseException):
            raise reply
        self.pending = reply
        return len(data)

    def read(self, count):
        result, self.pending = self.pending[:count], self.pending[count:]
        return result

    def close(self):
        self.closed = True


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="homehub-provision-test-")
        self.addCleanup(self.directory.cleanup)

    def credentials(self, fields=None, *, csv=None, bom=False):
        path = Path(self.directory.name) / ("input.csv" if csv is not None else "input.json")
        text = csv if csv is not None else json.dumps(fields or {"UUID": UUID, "AuthKey": KEY})
        path.write_text(text, encoding="utf-8-sig" if bom else "utf-8")
        return str(path)

    def xlsx(self, *, rows=None, physical_rows=None, filename=None, shared=False,
             headers=("uuid", "key"), formula=False, second_sheet=False):
        """Create only synthetic workbook data, never copy a user's file."""
        rows = rows if rows is not None else [(UUID, KEY), (SECOND_UUID, SECOND_KEY)]
        physical_rows = physical_rows or list(range(2, len(rows) + 2))
        path = Path(self.directory.name) / (filename or "3rhb25ktaqov4akw-order-2-20261106120000.xlsx")
        ns = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
        relns = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
        packns = "http://schemas.openxmlformats.org/package/2006/relationships"
        workbook = ET.Element("{" + ns + "}workbook")
        sheets = ET.SubElement(workbook, "{" + ns + "}sheets")
        ET.SubElement(sheets, "{" + ns + "}sheet", {"name": "devices", "sheetId": "1", "{" + relns + "}id": "rId1"})
        if second_sheet:
            ET.SubElement(sheets, "{" + ns + "}sheet", {"name": "other", "sheetId": "2", "{" + relns + "}id": "rId2"})
        relationships = ET.Element("{" + packns + "}Relationships")
        ET.SubElement(relationships, "{" + packns + "}Relationship", {"Id": "rId1", "Type": relns + "/worksheet", "Target": "worksheets/sheet1.xml"})
        sheet = ET.Element("{" + ns + "}worksheet")
        data = ET.SubElement(sheet, "{" + ns + "}sheetData")
        strings = []
        for number, values in [(1, headers)] + list(zip(physical_rows, rows)):
            row = ET.SubElement(data, "{" + ns + "}row", {"r": str(number)})
            for col, value in zip(("A", "B", "C"), values):
                cell = ET.SubElement(row, "{" + ns + "}c", {"r": col + str(number), "t": "s" if shared else "inlineStr"})
                if formula and number != 1 and col == "A":
                    ET.SubElement(cell, "{" + ns + "}f").text = '"not-executed"'
                if shared:
                    strings.append(value)
                    ET.SubElement(cell, "{" + ns + "}v").text = str(len(strings) - 1)
                else:
                    inline = ET.SubElement(cell, "{" + ns + "}is")
                    ET.SubElement(inline, "{" + ns + "}t").text = value
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("xl/workbook.xml", ET.tostring(workbook))
            archive.writestr("xl/_rels/workbook.xml.rels", ET.tostring(relationships))
            archive.writestr("xl/worksheets/sheet1.xml", ET.tostring(sheet))
            if shared:
                table = ET.Element("{" + ns + "}sst")
                for value in strings:
                    item = ET.SubElement(table, "{" + ns + "}si")
                    ET.SubElement(item, "{" + ns + "}t").text = value
                archive.writestr("xl/sharedStrings.xml", ET.tostring(table))
        return str(path)

    def run_cli(self, args, uart=None, factory=None):
        factory = factory or mock.Mock(return_value=uart)
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output), \
                mock.patch.object(provision.time, "monotonic", Clock()):
            code = provision.main(args, serial_factory=factory)
        raw = output.getvalue()
        self.assertTrue(all(secret not in raw for secret in (UUID, KEY, SECOND_UUID, SECOND_KEY)), "Output exposed credential material")
        result = json.loads(raw)
        return code, result, factory

    def test_default_validate_json_bom_does_not_import_or_open_serial(self):
        path = self.credentials(bom=True)
        with mock.patch.dict("sys.modules", {"serial": None}):
            code, result, factory = self.run_cli([path])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"])
        self.assertEqual((result["uuid_len"], result["authkey_len"]), (20, 32))
        self.assertIsNone(result["pid_ok"])
        factory.assert_not_called()

    def test_csv_header_aliases_and_bom(self):
        path = self.credentials(csv="UUID,auzkey\n" + UUID + "," + KEY + "\n", bom=True)
        code, result, factory = self.run_cli([path])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"])
        factory.assert_not_called()

    def test_single_csv_record_without_header(self):
        code, result, _ = self.run_cli([self.credentials(csv=UUID + "," + KEY)])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"])

    def test_invalid_uuid_and_command_injection_never_open_uart(self):
        for bad in ("A" * 19, "A" * 21, "A" * 19 + "_", "A" * 18 + "\r\n", "Ａ" * 20):
            with self.subTest(case="invalid UUID"):
                code, result, factory = self.run_cli([self.credentials({"uuid": bad, "authkey": KEY}), "--write", "--port", "COM10"])
                self.assertNotEqual(code, 0)
                self.assertFalse(result["validated"])
                factory.assert_not_called()

    def test_invalid_key_and_command_injection_never_open_uart(self):
        for bad in ("x" * 31, "x" * 31 + " ", "x" * 30 + "\r\n", "x" * 31 + ";", "é" * 32):
            with self.subTest(case="invalid key"):
                code, result, factory = self.run_cli([self.credentials({"uuid": UUID, "AuthKey": bad}), "--write", "--port", "COM10"])
                self.assertNotEqual(code, 0)
                self.assertFalse(result["validated"])
                factory.assert_not_called()

    def test_conflicting_key_aliases_are_rejected(self):
        _, result, factory = self.run_cli([self.credentials({"uuid": UUID, "AuthKey": KEY, "auzkey": "C" * 32})])
        self.assertFalse(result["validated"])
        factory.assert_not_called()

    def test_multiple_csv_records_are_rejected(self):
        path = self.credentials(csv="uuid,AuthKey\n" + UUID + "," + KEY + "\n" + UUID + "," + KEY)
        code, result, factory = self.run_cli([path])
        self.assertNotEqual(code, 0)
        self.assertFalse(result["validated"])
        factory.assert_not_called()

    def test_wrong_pid_is_rejected_before_uart(self):
        path = self.credentials({"uuid": UUID, "AuthKey": KEY, "PID": "different-product"})
        _, result, factory = self.run_cli([path, "--write", "--port", "COM10"])
        self.assertFalse(result["pid_ok"])
        self.assertEqual(result["error"], "pid_mismatch_configure_product")
        factory.assert_not_called()

    def test_write_requires_explicit_port(self):
        code, result, factory = self.run_cli([self.credentials(), "--write"])
        self.assertNotEqual(code, 0)
        self.assertFalse(result["write_attempted"])
        factory.assert_not_called()

    def test_probe_sends_only_empty_auth(self):
        uart = UART([USAGE])
        code, result, factory = self.run_cli(["--probe", "--port", "COM10"], uart)
        self.assertEqual(code, 0)
        self.assertTrue(result["auth_available"])
        self.assertIsNone(result["pid_ok"])
        self.assertEqual(uart.writes, [b"auth\r\n"])
        self.assertTrue(uart.closed)
        self.assertEqual(factory.call_args.kwargs["baudrate"], 115200)

    def test_missing_auth_command_prevents_write(self):
        uart = UART([b"unknown command\r\n"])
        _, result, _ = self.run_cli([self.credentials(), "--write", "--port", "COM10"], uart)
        self.assertFalse(result["auth_available"])
        self.assertFalse(result["write_attempted"])
        self.assertEqual(uart.writes, [b"auth\r\n"])

    def test_verified_write_requires_separate_matching_readback(self):
        uart = UART([USAGE, ACK, (UUID + "\r\n" + KEY + "\r\n").encode("ascii")])
        code, result, _ = self.run_cli([self.credentials(), "--write", "--port", "COM10"], uart)
        self.assertEqual(code, 0)
        self.assertTrue(result["verified"])
        self.assertEqual(uart.writes, [b"auth\r\n", ("auth " + UUID + " " + KEY + " 0\r\n").encode("ascii"), b"auth-read 0\r\n"])

    def test_unknown_write_is_not_retried_or_read_as_success(self):
        uart = UART([USAGE, b""])
        _, result, _ = self.run_cli([self.credentials(), "--write", "--port", "COM10"], uart)
        self.assertEqual(result["status"], "unknown")
        self.assertTrue(result["write_attempted"])
        self.assertFalse(result["verified"])
        self.assertEqual(len(uart.writes), 2)

    def test_command_echo_cannot_verify_readback(self):
        echo = ("auth " + UUID + " " + KEY + " 0\r\n").encode("ascii")
        uart = UART([USAGE, echo + ACK, b"auth-read 0\r\n" + echo])
        _, result, _ = self.run_cli([self.credentials(), "--write", "--port", "COM10"], uart)
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(result["verified"])
        self.assertEqual(len(uart.writes), 3)

    def test_mismatching_or_prefixed_readback_cannot_verify(self):
        for reply in ((UUID + "\r\n" + "z" * 32 + "\r\n"), ("uuid:" + UUID + "\r\nkey:" + KEY + "\r\n")):
            uart = UART([USAGE, ACK, reply.encode("ascii")])
            _, result, _ = self.run_cli([self.credentials(), "--write", "--port", "COM10"], uart)
            self.assertFalse(result["verified"])
            self.assertEqual(result["status"], "unknown")

    def test_unterminated_key_prefix_cannot_verify(self):
        # A 32-byte prefix can become a longer, invalid line in the next read.
        for reply in (UUID + "\r\n" + KEY, UUID + "\r\n" + KEY + "x\r\n"):
            uart = UART([USAGE, ACK, reply.encode("ascii")])
            _, result, _ = self.run_cli([self.credentials(), "--write", "--port", "COM10"], uart)
            self.assertFalse(result["verified"])
            self.assertEqual(result["status"], "unknown")

    def test_matching_product_id_alias_is_accepted(self):
        path = self.credentials({"uuid": UUID, "auzkey": KEY, "Product_ID": provision.EXPECTED_PID})
        code, result, factory = self.run_cli([path])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"] and result["pid_ok"])
        factory.assert_not_called()

    def test_invalid_arguments_do_not_echo_input(self):
        code, result, factory = self.run_cli(["--unexpected=" + KEY])
        self.assertNotEqual(code, 0)
        self.assertEqual(result["error"], "invalid_arguments")
        factory.assert_not_called()

    def test_write_exception_is_unknown_and_never_exposes_exception(self):
        uart = UART([USAGE, RuntimeError(UUID + KEY)])
        _, result, _ = self.run_cli([self.credentials(), "--write", "--port", "COM10"], uart)
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(result["verified"])
        self.assertEqual(len(uart.writes), 2)
        self.assertTrue(uart.closed)

    def test_serial_open_exception_never_exposes_exception(self):
        factory = mock.Mock(side_effect=RuntimeError(UUID + KEY))
        _, result, _ = self.run_cli(["--probe", "--port", "COM10"], factory=factory)
        self.assertEqual(result["error"], "serial_unavailable")

    def test_interrupted_write_remains_unknown(self):
        uart = UART([USAGE, KeyboardInterrupt()])
        try:
            code, result, _ = self.run_cli([self.credentials(), "--write", "--port", "COM10"], uart)
        except KeyboardInterrupt:
            self.fail("Interrupted write bypassed the sanitized unknown status")
        self.assertNotEqual(code, 0)
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(result["verified"])
        self.assertTrue(uart.closed)
        self.assertEqual(len(uart.writes), 2)

    def test_bad_port_is_rejected_without_echoing_input(self):
        _, result, factory = self.run_cli(["--probe", "--port", "COM10\r\n" + KEY])
        self.assertFalse(result["write_attempted"])
        factory.assert_not_called()

    def test_project_pid_default_and_key_alias(self):
        self.assertEqual(provision.EXPECTED_PID, "3rhb25ktaqov4akw")
        code, result, factory = self.run_cli([self.credentials({"uuid": UUID, "key": KEY})])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"])
        factory.assert_not_called()

    def test_xlsx_multiple_records_require_explicit_row(self):
        code, result, factory = self.run_cli([self.xlsx(), "--write", "--port", "COM10"])
        self.assertNotEqual(code, 0)
        self.assertEqual(result["error"], "xlsx_row_required")
        self.assertFalse(result["write_attempted"])
        factory.assert_not_called()

    def test_xlsx_inline_selection_validates_without_uart(self):
        code, result, factory = self.run_cli([self.xlsx(), "--row", "2"])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"])
        self.assertEqual(result["row"], 2)
        self.assertIsNone(result["pid_ok"])
        factory.assert_not_called()

    def test_xlsx_shared_strings_row_three_writes_only_selected_record(self):
        uart = UART([USAGE, ACK, (SECOND_UUID + "\r\n" + SECOND_KEY + "\r\n").encode("ascii")])
        code, result, _ = self.run_cli([self.xlsx(shared=True), "--row", "3", "--write", "--port", "COM10"], uart)
        self.assertEqual(code, 0)
        self.assertTrue(result["verified"])
        self.assertEqual(result["row"], 3)
        self.assertTrue(uart.writes[1] == ("auth " + SECOND_UUID + " " + SECOND_KEY + " 0\r\n").encode("ascii"), "Wrong selected workbook record")

    def test_differently_named_uuid_key_xlsx_is_valid_with_unknown_pid(self):
        code, result, factory = self.run_cli([self.xlsx(filename="device-authorizations.xlsx"), "--row", "2"])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"])
        self.assertIsNone(result["pid_ok"])
        factory.assert_not_called()

    def test_xlsx_filename_prefix_is_not_product_metadata(self):
        filename = "Z" * 16 + "-material-order-2-20261106120000.xlsx"
        code, result, factory = self.run_cli([self.xlsx(filename=filename), "--row", "2"])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"])
        self.assertIsNone(result["pid_ok"])
        factory.assert_not_called()

    def test_expected_pid_argument_cannot_verify_absent_product_metadata(self):
        pid = "Z" * 16
        code, result, factory = self.run_cli([self.xlsx(), "--expected-pid", pid, "--row", "2"])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"])
        self.assertIsNone(result["pid_ok"])
        factory.assert_not_called()

    def test_explicit_xlsx_pid_metadata_mismatch_rejects_before_uart(self):
        path = self.xlsx(rows=[(UUID, KEY, "Z" * 16)], headers=("uuid", "key", "PID"))
        code, result, factory = self.run_cli([path, "--row", "2", "--write", "--port", "COM10"])
        self.assertNotEqual(code, 0)
        self.assertIs(result["pid_ok"], False)
        self.assertEqual(result["error"], "pid_mismatch_configure_product")
        self.assertFalse(result["write_attempted"])
        factory.assert_not_called()

    def test_explicit_xlsx_pid_metadata_matching_expectation_is_checked(self):
        pid = "Z" * 16
        path = self.xlsx(filename="batch.xlsx", rows=[(UUID, KEY, pid)], headers=("uuid", "key", "Product_ID"))
        code, result, factory = self.run_cli([path, "--expected-pid", pid, "--row", "2"])
        self.assertEqual(code, 0)
        self.assertTrue(result["validated"])
        self.assertIs(result["pid_ok"], True)
        factory.assert_not_called()

    def test_xlsx_physical_row_selection_does_not_use_record_index(self):
        path = self.xlsx(rows=[(UUID, KEY)], physical_rows=[5])
        code, result, factory = self.run_cli([path])
        self.assertEqual(code, 0)
        self.assertEqual(result["row"], 5)
        _, result, _ = self.run_cli([path, "--row", "2"])
        self.assertEqual(result["error"], "xlsx_row_not_found")
        factory.assert_not_called()

    def test_xlsx_formula_cache_is_never_used_as_credentials(self):
        _, result, factory = self.run_cli([self.xlsx(formula=True), "--row", "2", "--write", "--port", "COM10"])
        self.assertEqual(result["error"], "xlsx_formulas_not_supported")
        self.assertFalse(result["write_attempted"])
        factory.assert_not_called()

    def test_xlsx_access_id_secret_are_not_device_credentials(self):
        _, result, factory = self.run_cli([self.xlsx(headers=("AccessID", "Secret")), "--row", "2"])
        self.assertFalse(result["validated"])
        factory.assert_not_called()

    def test_xlsx_multiple_sheets_are_ambiguous(self):
        _, result, factory = self.run_cli([self.xlsx(second_sheet=True), "--row", "2"])
        self.assertEqual(result["error"], "xlsx_single_sheet_required")
        factory.assert_not_called()


if __name__ == "__main__":
    unittest.main()
