# Replay an SCDA_TRACE_RESOLVE=<n> log: report sampled slots whose fetch is a resolve miss or mismatch.
import re, sys
samples = {}
fetch = {}
cur = None
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\[rtrace\] draw (\d+) vs \S+ ps (\S+) .*samples(.*)', line)
    if m:
        cur = int(m.group(1)); samples[cur] = (m.group(2), [int(x) for x in m.group(3).split()])
        continue
    m = re.match(r'\[rtrace\] draw (\d+) slot (\d+) fetch (\S+) fmt (\d+) (\d+)x(\d+) tiled (\d) resolve (\d+)x(\d+) fmt (\d+)', line)
    if m:
        fetch.setdefault(int(m.group(1)), {})[int(m.group(2))] = m.groups()[2:]
last = {}
for d in sorted(samples):
    ps, used = samples[d]
    if d in fetch:
        last = fetch[d]  # bindings are logged only when the register file changes
    for s in used:
        f = last.get(s)
        if not f:
            continue
        base, fmt, w, h, tiled, rw, rh, rf = f
        if int(rw) and (int(rw) < int(w) or int(rh) < int(h) or rf != fmt):
            print('mismatch', d, ps, s, f)
print('draws', len(samples))
