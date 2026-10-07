#!/usr/bin/env python3
"""Push this computer's clock into the PetWatch RTC (DS3231).

Run it whenever the feeder is plugged into a PC — Linux, Windows or macOS:

    python3 rtc_sync.py              # auto-detects the Arduino
    python3 rtc_sync.py --port COM3  # or name the port yourself

It opens the serial port (which resets the UNO), waits for the boot banner,
sends the existing `settime y m d hh mm ss` command and exits once the
feeder answers `OK,time set`. From then on the DS3231 keeps the time on its
own — power bank or unplugged, no further syncing needed.

No dependencies: stock Python 3 on any OS. pyserial is used when it is
already installed; otherwise the built-in backends are used — Win32 COM
via ctypes on Windows, the termios reader from petwatch_gateway.py on
Linux/macOS.
"""

import argparse
import datetime
import os
import sys
import time

BAUD = 9600
BOOT_TIMEOUT = 10
ACK_TIMEOUT = 2.5
RETRIES = 3
ARDUINO_VIDS = (0x2341, 0x2A03)


def log(msg):
    print(f"[sync] {msg}", flush=True)


def verbose_log(args, msg):
    if args.verbose:
        print(f"[serial] {msg}", flush=True)


class _BufferedPort:
    def __init__(self):
        self.buf = bytearray()

    def _read(self, maxbytes):
        raise NotImplementedError

    def readline(self, timeout=0.3):
        end = time.time() + timeout
        while b"\n" not in self.buf:
            if time.time() >= end:
                return None
            chunk = self._read(256)
            if chunk:
                self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").strip()


class PySerialPort(_BufferedPort):
    def __init__(self, path):
        import serial
        super().__init__()
        self.serial = serial.Serial(path, BAUD, timeout=0.2)
        try:
            self.serial.reset_input_buffer()
        except Exception:
            pass

    def _read(self, maxbytes):
        return self.serial.read(maxbytes)

    def write(self, text):
        self.serial.write((text + "\n").encode())
        self.serial.flush()

    def close(self):
        self.serial.close()


def _win32():
    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                wintypes.DWORD, wintypes.LPVOID,
                                wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.GetCommState.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
    k32.SetCommState.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
    k32.SetCommTimeouts.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
    k32.PurgeComm.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    for name in ("CloseHandle", "GetCommState", "SetCommState",
                 "SetCommTimeouts", "PurgeComm", "ReadFile", "WriteFile"):
        fn = getattr(k32, name)
        fn.restype = wintypes.BOOL
    k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                             wintypes.LPDWORD, wintypes.LPVOID]
    k32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
                              wintypes.LPDWORD, wintypes.LPVOID]
    return ctypes, wintypes, k32


def _open_win_handle(ctypes, k32, name):
    prefix = "\\\\.\\"   # Windows device path prefix, e.g. \\.\COM3
    full = name if name.startswith(prefix) else prefix + name
    h = k32.CreateFileW(full, 0xC0000000, 0, None, 3, 0, None)
    invalid = (1 << (8 * ctypes.sizeof(ctypes.c_void_p))) - 1
    if h is None or h == 0 or h == invalid or h == -1:
        raise OSError(ctypes.get_last_error(), f"cannot open {name}")
    return h


