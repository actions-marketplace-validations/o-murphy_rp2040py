# Run inside gdb attached to a running native emulator: `gdb -p <pid> -batch -x scripts/debug/gdb_clock_alarms.py`. Prints the simulated clock's alarm list (what the idle fast-forward is jumping between)
# when the emulator spins and Python-level tools cannot say why (record 0100, the origin-move hang).
import gdb
gdb.execute("set pagination off")
f = gdb.newest_frame()
while f is not None:
    n = f.name() or "?"
    if "_simulator_execute_batch" in n and "pf_" in n:
        f.select()
        break
    f = f.older()
try:
    rp = gdb.parse_and_eval("__pyx_v_rp2040")
    obj = rp.dereference()
    names = [fld.name for fld in obj.type.fields()]
    print("rp fields:", names[:50])
    clk = None
    for nm in names:
        if nm == "clock":
            clk = obj[nm]
    print("clock field:", clk)
    sc = clk.cast(gdb.lookup_type("struct __pyx_obj_8rp2040py_6native_17_simulation_clock_SimulationClock").pointer()).dereference()
    c = sc["_clock"]
    print("Clock:", c)
    head = c["head_"]
    i = 0
    while int(head) != 0 and i < 8:
        a = head.dereference()
        print("alarm", i, "ticks", a["ticks"], "ctx", a["ctx"], "fire", a["fire"], "scheduled", a["scheduled"])
        head = a["next"]
        i += 1
except Exception as e:
    print("ERR", e)
