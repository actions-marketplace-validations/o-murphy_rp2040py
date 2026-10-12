import time  # type: ignore[import-not-found]

import network  # type: ignore[import-not-found]

# The part of tests/micropython/main-cyw43.py that needs no internet: scan, join, the DHCP lease
# answered by the emulator's own DhcpServer, and the link coming back down. Everything here is
# produced by the emulator alone, so CI treats a failure as an emulator regression (the full script
# also opens real sockets and is soft-failing for that reason). The marker is printed only if every
# step held.
nic = network.WLAN(network.WLAN.IF_STA)
nic.active(True)
# not checked: active() before the first join reads False on MicroPython 1.23.0 (the getter differs between releases), which says nothing about the emulator
ok = True

found = nic.scan()
ok = ok and any(entry[0] == b"RP2040PY-GUEST" for entry in found)
print("scan:", found)

nic.connect("RP2040PY-GUEST", "key")
connected = False
for _ in range(50):
    if nic.isconnected():
        connected = True
        break
    time.sleep(0.1)
print("isconnected() after DHCP poll:", connected)
ok = ok and connected
ok = ok and nic.ifconfig()[0] == "10.0.0.2"  # ifconfig(), not ipconfig(): older MicroPython has only the former
print("ifconfig:", nic.ifconfig())

nic.disconnect()
for _ in range(50):
    if not nic.isconnected():
        break
    time.sleep(0.1)
ok = ok and not nic.isconnected()
print("after disconnect: isconnected=", nic.isconnected())

print("MICROPYTHON CYW43 CORE OK" if ok else "MICROPYTHON CYW43 CORE FAILED")
