#!/usr/bin/env python3
"""derive53.py -- derive_offsets.py for Unity 5.3 (Steven Universe: Attack the Light).

Unity 5.3 registers its icalls one call at a time -- `ldr r0,=name-GOT; ldr
r1,=fn-GOT; add r0,r0,rGOT; add r1,r1,rGOT; bl mono_add_internal_call` -- not
from the flat name/function arrays 5.6 has. This finds the UnityEngine.Time
icalls that way and hands them to dcr_sea_nx's own derive_time checks; vsync,
GC and Mono are dcr_sea_nx's derivations unchanged.

usage: derive53.py <lib/armeabi-v7a> -o <out.h>
"""
import os, sys, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import derive_offsets as D
from arm32 import Arm32


def ldr_lit(u, at):
    w = u.u32(at)
    if (w & 0x0F7F0000) != 0x051F0000:  # ldr rX, [pc, #+-imm]
        return None, None
    imm = w & 0xFFF
    if not (w & 0x00800000):
        imm = -imm
    return (w >> 12) & 0xF, u.u32(at + 8 + imm)


def time_icalls(u):
    s = u.find_str("UnityEngine.Time::get_time")[0]
    site = u.gotoff_refs(s)[0]          # the ldr of the name's GOT offset
    fn = u.func_start(site)
    # GOT base: `ldr rA,=lit; ... add rG, pc, rA` at the start of the function
    got = None
    lits = {}
    for k in range(0, 16):
        at = fn + 4 * k
        r, v = ldr_lit(u, at)
        if r is not None:
            lits[r] = v
        w = u.u32(at)
        if (w & 0x0FFF0FF0) == 0x008F0000 and (w & 0xF) in lits:  # add rD, pc, rM
            got = (at + 8 + lits[w & 0xF]) & 0xFFFFFFFF
            break
    if got is None:
        raise SystemExit("Time icall registration: no GOT base")
    funcs = {}
    lit = {}   # register -> literal loaded (a GOT offset)
    val = {}   # register -> address (literal + GOT)
    greg = None
    at = fn
    end = fn + 0x2000
    while at < end:
        w = u.u32(at)
        r, v = ldr_lit(u, at)
        if r is not None:
            lit[r] = v
            val.pop(r, None)
        elif (w & 0x0FFF0FF0) == 0x008F0000:  # add rD, pc, rM: the GOT register
            greg = (w >> 12) & 0xF
        elif (w & 0x0FE00FF0) == 0x00800000:  # add rD, rN, rM
            rd, rn, rm = (w >> 12) & 0xF, (w >> 16) & 0xF, w & 0xF
            other = rm if rn == greg else rn if rm == greg else None
            if other is not None and other in lit:
                val[rd] = (lit[other] + got) & 0xFFFFFFFF
        elif D.is_bl(w):
            if 0 in val and 1 in val:
                try:
                    name = u.cstr(val[0])
                except Exception:
                    name = ""
                if name.startswith("UnityEngine.Time::"):
                    funcs[name.split("::", 1)[1]] = val[1]
                elif funcs:
                    break
            val = {}
        elif (w & 0x0FFF0000) == 0x08BD0000 and (w & 0x8000):  # pop {..., pc}
            break
        at += 4
    return funcs


def derive_time53(u):
    funcs = time_icalls(u)
    names = list(funcs)
    orig_find, orig_words = u.find_str, u.words_equal
    # Reuse dcr_sea_nx's checks: give it a fake flat table.
    hooks = {}
    for nm, (kind, fld) in D.TIME_HOOKS.items():
        if nm not in funcs:
            print("  (Unity 5.3 has no Time.%s: not hooked)" % nm)
            continue
        fn = funcs[nm]
        if fld is not None:
            _, f2 = D.load_field(u, fn)
            note = "field +0x%x (5.6: +0x%x)" % (f2 if f2 is not None else 0xfff, fld)
        else:
            note = "control-flow getter"
        hooks[nm] = (fn, u.guard(fn), note)
    gtm = D.bl_target(u.u32(funcs["get_time"] + 4), funcs["get_time"] + 4)

    def nth_bl(fn, n):
        c = 0
        for k in range(24):
            w = u.u32(fn + 4 * k)
            if D.is_bl(w):
                if c == n:
                    return D.bl_target(w, fn + 4 * k)
                c += 1
        return None

    def tail_b(fn):
        for k in range(24):
            w = u.u32(fn + 4 * k)
            if D.is_b(w):
                return D.bl_target(w, fn + 4 * k)
        return None

    entries = {
        "GetTimeManager": gtm,
        "GetRealtimeSinceStartup": nth_bl(funcs["get_realtimeSinceStartup"], 1),
        "SetTimeScale": tail_b(funcs["set_timeScale"]),
        "SetFixedDeltaTime": tail_b(funcs["set_fixedDeltaTime"]),
    }
    entries = {k: (v, u.guard(v) if v else [0, 0, 0, 0]) for k, v in entries.items()}
    # Unity 5.3's TimeManager is laid out differently from 5.6: the fields are
    # the ones its own getters load (MAXPARTICLEDELTA does not exist; 0).
    fld = lambda nm: D.load_field(u, funcs[nm])[1] or 0
    fieldmap = {
        "FIXEDDELTA": fld("get_fixedDeltaTime"),
        "MAXPARTICLEDELTA": 0,
        "TIMESCALE": fld("get_timeScale"),
        "FRAMECOUNT": fld("get_frameCount"),
    }
    return dict(names=names, hooks=hooks, entries=entries, fieldmap=fieldmap, N=len(names))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libdir")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    u = Arm32(os.path.join(a.libdir, "libunity.so"))
    m = Arm32(os.path.join(a.libdir, "libmono.so"))
    t = derive_time53(u)
    for nm in [n for n in D.TIME_HOOKS if n not in t["hooks"]]:
        del D.TIME_HOOKS[nm]
    for nm, (fn, _, note) in t["hooks"].items():
        print("  %-28s 0x%x  %s" % (nm, fn, note))
    for nm, (rva, _) in t["entries"].items():
        print("  %-28s %s" % (nm, hex(rva) if rva else None))
    D.derive_time = lambda _u: t
    sys.argv = [sys.argv[0], a.libdir, "-o", a.out]
    D.main()


if __name__ == "__main__":
    main()
