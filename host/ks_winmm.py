"""Explicit Windows audio endpoint selection, capture, and playback via WinMM and MsRdcWebRTCSvc."""
import array
import base64
from collections import deque
import ctypes as C
from ctypes import wintypes as W
import hashlib
import hmac
import json
import math
import os
from pathlib import Path
import re
import select
import socket
import struct
import sys
import threading
import time
import uuid

WEBRTC_ID_BASE = 1000
_WEBRTC_SERVICE_ID = "388FE4DA-5AAD-C078-FCD5-2DB6ADE9C174"
_WEBRTC_VERSION = "1.1.2602.23001"
_STUN_COOKIE = 0x2112A442
_LOCAL_SSRC = 0x12345678


def _ulaw_encode_sample(s):
    s = max(-32768, min(32767, s))
    sign = 0x80 if s >= 0 else 0
    if s < 0:
        s = -s
    s = min(32635, s) + 0x84
    exp = 7
    for e in range(7, 0, -1):
        if s & (1 << (e + 7)):
            exp = e
            break
    else:
        exp = 0
    mant = (s >> (exp + 3)) & 0x0F
    return sign | ((~((exp << 4) | mant)) & 0x7F)


_ULAW_ENC = bytes(_ulaw_encode_sample(s - 32768) for s in range(65536))
_ULAW_DEC = tuple(
    -((((~b & 0x0F) << 3) + 0x84) << ((~b >> 4) & 7)) + 0x84 if (~b & 0x80)
    else ((((~b & 0x0F) << 3) + 0x84) << ((~b >> 4) & 7)) - 0x84
    for b in range(256)
)

_SINC_PHASES = tuple(
    tuple(
        1.0 if abs(r / 6.0 - k) < 1e-9 else
        math.sin(math.pi * (r / 6.0 - k)) / (math.pi * (r / 6.0 - k)) *
        (0.54 + 0.46 * math.cos(math.pi * (r / 6.0 - k) / 9.0))
        for k in range(-8, 9)
    )
    for r in range(6)
)

_COH_BASIS_8K = tuple(
    (
        tuple(math.cos(2 * math.pi * f * i / 8000.0) for i in range(40)),
        tuple(math.sin(2 * math.pi * f * i / 8000.0) for i in range(40)),
    )
    for f in (1000.0, 1800.0)
)


