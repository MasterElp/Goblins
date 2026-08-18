"""Минимальный WebSocket-клиент: рукопожатие и сборка кадров.

Нужен проверкам из tools/, чтобы разговаривать с сервером его же
протоколом, не таща в репозиторий зависимость ради десятка строк.
Полноценным клиентом не является и не должен: он умеет ровно послать
объект и дождаться объекта.
"""
import socket, base64, os, json, struct, sys, time

class WebSocketProbe:
    def __init__(self, host='127.0.0.1', port=9002):
        self.s = socket.create_connection((host, port), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.s.sendall(req.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(4096)
        self.buf = buf.split(b"\r\n\r\n", 1)[1]

    def _take_frame(self):
        """Снять из буфера один целый кадр или вернуть None.

        Целиком, а не по кусочкам: разбор с ожиданием посреди кадра
        (self._read внутри разбора) при таймауте съедал бы уже прочитанный
        заголовок и разъезжался с потоком навсегда. Проверке, которая
        СЛУШАЕТ ТИШИНУ (сервер обязан замолчать, см. "updates" в
        протоколе), таймаут нужен по определению — поэтому буфер трогается
        только тогда, когда кадр в нём целый.
        """
        b = self.buf
        if len(b) < 2:
            return None
        length = b[1] & 0x7F
        offset = 2
        if length == 126:
            if len(b) < 4:
                return None
            length = struct.unpack("!H", b[2:4])[0]
            offset = 4
        elif length == 127:
            if len(b) < 10:
                return None
            length = struct.unpack("!Q", b[2:10])[0]
            offset = 10
        if len(b) < offset + length:
            return None
        self.buf = b[offset + length:]
        return b[0] & 0x80, b[0] & 0x0F, b[offset:offset + length]

    def recv(self, timeout=None):
        """Дождаться сообщения. timeout в секундах — вернуть None, если за
        это время сообщение не пришло; поток при этом не портится."""
        deadline = None if timeout is None else time.monotonic() + timeout
        # Кадры фрагментируются: собираем до FIN, продолжения идут opcode 0.
        acc = b""
        started = False
        while True:
            frame = self._take_frame()
            if frame is None:
                if deadline is not None:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        return None
                    self.s.settimeout(left)
                try:
                    chunk = self.s.recv(65536)
                except (TimeoutError, socket.timeout):
                    return None
                finally:
                    if deadline is not None:
                        self.s.settimeout(30)
                if not chunk:
                    raise EOFError
                self.buf += chunk
                continue
            fin, opcode, payload = frame
            if opcode == 8:
                raise EOFError("closed")
            if opcode in (9, 10):
                continue
            if opcode in (1, 2):
                acc = payload
                started = True
            elif opcode == 0 and started:
                acc += payload
            if fin and started:
                return json.loads(acc)

    def send(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack("!H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack("!Q", n)
        self.s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
