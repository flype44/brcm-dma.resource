# Turns the output of bench.sh / matrix.sh into markdown tables: python3 parse.py <results file> [<results file> ...]
# One table of the operations a second (and the Mcycles an operation) for every screen / window size, one column for each threshold of the DMA,
# and one table of the counters of the resource.
import re, sys, collections

runs = []
for path in sys.argv[1:]:
    cur = None
    for line in open(path, encoding='latin-1'):
        line = line.rstrip()
        m = re.match(r'### \S+ screen (\d+x\d+) window (\d+x\d+) min (\d+)', line)
        if m:
            cur = {'screen': m.group(1), 'win': m.group(2), 'min': int(m.group(3)), 'tests': {}, 'jobs': None, 'mb': None, 'busy': None, 'infos': 0}
            runs.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r'(\w[\w ]*?): (\d+) ops in (\d+) ms = (\d+) ops/s, (\d+) 68k instr/op, (\d+) cycles/op', line)
        if m:
            cur['tests'][m.group(1)] = (int(m.group(4)), int(m.group(6)))
            continue
        m = re.match(r'jobs (\d+) \((\d+) MB\)', line)
        if m:
            cur['infos'] += 1
            if cur['infos'] == 2:
                cur['jobs'] = int(m.group(1))
                cur['mb'] = int(m.group(2))
            continue
        m = re.match(r'queue max (\d+), longest wait (\d+) us; busy: channel 12 (\d+) ms, channel 13 (\d+) ms', line)
        if m and cur['infos'] == 2 and cur['busy'] is None:
            cur['busy'] = (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))

names = ['ScrollRaster', 'RectFill', 'MoveLayer', 'SizeLayer', 'Desktop mix']
mins = sorted(set(r['min'] for r in runs))


def label(k):
    return 'off' if k >= 2147483647 else str(k)


groups = collections.OrderedDict()
for r in runs:
    groups.setdefault((r['screen'], r['win']), {})[r['min']] = r

print('| Screen | Window | Test | ' + ' | '.join('DMA ' + label(k) for k in mins) + ' |')
print('|---|---|---|' + '---:|' * len(mins))
for (screen, win), g in groups.items():
    for n in names:
        cells = []
        for k in mins:
            r = g.get(k)
            if r and n in r['tests']:
                ops, cyc = r['tests'][n]
                cells.append('%d (%.2f)' % (ops, cyc / 1e6))
            else:
                cells.append('-')
        print('| %s | %s | %s | %s |' % (screen, win, n, ' | '.join(cells)))

print()
print('| Screen | Window | DMA | jobs | MB | queue max | longest wait us | busy ch12 ms | busy ch13 ms |')
print('|---|---|---:|---:|---:|---:|---:|---:|---:|')
for r in runs:
    b = r['busy'] or ('-', '-', '-', '-')
    print('| %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (r['screen'], r['win'], label(r['min']), r['jobs'], r['mb'], b[0], b[1], b[2], b[3]))
