"""Validate a local Tuya JSON/CSV/XLSX; UART writes require --write --port COMx.

Examples:
  python scripts/provision_tuya.py C:\\private\\license.json
  python scripts/provision_tuya.py --probe --port COM10
  python scripts/provision_tuya.py C:\\private\\license.json --write --port COM10
  python scripts/provision_tuya.py C:\\private\\devices.xlsx --row 2

Only local credential files are accepted: never put credentials in arguments or
environment variables. UART0 is fixed at 115200 baud. Output contains metadata
only. An unknown write is never retried; inspect the device before any new run.
Filenames do not establish product identity. pid_ok is null when the selected
record has no explicit PID, or true/false for a metadata comparison with the
expected PID; it does not verify the connected device's product association.
"""
import argparse
import csv
import io
import json
from pathlib import Path
import posixpath
import re
import time
import xml.etree.ElementTree as ET
import zipfile

EXPECTED_PID = "3rhb25ktaqov4akw"
PID_PATTERN = re.compile(r"[A-Za-z0-9]{16}", re.ASCII)
UUID_PATTERN = re.compile(r"[A-Za-z0-9]{20}", re.ASCII)
KEY_PATTERN = re.compile(r"[A-Za-z0-9_-]{32}", re.ASCII)
PORT_PATTERN = re.compile(r"COM[1-9][0-9]{0,3}", re.ASCII | re.IGNORECASE)
READ_LIMIT = 16384
REPLY_TIMEOUT = 3.0


class Rejected(Exception):
    """Carries a fixed public error code, never untrusted input."""


class SafeParser(argparse.ArgumentParser):
    def error(self, message):
        # argparse's normal message can repeat arbitrary command arguments.
        raise Rejected("invalid_arguments")


def _field(key):
    if not isinstance(key, str):
        return None
    normalized = key.strip().lower().replace("_", "").replace("-", "")
    return {"uuid": "uuid", "authkey": "authkey", "auzkey": "authkey", "key": "authkey",
            "pid": "pid", "productid": "pid"}.get(normalized)


def _fields(pairs):
    result = {}
    for key, value in pairs:
        name = _field(key)
        if name:
            if name in result:
                raise Rejected("ambiguous_credentials")
            result[name] = value
    return result


def _xlsx_xml(archive, name):
    info = archive.getinfo(name)
    if info.file_size > 2 * 1024 * 1024:
        raise Rejected("input_too_large")
    raw = archive.read(name)
    # No DTDs/entities, external fetches, formula evaluation, or extraction.
    if b"<!DOCTYPE" in raw.upper() or b"<!ENTITY" in raw.upper():
        raise Rejected("invalid_xlsx_xml")
    return ET.fromstring(raw)


def _xlsx_target(target):
    name = posixpath.normpath(target.lstrip("/") if target.startswith("/") else posixpath.join("xl", target))
    if not name.startswith("xl/") or ":" in name:
        raise Rejected("invalid_xlsx_relationship")
    return name


