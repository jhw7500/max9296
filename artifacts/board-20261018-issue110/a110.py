"""#110 보드 측정 분석기. v110.sh 출력 디렉터리의 orig-*/new-* 를 읽어 부팅별로 판정·집계한다.

부팅마다 인스턴스(I2C:1=1-0048, I2C:2=2-0048)별로:
  dl  = start_fw_load -> end_fw_load (다운로드, 대조 구간)
  d1  = end_fw_load -> loaded firmware (100 ms 대기 + 새 빌드는 post_download 읽기)
  d3  = preview addr -> AP1302 boot check passed (새 빌드만: 설정 + post_config 읽기)
  pass/fail = AP1302 boot check passed/failed 줄 수
  isp = health_raw isp_boot (status, epoch_current, 검사 채널 값)
  errno = prepare 상태 줄 errno
사용: a110.py <out_dir> | a110.py --selftest
"""
import json
import re
import statistics
import sys
from pathlib import Path

TS = re.compile(r'^\[\s*(\d+\.\d+)\]')


def parse_dmesg(text):
    res = {}
    for line in text.splitlines():
        m = TS.match(line)
        if not m:
            continue
        t = float(m.group(1))
        inst = None
        m2 = re.search(r'\[I2C:(\d)\]', line)
        if m2:
            inst = m2.group(1)
        m3 = re.search(r'max9296 (\d)-0048: AP1302 boot check (passed|failed)', line)
        if m3:
            inst = m3.group(1)
        if inst is None:
            continue
        r = res.setdefault(inst, {'pass': 0, 'fail': 0})
        if 'start_fw_load' in line:
            r['start'] = t
        elif 'end_fw_load' in line:
            r['end'] = t
        elif re.search(r'loaded \S+ firmware', line):
            r['loaded'] = t
        elif 'preview addr=' in line:
            r.setdefault('preview', t)
        elif m3 and m3.group(2) == 'passed':
            r['pass'] += 1
            r['passed_at'] = t
        elif m3 and m3.group(2) == 'failed':
            r['fail'] += 1
    return res


def parse_state(text):
    lines = text.splitlines()
    out = {'boot': lines[0][:8] if lines else '', 'src': lines[1] if len(lines) > 1 else '',
           'cam': lines[2] if len(lines) > 2 else '', 'inst': {}}
    cur = None
    for line in lines[3:]:
        if line.startswith('== '):
            cur = line[3:4]
            out['inst'][cur] = {}
        elif cur and line.startswith('state='):
            m = re.search(r'errno=(-?\d+)', line)
            out['inst'][cur]['errno'] = int(m.group(1)) if m else None
        elif cur and line.startswith('{'):
            try:
                doc = json.loads(line)
            except ValueError:
                out['inst'][cur]['isp'] = 'BADJSON'
                continue
            boot = doc.get('isp_boot')
            if boot is None:
                out['inst'][cur]['isp'] = '-'
            else:
                vals = [c['value'] for c in boot['channels'] if c['checked']]
                out['inst'][cur]['isp'] = '%s/%s/%s/%s' % (
                    boot['status'], 'cur' if boot['epoch_current'] else 'old',
                    boot['stage'], ','.join(str(v) for v in vals))
    return out


def ms(a, b):
    return None if a is None or b is None else round((b - a) * 1000, 1)


def boot_row(d):
    dm = parse_dmesg((d / 'dmesg.txt').read_text(errors='replace'))
    st = parse_state((d / 'state.txt').read_text(errors='replace'))
    row = {'name': d.name, 'boot': st['boot'], 'src': st['src'][:8], 'cam': st['cam'], 'inst': {}}
    for inst in ('1', '2'):
        r = dm.get(inst, {'pass': 0, 'fail': 0})
        s = st['inst'].get(inst, {})
        row['inst'][inst] = {
            'dl': ms(r.get('start'), r.get('end')), 'd1': ms(r.get('end'), r.get('loaded')),
            'd3': ms(r.get('preview'), r.get('passed_at')), 'pass': r['pass'], 'fail': r['fail'],
            'isp': s.get('isp'), 'errno': s.get('errno')}
    return row


def verdict(row, phase):
    bad = []
    if row['cam'] != 'healthy':
        bad.append('cam_state')
    for inst, v in row['inst'].items():
        if v['errno'] != 0:
            bad.append(f'errno{inst}')
        if v['d1'] is None:
            bad.append(f'markers{inst}')
        if phase == 'new':
            if v['fail'] or v['pass'] < 1:
                bad.append(f'lines{inst}')
            if not (v['isp'] or '').startswith('OK/cur/post_config/'):
                bad.append(f'isp{inst}')
    return bad


