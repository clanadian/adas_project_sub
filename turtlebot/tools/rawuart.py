import serial, time, collections
s = serial.Serial('/dev/ttyS0', 115200, timeout=0)
buf = bytearray(); t0 = time.time()
while time.time() - t0 < 3.0:
    n = s.in_waiting
    if n: buf.extend(s.read(n))
    time.sleep(0.01)
s.close()
print("수신 바이트 수:", len(buf))
print("앞 60바이트 :", buf[:60].hex(' '))

def crc8(d):
    c = 0
    for v in d:
        c ^= v
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c

ok = bad = 0; states = collections.Counter(); i = 0
while i + 3 <= len(buf):
    if buf[i] != 0xA5:
        bad += 1; i += 1; continue
    if crc8(bytes(buf[i:i+2])) == buf[i+2]:
        ok += 1; states[buf[i+1]] += 1; i += 3
    else:
        bad += 1; i += 1
print("유효 프레임 :", ok, " / 버린 바이트:", bad)
print("프레임률    : %.1f Hz" % (ok / 3.0))
name = {0: "CLEAR", 1: "SLOW", 2: "STOP"}
for st, c in sorted(states.items()):
    print("  state=%d %-5s  %d회" % (st, name.get(st, "?"), c))