def _xlsx_fields(path, selected_row):
    if path.stat().st_size > 16 * 1024 * 1024:
        raise Rejected("input_too_large")
    with zipfile.ZipFile(path, "r") as archive:
        names = archive.namelist()
        if len(names) > 128 or len(names) != len(set(names)):
            raise Rejected("invalid_xlsx_archive")
        workbook = _xlsx_xml(archive, "xl/workbook.xml")
        sheets = workbook.findall(".//{*}sheet")
        if len(sheets) != 1:
            raise Rejected("xlsx_single_sheet_required")
        relationship_id = next((value for key, value in sheets[0].attrib.items() if key.endswith("}id")), None)
        relationships = _xlsx_xml(archive, "xl/_rels/workbook.xml.rels")
        targets = {}
        shared_path = "xl/sharedStrings.xml"
        for rel in relationships:
            if rel.get("TargetMode") == "External":
                continue
            if rel.get("Id") in targets:
                raise Rejected("invalid_xlsx_relationship")
            if rel.get("Type", "").endswith("/worksheet"):
                targets[rel.get("Id")] = _xlsx_target(rel.get("Target", ""))
            elif rel.get("Type", "").endswith("/sharedStrings"):
                shared_path = _xlsx_target(rel.get("Target", ""))
        if not relationship_id or relationship_id not in targets:
            raise Rejected("invalid_xlsx_relationship")
        strings = []
        if shared_path in names:
            table = _xlsx_xml(archive, shared_path)
            strings = ["".join(node.text or "" for node in item.findall(".//{*}t")) for item in table.findall("{*}si")]
        worksheet = _xlsx_xml(archive, targets[relationship_id])
        if worksheet.findall(".//{*}f"):
            raise Rejected("xlsx_formulas_not_supported")
        rows = {}
        for row in worksheet.findall("{*}sheetData/{*}row"):
            number = int(row.get("r", "0"))
            if number < 1 or number > 1048576 or number in rows or len(rows) >= 4096:
                raise Rejected("invalid_xlsx_rows")
            cells = {}
            for cell in row.findall("{*}c"):
                reference = re.fullmatch(r"([A-Z]+)([1-9][0-9]*)", cell.get("r", ""), re.ASCII)
                if not reference or int(reference.group(2)) != number or reference.group(1) in cells:
                    raise Rejected("invalid_xlsx_cells")
                kind = cell.get("t")
                if kind == "inlineStr":
                    value = "".join(node.text or "" for node in cell.findall(".//{*}t"))
                else:
                    node = cell.find("{*}v")
                    value = node.text or "" if node is not None else ""
                    if kind == "s":
                        index = int(value)
                        if index < 0 or index >= len(strings):
                            raise Rejected("invalid_xlsx_strings")
                        value = strings[index]
                cells[reference.group(1)] = value
            rows[number] = cells
        columns = {}
        found = set()
        for column, header in rows.get(1, {}).items():
            name = _field(header)
            if name:
                if name in found:
                    raise Rejected("ambiguous_credentials")
                found.add(name)
                columns[column] = header
        if not {"uuid", "authkey"}.issubset(found):
            raise Rejected("xlsx_device_columns_required")
        records = {number: _fields((header, cells.get(column, "")) for column, header in columns.items())
                   for number, cells in rows.items() if number > 1 and any(cells.get(column, "") for column in columns)}
        if selected_row is None:
            if len(records) > 1:
                raise Rejected("xlsx_row_required")
            if not records:
                raise Rejected("xlsx_row_not_found")
            selected_row = next(iter(records))
        if selected_row not in records:
            raise Rejected("xlsx_row_not_found")
        return records[selected_row], selected_row


def _credentials(path, selected_row, expected_pid):
    try:
        path = Path(path)
        if path.suffix.lower() == ".xlsx":
            fields, selected_row = _xlsx_fields(path, selected_row)
        else:
            if selected_row is not None:
                raise Rejected("row_requires_xlsx")
            with path.open("rb") as stream:
                raw = stream.read(65537)
            if len(raw) > 65536:
                raise Rejected("input_too_large")
            text = raw.decode("utf-8-sig")
            if text.lstrip().startswith("{"):
                fields = json.loads(text, object_pairs_hook=_fields)
                if not isinstance(fields, dict):
                    raise Rejected("invalid_file_format")
            else:
                rows = list(csv.reader(io.StringIO(text, newline=""), strict=True))
                if len(rows) == 1 and len(rows[0]) in (2, 3):
                    fields = dict(zip(("uuid", "authkey", "pid"), rows[0]))
                elif len(rows) == 2 and len(rows[0]) == len(rows[1]):
                    fields = _fields(zip(rows[0], rows[1]))
                else:
                    raise Rejected("invalid_file_format")
        uuid, authkey = fields.get("uuid"), fields.get("authkey")
        if not isinstance(uuid, str) or not UUID_PATTERN.fullmatch(uuid):
            raise Rejected("invalid_uuid")
        if not isinstance(authkey, str) or not KEY_PATTERN.fullmatch(authkey):
            raise Rejected("invalid_authkey")
        if "pid" in fields and fields["pid"] != expected_pid:
            raise Rejected("pid_mismatch_configure_product")
        return uuid, authkey, selected_row, True if "pid" in fields else None
    except Rejected:
        raise
    except Exception:
        # JSON, CSV, filesystem, and codec exceptions may contain secret input.
        raise Rejected("unreadable_or_invalid_file") from None


def _read_reply(uart, complete):
    reply = bytearray()
    deadline = time.monotonic() + REPLY_TIMEOUT
    while time.monotonic() < deadline and len(reply) < READ_LIMIT:
        chunk = uart.read(min(512, READ_LIMIT - len(reply)))
        if chunk:
            reply.extend(chunk)
            if complete(bytes(reply)):
                return True
    return False


def _command(uart, command, complete):
    # Never let buffered startup output or an earlier command verify this one.
    uart.reset_input_buffer()
    if uart.write(command) != len(command):
        return False
    return _read_reply(uart, complete)


def _readback_matches(reply, uuid, authkey):
    # Only standalone credential lines in this separate auth-read response
    # count. Input echo, prefixed logs, and additional candidates are rejected.
    # Ignore the final unterminated fragment: a matching prefix may acquire
    # another byte in the next UART read and cease to be a standalone line.
    lines = reply.decode("ascii", errors="replace").replace("\r\n", "\n").replace("\r", "\n").split("\n")[:-1]
    candidates = [line for line in lines if UUID_PATTERN.fullmatch(line) or KEY_PATTERN.fullmatch(line)]
    return candidates == [uuid, authkey]