class WinSerialPort(_BufferedPort):
    def __init__(self, path):
        ctypes, wintypes, k32 = _win32()
        super().__init__()
        self.ctypes, self.wintypes, self.k32 = ctypes, wintypes, k32
        self.handle = _open_win_handle(ctypes, k32, path)

        class DCB(ctypes.Structure):
            _fields_ = [
                ("DCBlength", wintypes.DWORD),
                ("BaudRate", wintypes.DWORD),
                ("fBinary", wintypes.DWORD, 1),
                ("fParity", wintypes.DWORD, 1),
                ("fOutxCtsFlow", wintypes.DWORD, 1),
                ("fOutxDsrFlow", wintypes.DWORD, 1),
                ("fDtrControl", wintypes.DWORD, 2),
                ("fDsrSensitivity", wintypes.DWORD, 1),
                ("fTXContinueOnXoff", wintypes.DWORD, 1),
                ("fOutX", wintypes.DWORD, 1),
                ("fInX", wintypes.DWORD, 1),
                ("fErrorChar", wintypes.DWORD, 1),
                ("fNull", wintypes.DWORD, 1),
                ("fRtsControl", wintypes.DWORD, 2),
                ("fAbortOnError", wintypes.DWORD, 1),
                ("fDummy2", wintypes.DWORD, 17),
                ("wReserved", wintypes.WORD),
                ("XonLim", wintypes.WORD),
                ("XoffLim", wintypes.WORD),
                ("ByteSize", wintypes.BYTE),
                ("Parity", wintypes.BYTE),
                ("StopBits", wintypes.BYTE),
                ("XonChar", ctypes.c_char),
                ("XoffChar", ctypes.c_char),
                ("ErrorChar", ctypes.c_char),
                ("EofChar", ctypes.c_char),
                ("EvtChar", ctypes.c_char),
                ("wReserved1", wintypes.WORD),
            ]

        class COMMTIMEOUTS(ctypes.Structure):
            _fields_ = [
                ("ReadIntervalTimeout", wintypes.DWORD),
                ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
                ("ReadTotalTimeoutConstant", wintypes.DWORD),
                ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
                ("WriteTotalTimeoutConstant", wintypes.DWORD),
            ]

        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        if not k32.GetCommState(self.handle, ctypes.byref(dcb)):
            raise OSError(ctypes.get_last_error(), "GetCommState failed")
        dcb.BaudRate = BAUD
        dcb.fBinary = 1
        dcb.fParity = 0
        dcb.fOutxCtsFlow = 0
        dcb.fOutxDsrFlow = 0
        dcb.fDtrControl = 1
        dcb.fDsrSensitivity = 0
        dcb.fTXContinueOnXoff = 1
        dcb.fOutX = 0
        dcb.fInX = 0
        dcb.fRtsControl = 1
        dcb.fAbortOnError = 0
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        if not k32.SetCommState(self.handle, ctypes.byref(dcb)):
            raise OSError(ctypes.get_last_error(), "SetCommState failed")

        to = COMMTIMEOUTS()
        to.ReadIntervalTimeout = 0
        to.ReadTotalTimeoutMultiplier = 0
        to.ReadTotalTimeoutConstant = 50
        to.WriteTotalTimeoutMultiplier = 0
        to.WriteTotalTimeoutConstant = 500
        k32.SetCommTimeouts(self.handle, ctypes.byref(to))
        k32.PurgeComm(self.handle, 0x000C)

    def _read(self, maxbytes):
        ctypes, wintypes, k32 = self.ctypes, self.wintypes, self.k32
        buf = ctypes.create_string_buffer(maxbytes)
        n = wintypes.DWORD()
        if not k32.ReadFile(self.handle, buf, maxbytes, ctypes.byref(n), None):
            raise OSError(ctypes.get_last_error(), "ReadFile failed")
        return bytes(buf.raw[:n.value])

    def write(self, text):
        ctypes, wintypes, k32 = self.ctypes, self.wintypes, self.k32
        data = (text + "\n").encode()
        n = wintypes.DWORD()
        if not k32.WriteFile(self.handle, data, len(data), ctypes.byref(n), None):
            raise OSError(ctypes.get_last_error(), "WriteFile failed")

    def close(self):
        self.k32.CloseHandle(self.handle)


class TermiosPort:
    def __init__(self, path):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from petwatch_gateway import SerialLine
        self.line = SerialLine(path)
        self.line.open()

    def readline(self, timeout=0.3):
        return self.line.readline(timeout)

    def write(self, text):
        self.line.write(text)

    def close(self):
        try:
            os.close(self.line.fd)
        except OSError:
            pass


def have_pyserial():
    try:
        import serial  # noqa: F401
        return True
    except ImportError:
        return False


