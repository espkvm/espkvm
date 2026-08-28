"""Hold a /video WebSocket open so the device counts a viewer and encodes."""
import base64, http.cookiejar, json, os, socket, ssl, sys, time, urllib.request

HOST = "10.42.0.93"

def login():
    ctx = ssl._create_unverified_context()
    req = urllib.request.Request(f"https://{HOST}/api/v1/auth/login",
        data=json.dumps({"user": "admin", "password": "espkvm-admin"}).encode(),
        headers={"Content-Type": "application/json"})
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx),
                                         urllib.request.HTTPCookieProcessor(jar))
    opener.open(req, timeout=10).read()
    return "; ".join(f"{c.name}={c.value}" for c in jar)

def view(seconds):
    cookie = login()
    ctx = ssl._create_unverified_context()
    raw = socket.create_connection((HOST, 443), timeout=10)
    s = ctx.wrap_socket(raw, server_hostname=HOST)
    key = base64.b64encode(os.urandom(16)).decode()
    s.send((f"GET /video HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\nCookie: {cookie}\r\n"
            f"Origin: https://{HOST}\r\n\r\n").encode())
    head = s.recv(1024)
    if b"101" not in head.split(b"\r\n")[0]:
        print("upgrade refused:", head.split(b"\r\n")[0].decode(errors="replace")); return
    # The device subscribes a viewer on its first message: one byte, 1 for the
    # picture (2 would ask for the screen as characters).
    payload, mask = b"\x01", os.urandom(4)
    s.send(b"\x82" + bytes([0x80 | len(payload)]) + mask +
           bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
    print("viewer connected and subscribed")
    end, got = time.time() + seconds, 0
    s.settimeout(5)
    while time.time() < end:
        try:
            b = s.recv(65536)
        except socket.timeout:
            continue
        if not b: break
        got += len(b)
    print(f"viewer done, {got/1024:.0f} KB received")
    s.close()

view(int(sys.argv[1]) if len(sys.argv) > 1 else 20)