def summarize(out):
    rows = {'orig': [], 'new': []}
    for d in sorted(Path(out).iterdir(), key=lambda p: (p.name.split('-')[0], int(p.name.split('-')[1]) if p.name[-1].isdigit() else 0)):
        if d.is_dir() and re.fullmatch(r'(orig|new)-\d+', d.name):
            rows[d.name.split('-')[0]].append(boot_row(d))
    failed = 0
    for phase in ('orig', 'new'):
        print(f'== {phase} ({len(rows[phase])} boots)')
        for row in rows[phase]:
            bad = verdict(row, phase)
            failed += bool(bad)
            cells = ' '.join(
                f"I2C:{i} dl={v['dl']} d1={v['d1']} d3={v['d3']} pass={v['pass']} fail={v['fail']} isp={v['isp']} errno={v['errno']}"
                for i, v in row['inst'].items())
            print(f"  {row['name']:7} boot={row['boot']} src={row['src']} cam={row['cam']} {cells} {'FAIL:' + ','.join(bad) if bad else 'ok'}")
        for key in ('dl', 'd1', 'd3'):
            vals = [v[key] for row in rows[phase] for v in row['inst'].values() if v[key] is not None]
            if vals:
                print(f'  {key}: n={len(vals)} median={statistics.median(vals):.1f} min={min(vals):.1f} max={max(vals):.1f} ms')
    d1o = [v['d1'] for r in rows['orig'] for v in r['inst'].values() if v['d1'] is not None]
    d1n = [v['d1'] for r in rows['new'] for v in r['inst'].values() if v['d1'] is not None]
    if d1o and d1n:
        delta = statistics.median(d1n) - statistics.median(d1o)
        print(f'd1 median delta (new-orig) = {delta:.1f} ms -> 3 reads per cold init ~ {3 * delta:.1f} ms (estimate)')
    print('BOOT_VERDICT', 'FAIL' if failed or not rows['new'] else 'PASS', f'(bad boots {failed})')
    return 1 if failed or not rows['new'] else 0


def selftest():
    """양성 대조: 실패 줄·FAILED 상태·errno·표식 누락을 분석기가 잡는지 본다."""
    good_dm = ('[ 22.1] [I2C:1][max9296.c:4635] start_fw_load\n[ 25.9] [I2C:1][max9296.c:4659] end_fw_load\n'
               '[ 26.0] [I2C:1][max9296.c:4782] loaded v4l-ap1302-ar0234.fw firmware (81616 bytes)\n'
               '[ 26.01] [I2C:1][max9296.c:5225] preview addr=0x3c output=1920x1080 fps=15 sensor_mode=KEEP\n'
               '[ 26.02] max9296 1-0048: AP1302 boot check passed: channel 1 R0x2000 0x0780 (expected 0x0780)\n')
    good_isp = '{"schema":1,"isp_boot":{"status":"OK","epoch_current":true,"stage":"post_config","channels":[{"channel":0,"checked":false,"errno":0,"value":null},{"channel":1,"checked":true,"errno":0,"value":1920}]}}'
    state = 'abcdefgh\nNEWSRC\nhealthy\n== 1-0048\nstate=READY errno=0 x\n' + good_isp + '\n'
    import tempfile
    cases = {
        'good': (good_dm, state, []),
        'fail_line': (good_dm + '[ 26.03] max9296 1-0048: AP1302 boot check failed: channel 1 stage post_download addr 0x3c R0x2000 0x0280 errno -5\n', state, ['lines1']),
        'isp_failed': (good_dm, state.replace('"status":"OK"', '"status":"FAILED"'), ['isp1']),
        'errno': (good_dm, state.replace('errno=0 x', 'errno=-5 x'), ['errno1']),
        'no_markers': ('', state, ['markers1', 'lines1']),
    }
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for name, (dm, st, want) in cases.items():
            d = Path(tmp) / name
            d.mkdir()
            (d / 'dmesg.txt').write_text(dm)
            (d / 'state.txt').write_text(st)
            row = boot_row(d)
            row['inst'] = {'1': row['inst']['1']}
            got = verdict(row, 'new')
            hit = all(w in got for w in want) and (bool(got) == bool(want))
            ok &= hit
            print(f'selftest {name}: got={got} want={want} {"ok" if hit else "MISMATCH"}')
    print('SELFTEST', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(selftest() if sys.argv[1:] == ['--selftest'] else summarize(sys.argv[1]))
