# A fake Co-op guest: joins the host on 127.0.0.1:7777, walks a circle as Yunica in the host's room, prints what
# the host sends. coop_bot.py [seconds] [cx cy cz]
import json, math, socket, sys, time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20
cx, cy, cz = (float(v) for v in sys.argv[2:5]) if len(sys.argv) > 4 else (0.0, 1.08, 19.0)
s = socket.create_connection(("127.0.0.1", 7777), timeout=5)
s.settimeout(0.02)
def send(m): s.sendall((json.dumps(m) + "\n").encode())
send({"t": "hello", "v": 2, "name": "Bot", "ch": 1})
buf, room, kinds, t0, last, n = b"", None, {}, time.time(), 0, 0
while time.time() - t0 < secs:
    try:
        d = s.recv(65536)
        if not d: print("closed by host"); break
        buf += d
    except socket.timeout:
        pass
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        try: m = json.loads(line)
        except Exception: continue
        k = m.get("t"); kinds[k] = kinds.get(k, 0) + 1
        if k == "welcome": print("welcome", m)
        elif k == "p":
            if m.get("r") != room: room = m.get("r"); print("host is in", repr(room), {x: m.get(x) for x in ("x", "y", "z", "s", "g", "f", "c", "hp")}, "eyes", len(m.get("e") or []))
        elif kinds[k] <= 2: print(k, json.dumps(m)[:300])
    now = time.time()
    if room and now - last >= 0.1:
        last = now; n += 1
        a = (now - t0) * 1.5
        send({"t": "p", "r": room, "x": round(cx + 2.2 * math.cos(a), 2), "y": cy, "z": round(cz + 2.2 * math.sin(a), 2),
              "yaw": round(math.degrees(a) + 90, 2), "s": 15, "g": 266, "f": n % 8, "c": 1, "hp": 80, "mhp": 100})
        if n == 30: send({"t": "chat", "text": "hello from the bot"})
print("received:", kinds)