def find_port(override):
    if override:
        return override
    if have_pyserial():
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        for p in ports:
            if p.vid in ARDUINO_VIDS:
                log(f"port {p.device} (Arduino VID)")
                return p.device
        for p in ports:
            if "arduino" in (p.manufacturer or "").lower():
                log(f"port {p.device} ({p.manufacturer})")
                return p.device
        if len(ports) == 1:
            log(f"port {ports[0].device} (only serial device)")
            return ports[0].device
        if not ports:
            log("no serial ports found — plug the feeder in, or pass --port")
            sys.exit(1)
        names = ", ".join(p.device for p in ports)
        log(f"several ports found ({names}) — pass --port")
        sys.exit(1)
    if sys.platform == "win32":
        found = _probe_com_ports()
        if len(found) == 1:
            log(f"port {found[0]}")
            return found[0]
        if not found:
            log("no serial ports found — plug the feeder in, or pass --port")
            sys.exit(1)
        log(f"several ports found ({', '.join(found)}) — pass --port")
        sys.exit(1)
    if os.name != "posix":
        log("unsupported OS — pass --port")
        sys.exit(1)
    for candidate in ("/dev/ttyUSB0", "/dev/ttyACM0"):
        if os.path.exists(candidate):
            log(f"port {candidate}")
            return candidate
    log("no serial device found — plug the feeder in, or pass --port")
    sys.exit(1)


def _probe_com_ports():
    ctypes, wintypes, k32 = _win32()
    found = []
    for i in range(1, 41):
        name = f"COM{i}"
        try:
            h = _open_win_handle(ctypes, k32, name)
        except OSError:
            continue
        k32.CloseHandle(h)
        found.append(name)
    return found


def open_port(path, args):
    if have_pyserial():
        return PySerialPort(path)
    if sys.platform == "win32":
        return WinSerialPort(path)
    if os.name != "posix":
        log("unsupported OS — pass --port")
        sys.exit(1)
    log("pyserial not installed, using termios fallback (pip install pyserial for any OS)")
    return TermiosPort(path)


def wait_for_banner(port, args):
    log(f"waiting up to {BOOT_TIMEOUT}s for the boot banner")
    deadline = time.time() + BOOT_TIMEOUT
    saw_rtc_fail = False
    while time.time() < deadline:
        line = port.readline(0.3)
        if line is None:
            continue
        verbose_log(args, line)
        if "PetWatch Ready" in line:
            return True
        if "ERR,rtc_not_found" in line:
            saw_rtc_fail = True
    if saw_rtc_fail:
        log("warning: feeder reported RTC FAIL at boot — sync may not stick")
    log("no boot banner (port may not have reset the board) — syncing anyway")
    return False


def sync_once(port, args, stamp):
    cmd = "settime %d %d %d %d %d %d" % (
        stamp.year, stamp.month, stamp.day,
        stamp.hour, stamp.minute, stamp.second,
    )
    verbose_log(args, f"-> {cmd}")
    port.write(cmd)
    deadline = time.time() + ACK_TIMEOUT
    while time.time() < deadline:
        line = port.readline(0.3)
        if line is None:
            continue
        verbose_log(args, line)
        if line.startswith("OK,time"):
            return True
    return False


def main():

    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--port", help="serial port (default: auto-detect the Arduino)")
    ap.add_argument("--verbose", action="store_true", help="echo every serial line")
    args = ap.parse_args()

    path = find_port(args.port)
    stamp = datetime.datetime.now()
    log(f"this computer's clock: {stamp:%Y-%m-%d %H:%M:%S}")

    try:
        port = open_port(path, args)
    except Exception as e:
        log(f"cannot open {path}: {e}")
        sys.exit(1)

    try:
        wait_for_banner(port, args)
        for attempt in range(1, RETRIES + 1):
            if sync_once(port, args, stamp):
                log(f"synced: {stamp:%Y-%m-%d %H:%M:%S}  (OK,time set)")
                log("RTC now keeps the time on the power bank — unplug and demo")
                return
            log(f"no ack, retry {attempt}/{RETRIES}")
    finally:
        port.close()

    log("feeder did not answer `OK,time set` — check wiring/RTC (try --verbose)")
    sys.exit(1)


if __name__ == "__main__":
    main()