def _serial_factory(**kwargs):
    # Validation has no pyserial dependency and cannot accidentally open UART.
    import serial
    return serial.Serial(**kwargs)


def _result():
    return {"ok": False, "validated": False, "write_attempted": False,
            "write_ack": False, "verified": False, "auth_available": False,
            "pid_ok": None, "uuid_len": 0, "authkey_len": 0,
            "status": "rejected"}


def _execute(args, result, serial_factory):
    if args.probe and args.file:
        raise Rejected("probe_does_not_accept_credentials")
    if not PID_PATTERN.fullmatch(args.expected_pid):
        raise Rejected("invalid_expected_pid")
    if args.row is not None and (args.row < 2 or args.row > 1048576):
        raise Rejected("invalid_row")
    if args.probe and args.row is not None:
        raise Rejected("row_requires_xlsx")
    if args.port is not None:
        if not PORT_PATTERN.fullmatch(args.port):
            raise Rejected("invalid_port")
        result["port"] = args.port.upper()
    if (args.write or args.probe) and not args.port:
        raise Rejected("explicit_port_required")
    if args.port and not (args.write or args.probe):
        raise Rejected("explicit_serial_mode_required")
    uuid = authkey = None
    if not args.probe:
        if not args.file:
            raise Rejected("credential_file_required")
        uuid, authkey, selected_row, pid_ok = _credentials(args.file, args.row, args.expected_pid)
        result.update(validated=True, uuid_len=len(uuid), authkey_len=len(authkey), pid_ok=pid_ok)
        if selected_row is not None:
            result["row"] = selected_row
        if not args.write:
            result.update(ok=True, status="validated")
            return 0
    uart = None
    try:
        uart = serial_factory(port=result["port"], baudrate=115200, timeout=0.1, write_timeout=1.0)
        available = _command(uart, b"auth\r\n", lambda reply: b"Use like: auth " in reply)
        result["auth_available"] = available
        if not available:
            result.update(status="unavailable", error="auth_command_unconfirmed")
            return 2
        if args.probe:
            result.update(ok=True, status="probed")
            return 0
        # Mark the attempt before write(), whose failure can mean a partial send.
        # No branch retries the write or invokes reset/OTP/erase commands.
        uart.reset_input_buffer()
        result["write_attempted"] = True
        command = ("auth " + uuid + " " + authkey + " 0\r\n").encode("ascii")
        if uart.write(command) != len(command):
            result.update(status="unknown", error="write_outcome_unknown")
            return 3
        if not _read_reply(uart, lambda reply: b"Authorization write succeeds." in reply):
            result.update(status="unknown", error="write_outcome_unknown")
            return 3
        result["write_ack"] = True
        verified = _command(uart, b"auth-read 0\r\n", lambda reply: _readback_matches(reply, uuid, authkey))
        if not verified:
            result.update(status="unknown", error="readback_unverified")
            return 3
        result.update(ok=True, verified=True, status="verified")
        return 0
    except Exception:
        # Do not stringify any exception: serial drivers can include sent data.
        if result["write_attempted"]:
            result.update(status="unknown", error="write_outcome_unknown")
            return 3
        result.update(status="unavailable", error="serial_unavailable")
        return 2
    finally:
        if uart is not None:
            try:
                uart.close()
            except Exception:
                pass


def main(argv=None, *, serial_factory=None):
    parser = SafeParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file", nargs="?", help="Local JSON, one-record CSV, or XLSX; contents are never printed")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--write", action="store_true", help="Explicitly write KV storage 0, then verify a separate readback")
    modes.add_argument("--probe", action="store_true", help="Send only the empty auth command; does not read credentials")
    parser.add_argument("--port", help="Explicit Windows UART0 port, for example COM10")
    parser.add_argument("--row", type=int, help="Excel physical data row, for example 2; required when more than one record exists")
    parser.add_argument("--expected-pid", default=EXPECTED_PID, help="Compare explicit record PID metadata with this 16-character ID; absent metadata remains unknown")
    result = _result()
    try:
        code = _execute(parser.parse_args(argv), result, serial_factory or _serial_factory)
    except Rejected as error:
        fixed_code = error.args[0]
        result["error"] = fixed_code
        if fixed_code == "pid_mismatch_configure_product":
            result["pid_ok"] = False
        code = 2
    except KeyboardInterrupt:
        result.update(status="unknown" if result["write_attempted"] else "interrupted",
                      error="write_outcome_unknown" if result["write_attempted"] else "interrupted")
        code = 130
    except Exception:
        result["error"] = "operation_failed"
        code = 2
    print(json.dumps(result, sort_keys=True))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