def _pkt_is_tone(pkt):
    avg = sum(abs(x) for x in pkt) / len(pkt)
    if avg < 3.0:
        return False
    if avg >= 60.0:
        return True
    coh_sum = 0.0
    for sub in range(0, len(pkt), 40):
        blk = pkt[sub:sub + 40]
        e_tot = sum(x * x for x in blk) + 1.0
        best_e = 0.0
        for co, si in _COH_BASIS_8K:
            re_v = sum(x * y for x, y in zip(blk, co))
            im_v = sum(x * y for x, y in zip(blk, si))
            best_e = max(best_e, 2.0 * (re_v * re_v + im_v * im_v) / len(blk))
        coh_sum += best_e / e_tot
    return (coh_sum / (len(pkt) // 40)) >= 0.095


def _webrtc_cache_path():
    base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
    return Path(base) / "DongleLight" / "webrtc_devices.json"


def _webrtc_reg_config():
    if sys.platform != "win32":
        return None
    try:
        import winreg
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Control\Terminal Server\AddIns\WebRTC Redirector",
        ) as key:
            raw, _ = winreg.QueryValueEx(key, "Config")
            cfg = json.loads(raw)
            if "port" in cfg and "key" in cfg:
                return cfg
    except Exception:
        pass
    return None


class _WebRtcWs:
    def __init__(self, port, raw_key, timeout=2.5):
        self.sock = socket.create_connection(("127.0.0.1", int(port)), timeout=timeout)
        sec_key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = (
            f"GET / HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {sec_key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise OSError("MsRdcWebRTCSvc closed connection during WebSocket upgrade")
            buf += chunk
        self._buf = buf.split(b"\r\n\r\n", 1)[1]
        self._call_id = 0
        self.raw_key = raw_key

    def _recv_exact(self, n):
        while len(self._buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("MsRdcWebRTCSvc WebSocket EOF")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def send_json(self, obj):
        payload = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        mask = os.urandom(4)
        header = bytearray([0x81])
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", n))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", n))
        header.extend(mask)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def call(self, obj_type, name, args=None, obj_id=None):
        self._call_id += 1
        msg = {
            "rpcObjectType": obj_type,
            "rpcName": name,
            "rpcArgs": [] if args is None else args,
            "rpcCallId": self._call_id,
        }
        if obj_id is not None:
            msg["rpcObjectId"] = obj_id
        self.send_json(msg)
        return self._call_id

    def recv_msg(self, timeout=2.0):
        self.sock.settimeout(timeout)
        while True:
            b0, b1 = self._recv_exact(2)
            opcode = b0 & 0x0F
            length = b1 & 0x7F
            if length == 126:
                length, = struct.unpack("!H", self._recv_exact(2))
            elif length == 127:
                length, = struct.unpack("!Q", self._recv_exact(8))
            if b1 & 0x80:
                mask = self._recv_exact(4)
                raw = bytes(b ^ mask[i & 3] for i, b in enumerate(self._recv_exact(length)))
            else:
                raw = self._recv_exact(length)
            if opcode == 0x8:
                raise EOFError("MsRdcWebRTCSvc closed WebSocket")
            if opcode == 0x9:
                # Reply with masked Pong frame.
                m = os.urandom(4)
                self.sock.sendall(bytes([0x8A, 0x80 | len(raw)]) + m + bytes(b ^ m[i & 3] for i, b in enumerate(raw)))
                continue
            txt = raw.decode("utf-8", errors="ignore").strip("\x00 \r\n\t")
            end = txt.rfind("}")
            if end >= 0:
                txt = txt[:end + 1]
            if txt:
                return json.loads(txt)

    def authenticate(self, timeout=2.0):
        u = uuid.uuid4().hex
        ts = str(int(time.time() * 1000))
        sig = hmac.new(self.raw_key, f"{u}|{ts}".encode("ascii"), hashlib.sha256).hexdigest()
        cid = self.call("RDWebRTCRedirector", "rtcSession", [u, ts, sig, _WEBRTC_VERSION])
        got_ack = False
        got_session = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and not (got_ack and got_session):
            msg = self.recv_msg(timeout=max(0.1, deadline - time.monotonic()))
            if msg.get("rpcCallId") == cid:
                if "error" in msg:
                    raise OSError(f"MsRdcWebRTCSvc rtcSession rejected: {msg['error']}")
                got_ack = True
            elif msg.get("rpcEventName") == "sessioninfo":
                got_session = True
            elif msg.get("rpcEventName") == "virtualchannelerror":
                raise OSError("RDP WebRTC virtual channel is not connected; ensure Remote Desktop is active")
        if not (got_ack and got_session):
            raise TimeoutError("Timed out waiting for MsRdcWebRTCSvc sessioninfo")

    def close(self):
        try:
            self.sock.sendall(bytes([0x88, 0x80, 0, 0, 0, 0]))
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


def _webrtc_raw_devices():
    cfg = _webrtc_reg_config()
    cache_file = _webrtc_cache_path()
    if cfg is not None:
        raw_key = bytes.fromhex((_WEBRTC_SERVICE_ID + cfg["key"]).replace("-", ""))
        ws = None
        try:
            ws = _WebRtcWs(cfg["port"], raw_key, timeout=1.5)
            ws.authenticate(timeout=1.5)
            cid = ws.call("MediaDevices", "enumerateDevices", [])
            deadline = time.monotonic() + 1.5
            while time.monotonic() < deadline:
                msg = ws.recv_msg(timeout=max(0.1, deadline - time.monotonic()))
                if msg.get("rpcCallId") == cid and isinstance(msg.get("result"), list):
                    devs = msg["result"]
                    try:
                        cache_file.parent.mkdir(parents=True, exist_ok=True)
                        tmp = cache_file.with_suffix(".tmp")
                        tmp.write_text(json.dumps(devs, indent=2), encoding="utf-8")
                        tmp.replace(cache_file)
                    except OSError:
                        pass
                    return devs
        except Exception:
            pass
        finally:
            if ws is not None:
                ws.close()
    try:
        if cache_file.exists():
            devs = json.loads(cache_file.read_text(encoding="utf-8"))
            if isinstance(devs, list):
                return devs
    except Exception:
        pass
    return []


def _webrtc_catalog():
    raw = _webrtc_raw_devices()
    inputs, outputs = [], []
    seen_in, seen_out = set(), set()
    # Prefer physical endpoint GUIDs over "default"/"communications" aliases when labels match.
    ordered = sorted(raw, key=lambda d: 1 if d.get("deviceId") in ("default", "communications") else 0)
    for d in ordered:
        kind = d.get("kind")
        label = (d.get("label") or "").strip()
        dev_id = (d.get("deviceId") or "").strip()
        if not label or not dev_id:
            continue
        if kind == "audioinput" and label not in seen_in:
            seen_in.add(label)
            inputs.append(dict(id=WEBRTC_ID_BASE + len(inputs), name=label, deviceId=dev_id, label=label))
        elif kind == "audiooutput" and label not in seen_out:
            seen_out.add(label)
            outputs.append(dict(id=WEBRTC_ID_BASE + len(outputs), name=label, deviceId=dev_id, label=label))
    return inputs, outputs


def _load_ice_servers():
    try:
        base = Path(os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local"))
        conf_dir = base / r"Packages\MSTeams_8wekyb3d8bbwe\LocalCache\Microsoft\MSTeams\Logs\sc-tfw\CS_sc-tfw"
        if conf_dir.exists():
            for p in sorted(conf_dir.glob("*.conf"), key=lambda x: x.stat().st_mtime, reverse=True):
                txt = p.read_text(encoding="utf-8", errors="ignore")
                m = re.search(r'"Lib/MDN/RelayTokenCache"\s*:\s*"(\{.*?\})"(?:,|})', txt)
                if m:
                    tok = json.loads(m.group(1).replace('\\"', '"'))
                    u = tok.get("username")
                    pw = tok.get("password")
                    if u and pw:
                        return [{
                            "urls": [
                                "turn:worldazr0.tr.teams.microsoft.com:3478?transport=udp",
                                "turn:gateway-world.r0.az.relay.teams.cloud.microsoft:3478?transport=udp",
                                "stun:stun.l.google.com:19302",
                            ],
                            "username": u,
                            "credential": pw,
                        }]
    except Exception:
        pass
    return [{"urls": ["stun:stun.l.google.com:19302"]}]


class _OpenSslDtlsSrtp:
    def __init__(self):
        dll_dir = Path(sys.base_prefix) / "DLLs"
        self.crypto = C.CDLL(str(dll_dir / "libcrypto-3.dll"))
        self.ssl_lib = C.CDLL(str(dll_dir / "libssl-3.dll"))
        self._setup_prototypes()
        self.pkey = self.crypto.EVP_PKEY_Q_keygen(None, None, b"RSA", C.c_size_t(2048))
        if not self.pkey:
            raise OSError("EVP_PKEY_Q_keygen failed")
        self.x509 = self.crypto.X509_new()
        self.crypto.X509_set_version(self.x509, 2)
        self.crypto.ASN1_INTEGER_set(self.crypto.X509_get_serialNumber(self.x509), 1)
        self.crypto.X509_gmtime_adj(self.crypto.X509_getm_notBefore(self.x509), -3600)
        self.crypto.X509_gmtime_adj(self.crypto.X509_getm_notAfter(self.x509), 86400 * 30)
        self.crypto.X509_set_pubkey(self.x509, self.pkey)
        name = self.crypto.X509_get_subject_name(self.x509)
        self.crypto.X509_NAME_add_entry_by_txt(name, b"CN", 0x1001, b"dongleLight", -1, -1, 0)
        self.crypto.X509_set_issuer_name(self.x509, name)
        self.crypto.X509_sign(self.x509, self.pkey, self.crypto.EVP_sha256())
        md = (C.c_ubyte * 32)()
        md_len = C.c_uint(32)
        self.crypto.X509_digest(self.x509, self.crypto.EVP_sha256(), md, C.byref(md_len))
        self.fingerprint = ":".join(f"{b:02X}" for b in md[:md_len.value])

        self.ctx = self.ssl_lib.SSL_CTX_new(self.ssl_lib.DTLS_method())
        self.ssl_lib.SSL_CTX_use_certificate(self.ctx, self.x509)
        self.ssl_lib.SSL_CTX_use_PrivateKey(self.ctx, self.pkey)
        self.ssl_lib.SSL_CTX_set_tlsext_use_srtp(self.ctx, b"SRTP_AES128_CM_SHA1_80")
        self.ssl = self.ssl_lib.SSL_new(self.ctx)
        self.rbio = self.crypto.BIO_new(self.crypto.BIO_s_mem())
        self.wbio = self.crypto.BIO_new(self.crypto.BIO_s_mem())
        self.ssl_lib.SSL_set_bio(self.ssl, self.rbio, self.wbio)
        self.ssl_lib.SSL_set_connect_state(self.ssl)
        self.aes_ctr = self.crypto.EVP_aes_128_ctr()
        self.ready = False
        self.rx_ke = self.rx_ks = None
        self.tx_ke = self.tx_ka = self.tx_ks = None

    def _setup_prototypes(self):
        self.crypto.EVP_PKEY_Q_keygen.restype = C.c_void_p
        self.crypto.EVP_PKEY_Q_keygen.argtypes = [C.c_void_p, C.c_void_p, C.c_char_p, C.c_size_t]
        self.crypto.EVP_PKEY_free.argtypes = [C.c_void_p]
        self.crypto.X509_new.restype = C.c_void_p
        self.crypto.X509_free.argtypes = [C.c_void_p]
        self.crypto.X509_set_version.argtypes = [C.c_void_p, C.c_long]
        self.crypto.X509_get_serialNumber.restype = C.c_void_p
        self.crypto.X509_get_serialNumber.argtypes = [C.c_void_p]
        self.crypto.ASN1_INTEGER_set.argtypes = [C.c_void_p, C.c_long]
        self.crypto.X509_gmtime_adj.restype = C.c_void_p
        self.crypto.X509_gmtime_adj.argtypes = [C.c_void_p, C.c_long]
        self.crypto.X509_getm_notBefore.restype = C.c_void_p
        self.crypto.X509_getm_notBefore.argtypes = [C.c_void_p]
        self.crypto.X509_getm_notAfter.restype = C.c_void_p
        self.crypto.X509_getm_notAfter.argtypes = [C.c_void_p]
        self.crypto.X509_set_pubkey.argtypes = [C.c_void_p, C.c_void_p]
        self.crypto.X509_get_subject_name.restype = C.c_void_p
        self.crypto.X509_get_subject_name.argtypes = [C.c_void_p]
        self.crypto.X509_NAME_add_entry_by_txt.argtypes = [
            C.c_void_p, C.c_char_p, C.c_int, C.c_char_p, C.c_int, C.c_int, C.c_int
        ]
        self.crypto.X509_set_issuer_name.argtypes = [C.c_void_p, C.c_void_p]
        self.crypto.EVP_sha256.restype = C.c_void_p
        self.crypto.X509_sign.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
        self.crypto.X509_digest.argtypes = [
            C.c_void_p, C.c_void_p, C.POINTER(C.c_ubyte), C.POINTER(C.c_uint)
        ]
        self.ssl_lib.DTLS_method.restype = C.c_void_p
        self.ssl_lib.SSL_CTX_new.restype = C.c_void_p
        self.ssl_lib.SSL_CTX_new.argtypes = [C.c_void_p]
        self.ssl_lib.SSL_CTX_free.argtypes = [C.c_void_p]
        self.ssl_lib.SSL_CTX_use_certificate.argtypes = [C.c_void_p, C.c_void_p]
        self.ssl_lib.SSL_CTX_use_PrivateKey.argtypes = [C.c_void_p, C.c_void_p]
        self.ssl_lib.SSL_CTX_set_tlsext_use_srtp.argtypes = [C.c_void_p, C.c_char_p]
        self.ssl_lib.SSL_new.restype = C.c_void_p
        self.ssl_lib.SSL_new.argtypes = [C.c_void_p]
        self.ssl_lib.SSL_free.argtypes = [C.c_void_p]
        self.ssl_lib.SSL_set_bio.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
        self.ssl_lib.SSL_set_connect_state.argtypes = [C.c_void_p]
        self.ssl_lib.SSL_do_handshake.argtypes = [C.c_void_p]
        self.ssl_lib.SSL_do_handshake.restype = C.c_int
        self.ssl_lib.SSL_export_keying_material.argtypes = [
            C.c_void_p, C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t, C.c_void_p, C.c_size_t, C.c_int
        ]
        self.ssl_lib.SSL_export_keying_material.restype = C.c_int
        self.crypto.BIO_s_mem.restype = C.c_void_p
        self.crypto.BIO_new.restype = C.c_void_p
        self.crypto.BIO_new.argtypes = [C.c_void_p]
        self.crypto.BIO_write.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
        self.crypto.BIO_write.restype = C.c_int
        self.crypto.BIO_read.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
        self.crypto.BIO_read.restype = C.c_int
        self.crypto.EVP_CIPHER_CTX_new.restype = C.c_void_p
        self.crypto.EVP_CIPHER_CTX_free.argtypes = [C.c_void_p]
        self.crypto.EVP_aes_128_ctr.restype = C.c_void_p
        self.crypto.EVP_EncryptInit_ex.argtypes = [
            C.c_void_p, C.c_void_p, C.c_void_p, C.c_char_p, C.c_char_p
        ]
        self.crypto.EVP_EncryptUpdate.argtypes = [
            C.c_void_p, C.c_char_p, C.POINTER(C.c_int), C.c_char_p, C.c_int
        ]

    def aes_128_ctr(self, key, iv, data):
        ctx = self.crypto.EVP_CIPHER_CTX_new()
        try:
            self.crypto.EVP_EncryptInit_ex(ctx, self.aes_ctr, None, key, iv)
            out = C.create_string_buffer(len(data))
            out_len = C.c_int(0)
            self.crypto.EVP_EncryptUpdate(ctx, out, C.byref(out_len), data, len(data))
            return out.raw[:out_len.value]
        finally:
            self.crypto.EVP_CIPHER_CTX_free(ctx)

    def _derive_srtp_key(self, master_key, master_salt, label, out_len):
        salt_int = int.from_bytes(master_salt, "big") ^ (label << 48)
        iv = salt_int.to_bytes(14, "big") + b"\x00\x00"
        return self.aes_128_ctr(master_key, iv, b"\x00" * out_len)

    def step_handshake(self, incoming=None):
        if incoming:
            self.crypto.BIO_write(self.rbio, incoming, len(incoming))
        rc = self.ssl_lib.SSL_do_handshake(self.ssl)
        out_packets = []
        buf = C.create_string_buffer(4096)
        while True:
            n = self.crypto.BIO_read(self.wbio, buf, 4096)
            if n <= 0:
                break
            out_packets.append(buf.raw[:n])
        if rc == 1 and not self.ready:
            km = C.create_string_buffer(60)
            if self.ssl_lib.SSL_export_keying_material(
                self.ssl, km, 60, b"EXTRACTOR-dtls_srtp", 19, None, 0, 0
            ) == 1:
                raw = km.raw
                c_key, s_key = raw[0:16], raw[16:32]
                c_salt, s_salt = raw[32:46], raw[46:60]
                self.tx_ke = self._derive_srtp_key(c_key, c_salt, 0x00, 16)
                self.tx_ka = self._derive_srtp_key(c_key, c_salt, 0x01, 20)
                self.tx_ks = self._derive_srtp_key(c_key, c_salt, 0x02, 14)
                self.rx_ke = self._derive_srtp_key(s_key, s_salt, 0x00, 16)
                self.rx_ks = self._derive_srtp_key(s_key, s_salt, 0x02, 14)
                self.ready = True
        return out_packets

    def decrypt_srtp(self, pkt, roc=0):
        if not self.ready or len(pkt) <= 22:
            return None
        cc = pkt[0] & 0x0F
        hdr_len = 12 + cc * 4
        if pkt[0] & 0x10:
            if len(pkt) < hdr_len + 4:
                return None
            ext_words, = struct.unpack("!H", pkt[hdr_len + 2:hdr_len + 4])
            hdr_len += 4 + ext_words * 4
        if len(pkt) <= hdr_len + 10:
            return None
        seq, _ts, ssrc = struct.unpack("!HII", pkt[2:12])
        idx = (roc << 16) | seq
        iv_int = (int.from_bytes(self.rx_ks, "big") << 16) ^ (ssrc << 64) ^ (idx << 16)
        return self.aes_128_ctr(self.rx_ke, iv_int.to_bytes(16, "big"), pkt[hdr_len:-10])

    def encrypt_srtp(self, pt, seq, ts, ssrc, payload):
        roc = (seq >> 16) & 0xFFFFFFFF
        seq16 = seq & 0xFFFF
        hdr = struct.pack("!BBHII", 0x80, pt & 0x7F, seq16, ts & 0xFFFFFFFF, ssrc & 0xFFFFFFFF)
        idx = (roc << 16) | seq16
        iv_int = (int.from_bytes(self.tx_ks, "big") << 16) ^ (ssrc << 64) ^ (idx << 16)
        enc = self.aes_128_ctr(self.tx_ke, iv_int.to_bytes(16, "big"), payload)
        tag = hmac.new(self.tx_ka, hdr + enc + struct.pack("!I", roc), hashlib.sha1).digest()[:10]
        return hdr + enc + tag

    def close(self):
        if getattr(self, "ssl", None):
            self.ssl_lib.SSL_free(self.ssl)
            self.ssl = None
        if getattr(self, "ctx", None):
            self.ssl_lib.SSL_CTX_free(self.ctx)
            self.ctx = None
        if getattr(self, "x509", None):
            self.crypto.X509_free(self.x509)
            self.x509 = None
        if getattr(self, "pkey", None):
            self.crypto.EVP_PKEY_free(self.pkey)
            self.pkey = None




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


def _stun_crc32(data):
    return (zlib_crc32(data) ^ 0x5354554E) & 0xFFFFFFFF


def zlib_crc32(data):
    import binascii
    return binascii.crc32(data) & 0xFFFFFFFF


def _stun_attr(atype, val):
    pad = (4 - (len(val) % 4)) % 4
    return struct.pack("!HH", atype, len(val)) + val + (b"\x00" * pad)


def _build_stun_request(lufrag, r_ufrag, r_pwd, use_candidate=True):
    tid = os.urandom(12)
    attrs = [
        _stun_attr(0x0006, f"{r_ufrag}:{lufrag}".encode("utf-8")),
        _stun_attr(0x8029, os.urandom(8)),
        _stun_attr(0x0024, struct.pack("!I", 1853824767)),
    ]
    if use_candidate:
        attrs.append(_stun_attr(0x0025, b""))
    body = b"".join(attrs)
    hdr_mi = struct.pack("!HHI12s", 0x0001, len(body) + 24, _STUN_COOKIE, tid)
    mi = _stun_attr(0x0008, hmac.new(r_pwd.encode("utf-8"), hdr_mi + body, hashlib.sha1).digest())
    body_mi = body + mi
    hdr_fp = struct.pack("!HHI12s", 0x0001, len(body_mi) + 8, _STUN_COOKIE, tid)
    fp = _stun_attr(0x8028, struct.pack("!I", _stun_crc32(hdr_fp + body_mi)))
    body_all = body_mi + fp
    return struct.pack("!HHI12s", 0x0001, len(body_all), _STUN_COOKIE, tid) + body_all


def _build_stun_response(req_pkt, addr, lpwd):
    tid = req_pkt[8:20]
    ip_int = int.from_bytes(socket.inet_aton(addr[0]), "big") ^ _STUN_COOKIE
    port_xor = addr[1] ^ (_STUN_COOKIE >> 16)
    xma = _stun_attr(0x0020, struct.pack("!BBHI", 0, 1, port_xor, ip_int))
    hdr_mi = struct.pack("!HHI12s", 0x0101, len(xma) + 24, _STUN_COOKIE, tid)
    mi = _stun_attr(0x0008, hmac.new(lpwd.encode("utf-8"), hdr_mi + xma, hashlib.sha1).digest())
    body_mi = xma + mi
    hdr_fp = struct.pack("!HHI12s", 0x0101, len(body_mi) + 8, _STUN_COOKIE, tid)
    fp = _stun_attr(0x8028, struct.pack("!I", _stun_crc32(hdr_fp + body_mi)))
    body_all = body_mi + fp
    return struct.pack("!HHI12s", 0x0101, len(body_all), _STUN_COOKIE, tid) + body_all


def _discover_snat_ips(udp, local_port):
    local_ip = "127.0.0.1"
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
    except Exception:
        pass
    pub_ip, pub_port = None, local_port
    try:
        tid = os.urandom(12)
        udp.sendto(struct.pack("!HHI12s", 0x0001, 0, _STUN_COOKIE, tid), ("stun.l.google.com", 19302))
        r, _, _ = select.select([udp], [], [], 1.5)
        if r:
            data, _ = udp.recvfrom(2048)
            pos = 20
            while pos + 4 <= len(data):
                at, al = struct.unpack("!HH", data[pos:pos + 4])
                av = data[pos + 4:pos + 4 + al]
                if at == 0x0020 and al >= 8:
                    pub_port = struct.unpack("!H", av[2:4])[0] ^ (_STUN_COOKIE >> 16)
                    ip_int = struct.unpack("!I", av[4:8])[0] ^ _STUN_COOKIE
                    pub_ip = socket.inet_ntoa(ip_int.to_bytes(4, "big"))
                    break
                pos += 4 + ((al + 3) & ~3)
    except Exception:
        pass
    cands = [(local_ip, local_port, "host")]
    if pub_ip:
        base_int = int.from_bytes(socket.inet_aton(pub_ip), "big") & ~0x0F
        cands.append((pub_ip, pub_port, "srflx"))
        for i in range(1, 16):
            ip_s = socket.inet_ntoa((base_int + i).to_bytes(4, "big"))
            if ip_s != pub_ip:
                cands.append((ip_s, pub_port, "srflx"))
    return cands


class WebRtcAudio:
    def __init__(self, output, input_device, callback, on_error):
        self.output = int(output)
        self.input_device = int(input_device)
        self.callback = callback
        self.on_error = on_error
        self.failed = None
        self.play_lock = threading.Lock()
        self.stop = threading.Event()
        self._ready = threading.Event()
        self._tx_queue = deque()
        self._tx_drained = threading.Event()
        self._tx_drained.set()
        self._tx_seq = int.from_bytes(os.urandom(2), "big")
        self._tx_ts = int.from_bytes(os.urandom(4), "big")
        self._hist_8k = [0] * 16

        cfg = _webrtc_reg_config()
        if cfg is None:
            raise OSError("MsRdcWebRTCSvc is not configured on this machine")
        self.port = cfg["port"]
        self.raw_key = bytes.fromhex((_WEBRTC_SERVICE_ID + cfg["key"]).replace("-", ""))

        w_inputs, w_outputs = _webrtc_catalog()
        self.mic = next((d for d in w_inputs if d["id"] == self.input_device), None)
        if self.mic is None:
            self.mic = next((d for d in w_inputs if "usb uac" in d["name"].lower()), w_inputs[0] if w_inputs else None)
        self.spk = next((d for d in w_outputs if d["id"] == self.output), None)
        if self.spk is None:
            self.spk = next((d for d in w_outputs if "usb uac" in d["name"].lower()), w_outputs[0] if w_outputs else None)
        if self.mic is None or self.spk is None:
            raise OSError("WebRTC USB audio device not found; ensure Remote Desktop audio redirection is active")

        self.dtls = None
        self.udp = None
        self.ws = None
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        if not self._ready.wait(timeout=12.0):
            err = self.failed or TimeoutError("Timed out establishing MsRdcWebRTCSvc WebRTC audio stream")
            self.close()
            raise err

    def _process_rx_pcmu(self, pcmu):
        if len(pcmu) < 80:
            return
        pkt = [_ULAW_DEC[b] for b in pcmu]
        tone = _pkt_is_tone(pkt)
        buf = self._hist_8k + pkt
        self._hist_8k = buf[-16:]
        n_pkt = len(pkt)
        if not tone:
            self.callback(b"\x00" * (n_pkt * 12))
            return
        out = array.array("h", [0] * (n_pkt * 6))
        idx = 0
        for i in range(n_pkt):
            win = buf[i:i + 17]
            for taps in _SINC_PHASES:
                v = int(sum(a * b for a, b in zip(win, taps)) * 4.0)
                out[idx] = -32768 if v < -32768 else (32767 if v > 32767 else v)
                idx += 1
        if sys.byteorder != "little":
            out.byteswap()
        self.callback(out.tobytes())

    def _run(self):
        try:
            self.dtls = _OpenSslDtlsSrtp()
            self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.udp.bind(("0.0.0.0", 0))
            local_port = self.udp.getsockname()[1]
            snat_cands = _discover_snat_ips(self.udp, local_port)
            local_ip = snat_cands[0][0]

            self.ws = _WebRtcWs(self.port, self.raw_key, timeout=4.0)
            self.ws.authenticate(timeout=4.0)

            # Refresh device IDs from live session in case deviceId GUID changed.
            cid_enum = self.ws.call("MediaDevices", "enumerateDevices", [])
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                msg = self.ws.recv_msg(timeout=1.0)
                if msg.get("rpcCallId") == cid_enum and isinstance(msg.get("result"), list):
                    for d in msg["result"]:
                        if d.get("kind") == "audioinput" and d.get("label") == self.mic["label"]:
                            self.mic["deviceId"] = d["deviceId"]
                        elif d.get("kind") == "audiooutput" and d.get("label") == self.spk["label"]:
                            self.spk["deviceId"] = d["deviceId"]
                    break

            self.ws.call("MediaStream", "createMediaStream", [{
                "id": str(uuid.uuid4()),
                "constraints": {
                    "audio": {
                        "deviceId": self.mic["deviceId"],
                        "echoCancellation": False,
                        "noiseSuppression": False,
                        "autoGainControl": False,
                    }
                },
            }], obj_id=20)
            self.ws.call("MediaStreamTrack", "createMediaStreamTrack", [{
                "mediaStreamRpcObjectId": 20,
                "id": str(uuid.uuid4()),
                "kind": "audio",
                "label": self.mic["label"],
            }], obj_id=21)
            self.ws.call("RTCPeerConnection", "createPeerConnection", [{
                "sdpSemantics": "unified-plan",
                "iceServers": _load_ice_servers(),
            }], obj_id=10)
            self.ws.call("RTCPeerConnection", "addTrack", [{
                "trackRpcObjectId": 21,
                "streamRpcObjectIds": [20],
                "senderRpcObjectId": 12,
                "receiverRpcObjectId": 13,
                "transceiverRpcObjectId": 11,
                "reuseSender": False,
            }], obj_id=10)

            cid_offer = self.ws.call("RTCPeerConnection", "createOffer", [], obj_id=10)
            offer = None
            remote_cands = []
            deadline = time.monotonic() + 6.0
            while time.monotonic() < deadline:
                msg = self.ws.recv_msg(timeout=1.5)
                if msg.get("rpcCallId") == cid_offer and "result" in msg:
                    offer = msg["result"]["desc"]
                    break

            if not offer:
                raise TimeoutError("MsRdcWebRTCSvc createOffer timed out")

            cid_sld = self.ws.call("RTCPeerConnection", "setLocalDescription", [offer], obj_id=10)
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                msg = self.ws.recv_msg(timeout=1.5)
                if msg.get("rpcCallId") == cid_sld:
                    break
                if msg.get("rpcEventName") == "icecandidate":
                    c = (msg.get("rpcEventArgs") or {}).get("candidate")
                    if c and c.get("candidate"):
                        remote_cands.append(c["candidate"])

            sdp_txt = offer["sdp"]
            r_ufrag = re.search(r"a=ice-ufrag:(\S+)", sdp_txt).group(1)
            r_pwd = re.search(r"a=ice-pwd:(\S+)", sdp_txt).group(1)
            lufrag = uuid.uuid4().hex[:8]
            lpwd = uuid.uuid4().hex[:24]

            cand_lines = []
            for idx, (cip, cport, ctype) in enumerate(snat_cands, 1):
                extra = f" raddr {local_ip} rport {local_port}" if ctype == "srflx" else ""
                cand_lines.append(
                    f"a=candidate:{idx} 1 UDP {2130706431 - idx} {cip} {cport} typ {ctype}{extra}\r\n"
                )

            answer_sdp = (
                "v=0\r\n"
                f"o=- {int(time.time())} 2 IN IP4 {local_ip}\r\n"
                "s=-\r\n"
                "t=0 0\r\n"
                "a=group:BUNDLE 0\r\n"
                "a=msid-semantic: WMS dongle-stream\r\n"
                f"m=audio {local_port} UDP/TLS/RTP/SAVPF 0\r\n"
                f"c=IN IP4 {local_ip}\r\n"
                "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                f"{''.join(cand_lines)}"
                f"a=ice-ufrag:{lufrag}\r\n"
                f"a=ice-pwd:{lpwd}\r\n"
                "a=ice-options:trickle\r\n"
                f"a=fingerprint:sha-256 {self.dtls.fingerprint}\r\n"
                "a=setup:active\r\n"
                "a=mid:0\r\n"
                "a=sendrecv\r\n"
                "a=msid:dongle-stream dongle-track\r\n"
                "a=rtcp-mux\r\n"
                "a=rtpmap:0 PCMU/8000\r\n"
                f"a=ssrc:{_LOCAL_SSRC} cname:dongle\r\n"
                f"a=ssrc:{_LOCAL_SSRC} msid:dongle-stream dongle-track\r\n"
            )

            cid_srd = self.ws.call(
                "RTCPeerConnection", "setRemoteDescription", [{"type": "answer", "sdp": answer_sdp}], obj_id=10
            )
            remote_stream_id = None
            got_srd = False
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline and not (got_srd and remote_stream_id is not None):
                msg = self.ws.recv_msg(timeout=1.0)
                if msg.get("rpcCallId") == cid_srd:
                    got_srd = True
                elif msg.get("rpcEventName") == "track":
                    remote_stream_id = msg["rpcEventArgs"]["stream"]["rpcObjectId"]
                elif msg.get("rpcEventName") == "icecandidate":
                    c = (msg.get("rpcEventArgs") or {}).get("candidate")
                    if c and c.get("candidate"):
                        remote_cands.append(c["candidate"])

            if remote_stream_id is not None:
                self.ws.call("MediaElement", "createMediaElement", ["audio", "0000000000000000"], obj_id=30)
                self.ws.call("MediaElement", "setSinkId", [self.spk["deviceId"]], obj_id=30)
                self.ws.call("MediaElement", "setAttribute", ["volume", 1], obj_id=30)
                self.ws.call("MediaElement", "setAttribute", ["muted", False], obj_id=30)
                self.ws.call("MediaElement", "setAttribute", ["autoplay", True], obj_id=30)
                self.ws.call(
                    "MediaElement",
                    "setAttribute",
                    ["srcObject", {"rpcObjectType": "MediaStream", "rpcObjectId": remote_stream_id}],
                    obj_id=30,
                )
                self.ws.call("MediaElement", "play", [], obj_id=30)

            for idx, (cip, cport, ctype) in enumerate(snat_cands, 1):
                extra = f" raddr {local_ip} rport {local_port}" if ctype == "srflx" else ""
                self.ws.call("RTCPeerConnection", "addIceCandidate", [{
                    "sdpMid": "0",
                    "sdpMLineIndex": 0,
                    "sdp": f"candidate:{idx} 1 UDP {2130706431 - idx} {cip} {cport} typ {ctype}{extra}",
                }], obj_id=10)

            def parse_targets(c_list):
                targets = []
                for c_str in c_list:
                    m = re.search(r"candidate:\S+\s+1\s+udp\s+\d+\s+(\S+)\s+(\d+)\s+typ\s+(\S+)", c_str, re.I)
                    if m and ":" not in m.group(1):
                        ip_s, port_s, c_t = m.group(1), int(m.group(2)), m.group(3)
                        prio = 0 if c_t == "relay" else (1 if c_t == "srflx" else 2)
                        targets.append((prio, ip_s, port_s))
                targets.sort()
                return [(ip_s, port_s) for _, ip_s, port_s in targets]

            active_peer = None
            dtls_started = False
            last_stun = 0.0
            last_rx_rtp = time.monotonic()
            next_tx = time.monotonic()
            rx_roc = 0
            last_rx_seq = None

            while not self.stop.is_set():
                now = time.monotonic()
                # Drain any new ICE candidates or virtual channel events from MsRdcWebRTCSvc.
                r_ws, _, _ = select.select([self.ws.sock], [], [], 0.0)
                if r_ws:
                    try:
                        msg = self.ws.recv_msg(timeout=0.01)
                        if msg.get("rpcEventName") == "icecandidate":
                            c = (msg.get("rpcEventArgs") or {}).get("candidate")
                            if c and c.get("candidate"):
                                remote_cands.append(c["candidate"])
                        elif msg.get("rpcEventName") == "virtualchannelerror":
                            raise OSError("RDP WebRTC session disconnected")
                    except (TimeoutError, socket.timeout):
                        pass

                # Send periodic STUN checks to establish/maintain ICE consent.
                if now - last_stun >= (0.25 if active_peer is None else 2.0):
                    stun_pkt = _build_stun_request(lufrag, r_ufrag, r_pwd, use_candidate=True)
                    if active_peer is not None:
                        try:
                            self.udp.sendto(stun_pkt, active_peer)
                        except OSError:
                            pass
                    else:
                        for tgt in parse_targets(remote_cands):
                            try:
                                self.udp.sendto(stun_pkt, tgt)
                            except OSError:
                                pass
                    last_stun = now

                # Send continuous 20ms SRTP uplink packets when DTLS-SRTP is ready.
                if self.dtls.ready and active_peer is not None and now >= next_tx:
                    if self._tx_queue:
                        frame = self._tx_queue.popleft()
                        if not self._tx_queue:
                            self._tx_drained.set()
                    else:
                        frame = b"\xFF" * 160
                    srtp_out = self.dtls.encrypt_srtp(0, self._tx_seq, self._tx_ts, _LOCAL_SSRC, frame)
                    self._tx_seq += 1
                    self._tx_ts = (self._tx_ts + 160) & 0xFFFFFFFF
                    try:
                        self.udp.sendto(srtp_out, active_peer)
                    except OSError:
                        pass
                    next_tx += 0.02
                    if now - next_tx > 0.1:
                        next_tx = now + 0.02

                wait_s = max(0.001, min(0.01, (next_tx - time.monotonic()) if self.dtls.ready else 0.01))
                r_udp, _, _ = select.select([self.udp], [], [], wait_s)
                while r_udp and not self.stop.is_set():
                    pkt, addr = self.udp.recvfrom(4096)
                    if not pkt:
                        break
                    b0 = pkt[0]
                    if b0 in (0, 1) and len(pkt) >= 20:
                        mtype = struct.unpack("!H", pkt[:2])[0]
                        if mtype == 0x0001:
                            resp = _build_stun_response(pkt, addr, lpwd)
                            self.udp.sendto(resp, addr)
                            active_peer = addr
                        elif mtype == 0x0101:
                            active_peer = addr
                        if active_peer is not None and not dtls_started:
                            dtls_started = True
                            for out_p in self.dtls.step_handshake():
                                self.udp.sendto(out_p, active_peer)
                    elif 20 <= b0 <= 63:
                        active_peer = addr
                        dtls_started = True
                        for out_p in self.dtls.step_handshake(pkt):
                            self.udp.sendto(out_p, active_peer)
                        if self.dtls.ready and not self._ready.is_set():
                            next_tx = time.monotonic()
                            last_rx_rtp = time.monotonic()
                            self._ready.set()
                    elif 128 <= b0 <= 191 and len(pkt) > 22:
                        pt = pkt[1] & 0x7F
                        if pt == 0 and self.dtls.ready:
                            seq, = struct.unpack("!H", pkt[2:4])
                            if last_rx_seq is not None:
                                if seq < last_rx_seq and (last_rx_seq - seq) > 32768:
                                    rx_roc += 1
                            last_rx_seq = seq
                            pcmu = self.dtls.decrypt_srtp(pkt, rx_roc)
                            if pcmu is not None:
                                last_rx_rtp = time.monotonic()
                                self._process_rx_pcmu(pcmu)
                    r_udp, _, _ = select.select([self.udp], [], [], 0.0)

                if self._ready.is_set() and time.monotonic() - last_rx_rtp > 8.0:
                    raise TimeoutError("WebRTC microphone capture stalled; reconnecting audio")
        except Exception as error:
            self.failed = error
            self._ready.set()
            self._tx_drained.set()
            if not self.stop.is_set():
                self.on_error(error)

    def play(self, pcm):
        if self.failed is not None:
            raise self.failed
        with self.play_lock:
            samples = array.array("h", pcm)
            if sys.byteorder != "little":
                samples.byteswap()
            ulaw = bytearray(_ULAW_ENC[s + 32768] for s in samples[0::6])
            rem = len(ulaw) % 160
            if rem:
                ulaw.extend(b"\xFF" * (160 - rem))
            self._tx_drained.clear()
            for offset in range(0, len(ulaw), 160):
                self._tx_queue.append(bytes(ulaw[offset:offset + 160]))
            deadline = time.monotonic() + len(pcm) / 96000.0 + 10.0
            while not self._tx_drained.wait(timeout=0.05):
                if self.failed is not None:
                    raise self.failed
                if self.stop.is_set() or time.monotonic() > deadline:
                    raise TimeoutError("WebRTC audio playback stalled")

    def close(self):
        self.stop.set()
        self._tx_drained.set()
        thread = getattr(self, "thread", None)
        if thread and thread is not threading.current_thread():
            thread.join(timeout=5)
        ws = getattr(self, "ws", None)
        if ws is not None:
            try:
                ws.call("MediaElement", "shutdown", [], obj_id=30)
                ws.call("MediaStreamTrack", "stop", [], obj_id=21)
                ws.call("RTCPeerConnection", "close", [], obj_id=10)
            except Exception:
                pass
            ws.close()
            self.ws = None
        udp = getattr(self, "udp", None)
        if udp is not None:
            try:
                udp.close()
            except Exception:
                pass
            self.udp = None
        dtls = getattr(self, "dtls", None)
        if dtls is not None:
            dtls.close()
            self.dtls = None


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
    w_inputs, w_outputs = _webrtc_catalog()
    for entries, extra in ((result["inputs"], w_inputs), (result["outputs"], w_outputs)):
        existing = {e["name"] for e in entries}
        for item in extra:
            name = item["name"] if item["name"] not in existing else f"{item['name']} (WebRTC)"
            existing.add(name)
            entries.append(dict(id=item["id"], name=name))
    return result


class Audio:
    def __init__(self, output, input_device, callback, on_error):
        self.output = int(output)
        self.callback, self.on_error = callback, on_error
        self.failed = None
        self._webrtc = None
        if self.output >= WEBRTC_ID_BASE or int(input_device) >= WEBRTC_ID_BASE:
            def _on_webrtc_error(error):
                self.failed = error
                self.on_error(error)
            self._webrtc = WebRtcAudio(self.output, int(input_device), callback, _on_webrtc_error)
            return
        self.dll = library()
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
        next_header = 0
        last_buffer = time.monotonic()
        try:
            while not self.stop.wait(0.01):
                for _ in self.headers:
                    header, buf = self.headers[next_header]
                    if not header.flags & 1:
                        break
                    # Completed buffers must follow queue order, including across wraparound.
                    pcm = buf.raw[:header.recorded]
                    header.recorded = 0
                    check(self.dll.waveInAddBuffer(self.capture, C.byref(header), C.sizeof(header)), "Requeue capture")
                    next_header = (next_header + 1) % len(self.headers)
                    last_buffer = time.monotonic()
                    self.callback(pcm)
                if time.monotonic() - last_buffer > 3:
                    raise TimeoutError("Microphone capture stalled; reconnecting audio")
        except Exception as error:
            self.failed = error
            self.on_error(error)

    def play(self, pcm):
        if getattr(self, "_webrtc", None) is not None:
            return self._webrtc.play(pcm)
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
        if getattr(self, "_webrtc", None) is not None:
            self._webrtc.close()
            self._webrtc = None
            return
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
