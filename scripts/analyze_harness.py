#!/usr/bin/env python3
"""Cross-peer harness log analysis: reconstruct rollback activity + compare per-frame
region hashes between host and client to localize any desync.

Usage: analyze_harness.py <host_session.log> <client_session.log>
"""
import re, sys, collections

def load(path):
    saves = {}   # frame -> (rh tuple, rng, in L/R)
    batches = []
    reloads = []
    resim = []
    verdict = None
    reseed = []
    veto = []
    relabel = []
    injmm = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"save f=(\d+) sim=\d+ rh=\[([0-9A-F ]+)\] rng=([0-9A-F]+) fpu=[0-9A-F]+ in\[L=0x([0-9A-F]+) R=0x([0-9A-F]+)\]", line)
            if m:
                fr = int(m.group(1))
                rh = tuple(m.group(2).split())
                saves[fr] = (rh, m.group(3), m.group(4), m.group(5))
                continue
            m = re.search(r"batch n=(-?\d+) cur=(\d+) floor=(\d+) confirmed=(\d+) frontier=(\d+) local=(\d+) remote=(\d+) predDepth=(-?\d+) adj=(\d+)", line)
            if m:
                batches.append(tuple(int(x) for x in m.groups()))
                continue
            if "reload floor" in line:
                mm = re.search(r"reload floor (\d+) for target (\d+)", line)
                if mm: reloads.append((int(mm.group(1)), int(mm.group(2))))
            if "RESIM NON-DETERMINISTIC" in line: resim.append(line.strip())
            if "spec_transition_veto" in line or "VETOED" in line: veto.append(line.strip())
            if "round entry relabel" in line: relabel.append(line.strip())
            if "battle entry rng reseed" in line: reseed.append(line.strip())
            if "inject verify" in line and "MISMATCH" in line: injmm.append(line.strip())
            if "HARNESS VERDICT" in line: verdict = line.strip()
    return dict(saves=saves, batches=batches, reloads=reloads, resim=resim,
                verdict=verdict, reseed=reseed, veto=veto, relabel=relabel, injmm=injmm)

def summ(tag, d):
    print(f"--- {tag} ---")
    print(f"  saved frames: {len(d['saves'])}  range: {min(d['saves']) if d['saves'] else '-'}..{max(d['saves']) if d['saves'] else '-'}")
    print(f"  batches: {len(d['batches'])}  reloads(rollbacks): {len(d['reloads'])}")
    preddepths = [b[7] for b in d['batches'] if b[7] > 0]
    if preddepths:
        print(f"  predicted batches: {len(preddepths)}  max predDepth: {max(preddepths)}  avg: {sum(preddepths)/len(preddepths):.1f}")
    print(f"  reseeds: {len(d['reseed'])}  vetoes: {len(d['veto'])}  relabels: {len(d['relabel'])}")
    print(f"  RESIM divergences: {len(d['resim'])}  inject mismatches: {len(d['injmm'])}")
    print(f"  verdict: {d['verdict'] or '(none)'}")
    for r in d['resim'][:5]: print("    RESIM:", r)
    for r in d['injmm'][:5]: print("    INJMM:", r)

def main():
    if len(sys.argv) < 3:
        print("usage: analyze_harness.py <host.log> <client.log>"); return 1
    h = load(sys.argv[1]); c = load(sys.argv[2])
    summ("HOST", h); summ("CLIENT", c)
    print("--- cross-peer per-frame region hash compare ---")
    common = sorted(set(h['saves']) & set(c['saves']))
    if not common:
        print("  no overlapping saved frames — peers never aligned on frame numbers (entry skew?)")
        return 0
    print(f"  overlapping frames: {len(common)}  ({common[0]}..{common[-1]})")
    first_div = None
    div_regions = collections.Counter()
    names = ["p1_core","p1_anim","p2_block","p2_anim","globals"]
    for fr in common:
        rh_h, rh_c = h['saves'][fr][0], c['saves'][fr][0]
        if rh_h != rh_c:
            if first_div is None: first_div = fr
            for i,(a,b) in enumerate(zip(rh_h, rh_c)):
                if a != b: div_regions[names[i] if i < len(names) else i] += 1
    if first_div is None:
        print("  ALL overlapping frames hash-identical across peers — DETERMINISTIC ✓")
    else:
        print(f"  FIRST divergent frame: {first_div}")
        print(f"  host   rh={h['saves'][first_div][0]} rng={h['saves'][first_div][1]}")
        print(f"  client rh={c['saves'][first_div][0]} rng={c['saves'][first_div][1]}")
        print(f"  divergent-region histogram: {dict(div_regions)}")
    return 0

sys.exit(main())
