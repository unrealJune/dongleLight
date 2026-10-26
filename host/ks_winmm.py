"""Explicit Windows audio endpoint selection, capture, and playback via WinMM."""
import ctypes as C
from ctypes import wintypes as W
import threading
import time


class Format(C.Structure):
    _fields_ = [("tag", W.WORD), ("channels", W.WORD), ("rate", W.DWORD),
                ("bytes_sec", W.DWORD), ("align", W.WORD), ("bits", W.WORD), ("extra", W.WORD)]


class Header(C.Structure):
    _fields_ = [("data", C.c_void_p), ("length", W.DWORD), ("recorded", W.DWORD),
                ("user", C.c_size_t), ("flags", W.DWORD), ("loops", W.DWORD),
                ("next", C.c_void_p), ("reserved", C.c_size_t)]


class Caps(C.Structure):
    _fields_ = [("mid", W.WORD), ("pid", W.WORD), ("version", W.DWORD),
                ("name", W.WCHAR * 32), ("formats", W.DWORD),
                ("channels", W.WORD), ("reserved", W.WORD)]


def library():
    if not hasattr(C, "WinDLL"):
        raise OSError("Windows audio requires Windows")
    dll = C.WinDLL("winmm")
    for direction in ("In", "Out"):
        for method in ("Open", "Close", "Reset", "PrepareHeader", "UnprepareHeader"):
            fn = getattr(dll, "wave" + direction + method)
            fn.restype = W.UINT
        getattr(dll, "wave" + direction + "Open").argtypes = [
            C.POINTER(C.c_void_p), W.UINT, C.POINTER(Format), C.c_size_t, C.c_size_t, W.DWORD]
        for method in ("Close", "Reset"):
            getattr(dll, "wave" + direction + method).argtypes = [C.c_void_p]
        for method in ("PrepareHeader", "UnprepareHeader"):
            getattr(dll, "wave" + direction + method).argtypes = [C.c_void_p, C.POINTER(Header), W.UINT]
    for name in ("waveInStart", "waveInStop"):
        getattr(dll, name).argtypes = [C.c_void_p]
    for name in ("waveInAddBuffer", "waveOutWrite"):
        getattr(dll, name).argtypes = [C.c_void_p, C.POINTER(Header), W.UINT]
    return dll


def check(result, action):
    if result:
        raise OSError(f"{action}: Windows audio error {result}; check the selected device and RDP routing")


def devices():
    dll = library()
    result = {}
    for direction, key in (("In", "inputs"), ("Out", "outputs")):
        entries = [dict(id=-1, name="Windows default device")]
        for index in range(getattr(dll, "wave" + direction + "GetNumDevs")()):
            # WAVEOUTCAPS includes one extra DWORD (dwSupport).
            raw = C.create_string_buffer(C.sizeof(Caps) + 4)
            error = getattr(dll, "wave" + direction + "GetDevCapsW")(index, raw, len(raw))
            if not error:
                caps = C.cast(raw, C.POINTER(Caps)).contents
                entries.append(dict(id=index, name=caps.name))
        result[key] = entries
    return result


class Audio:
    def __init__(self, output, input_device, callback, on_error):
        self.dll = library()
        self.output = int(output)
        self.callback, self.on_error = callback, on_error
        self.failed = None
        self.play_lock = threading.Lock()
        self.stop = threading.Event()
        self.capture = C.c_void_p()
        self.headers = []
        fmt = Format(1, 1, 48000, 96000, 2, 16, 0)
        check(self.dll.waveInOpen(C.byref(self.capture), int(input_device) & 0xFFFFFFFF,
                                 C.byref(fmt), 0, 0, 0), "Open microphone")
        try:
            for _ in range(8):
                buf = C.create_string_buffer(9600)
                header = Header(C.cast(buf, C.c_void_p), len(buf), 0, 0, 0, 0, None, 0)
                check(self.dll.waveInPrepareHeader(self.capture, C.byref(header), C.sizeof(header)), "Prepare capture")
                self.headers.append((header, buf))
                check(self.dll.waveInAddBuffer(self.capture, C.byref(header), C.sizeof(header)), "Queue capture")
            check(self.dll.waveInStart(self.capture), "Start microphone")
        except Exception:
            self.close()
            raise
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        try:
            while not self.stop.wait(0.01):
                for header, buf in self.headers:
                    if header.flags & 1:
                        self.callback(buf.raw[:header.recorded])
                        header.recorded = 0
                        check(self.dll.waveInAddBuffer(self.capture, C.byref(header), C.sizeof(header)), "Requeue capture")
        except Exception as error:
            self.failed = error
            self.on_error(error)

    def play(self, pcm):
        if self.failed is not None:
            raise self.failed
        with self.play_lock:
            handle = C.c_void_p()
            fmt = Format(1, 1, 48000, 96000, 2, 16, 0)
            try:
                check(self.dll.waveOutOpen(C.byref(handle), self.output & 0xFFFFFFFF,
                                          C.byref(fmt), 0, 0, 0), "Open playback")
            except Exception as error:
                self.failed = error
                raise
            buf = C.create_string_buffer(pcm)
            header = Header(C.cast(buf, C.c_void_p), len(pcm), 0, 0, 0, 0, None, 0)
            prepared = False
            completed = False
            try:
                check(self.dll.waveOutPrepareHeader(handle, C.byref(header), C.sizeof(header)), "Prepare playback")
                prepared = True
                check(self.dll.waveOutWrite(handle, C.byref(header), C.sizeof(header)), "Play command")
                deadline = time.monotonic() + len(pcm) / 96000 + 10
                while not header.flags & 1:
                    if self.stop.wait(0.02) or time.monotonic() > deadline:
                        raise TimeoutError("Audio playback stalled")
                completed = True
            except OSError as error:
                self.failed = error
                raise
            finally:
                if not completed:
                    self.dll.waveOutReset(handle)
                if prepared:
                    self.dll.waveOutUnprepareHeader(handle, C.byref(header), C.sizeof(header))
                self.dll.waveOutClose(handle)

    def close(self):
        self.stop.set()
        thread = getattr(self, "thread", None)
        if thread and thread is not threading.current_thread():
            thread.join(timeout=10)
        if self.capture:
            self.dll.waveInReset(self.capture)
            for header, _ in self.headers:
                self.dll.waveInUnprepareHeader(self.capture, C.byref(header), C.sizeof(header))
            self.dll.waveInClose(self.capture)
            self.capture = C.c_void_p()
