# A fake Co-op teammate, for testing the Co-op mod against one game.
#
#   coop_bot.py [seconds] [cx cy cz]        a guest: joins the host on 127.0.0.1:7777 as Yunica, walks a circle in the
#                                           host's room, says one chat line, prints what the host sends
#   coop_bot.py host [seconds]              a host: waits for the game to join. Its id is 1, so it owns the monsters
#                                           of the room it shares with the game: in S_1001 it sends snapshots that
#                                           line three of them up beside the entrance and let a fourth fall, then
#                                           one "struck" (a hit on the game's player). It prints what the game sends.
import json, math, socket, sys, time

host_mode = len(sys.argv) > 1 and sys.argv[1] == "host"
args = sys.argv[2:] if host_mode else sys.argv[1:]
secs = float(args[0]) if args else 20
cx, cy, cz = (float(v) for v in args[1:4]) if len(args) > 3 else (0.0, 1.08, 19.0)

if host_mode:
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 7777)); srv.listen(1); srv.settimeout(60)
    print("waiting for the game to join..."); sys.stdout.flush()
    s, _ = srv.accept()
else:
    s = socket.create_connection(("127.0.0.1", 7777), timeout=5)
s.settimeout(0.02)
def send(m): s.sendall((json.dumps(m) + "\n").encode())
if not host_mode: send({"t": "hello", "v": 2, "name": "Bot", "ch": 1})

# S_1001's monsters as the game builds them (id, cast, level, max HP), and where the host's snapshots put them
MONSTERS = [(0x01000009, 14, 1, 60), (0x0100000b, 14, 1, 60), (0x0100000d, 14, 1, 60), (0x01000013, 14, 1, 60)]
buf, room, kinds, t0, last, n, pose = b"", None, {}, time.time(), 0, 0, None
while time.time() - t0 < secs:
    try:
        d = s.recv(65536)
        if not d: print("closed by the other side"); break
        buf += d
    except socket.timeout:
        pass
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        try: m = json.loads(line)
        except Exception: continue
        k = m.get("t"); kinds[k] = kinds.get(k, 0) + 1
        if k == "hello" and host_mode:
            print("hello", m); send({"t": "welcome", "id": 2, "peers": [[1, "Bot", 1]]})
        elif k == "welcome": print("welcome", m)
        elif k == "p":
            pose = m
            if m.get("r") != room:
                room = m.get("r")
                print("the game is in", repr(room), {x: m.get(x) for x in ("x", "y", "z", "s", "g", "f", "c", "hp")}, "eyes", len(m.get("e") or []))
        elif kinds[k] <= 3: print(k, json.dumps(m)[:300])
    sys.stdout.flush()
    now = time.time()
    if room and now - last >= 0.1:
        last = now; n += 1
        a = (now - t0) * 1.5
        if host_mode: cx, cy, cz = pose["x"], pose["y"], pose["z"]
        me = {"t": "p", "r": room, "x": round(cx + 2.2 * math.cos(a), 2), "y": cy, "z": round(cz + 2.2 * math.sin(a), 2),
              "yaw": round(math.degrees(a) + 90, 2), "s": 15, "g": 266, "f": n % 8, "c": 1, "hp": 80, "mhp": 100}
        if host_mode: me["from"] = 1
        send(me)
        if n == 30: send({"t": "chat", "text": "hello from the bot", **({"from": 1} if host_mode else {})})
        if host_mode and room == "S_1001":
            snap = []
            for i, (mid, cast, level, mhp) in enumerate(MONSTERS):
                fallen = i == 3 and n > 40
                snap.append([mid, cast, level, round(cx - 3 + 2.0 * i, 2), cy, round(cz + 3.5, 2), 180, 0 if fallen else mhp, mhp, 2, n % 6, -1])
            send({"t": "snap", "from": 1, "r": room, "m": snap})
            if n == 60: send({"t": "struck", "from": 1, "atk": 120, "power": 0})
print("received:", kinds)
