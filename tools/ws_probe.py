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

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        # Кадры фрагментируются: собираем до FIN, продолжения идут opcode 0.
        acc = b""
        started = False
        while True:
            h = self._read(2)
            fin = h[0] & 0x80
            opcode = h[0] & 0x0F
            ln = h[1] & 0x7F
            if ln == 126:
                ln = struct.unpack("!H", self._read(2))[0]
            elif ln == 127:
                ln = struct.unpack("!Q", self._read(8))[0]
            payload = self._read(ln)
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
