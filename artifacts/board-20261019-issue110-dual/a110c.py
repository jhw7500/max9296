"""#110 AC1·AC3 분석기 2판 (192.168.214.6). a110b.py 의 두 결함을 고친다.

a110b 결함: (1) 두 버스 4개 주소가 모두 읽기 실패인 부팅(마지막 시도 실패 뒤 전원 주기가 바뀐 상태,
health_raw epoch_current=false)을 정답 '기동 안 함'으로 셌다. (2) 정답상 실패 인스턴스의 모든 채널에
errno 를 요구해, 드라이버가 정상으로 본 짝 채널까지 누락으로 셌다.

정답(독립, 가동 75 s 의 i2ctransfer R0x2000): 4개 주소가 모두 읽기 실패면 'link_down' 으로 정답 없음.
  그 밖에는 ISP별로 읽기 실패·0x0000·0x0280 이면 기동 안 함.
시도 단위 대조(new 단계): 인스턴스별 dmesg 'boot check failed/passed' 줄을 2 s 간격으로 묶어 시도로 보고
  (실패 줄이 있으면 F, 통과 줄만 있으면 P), 같은 부팅의 gstApp [MAX9296_PREPARE] 기록(action!=2)을
  primary_errno 로 F/O 로 바꿔 순서를 맞춘다. 마지막 시도는 health_raw isp_boot 와 단계·채널·errno·값이
  같아야 한다. 정답이 있는 인스턴스는 정답 실패인데 마지막 시도가 P 면 missed, 정답 정상인데 epoch_current
  true 로 FAILED 면 false_failure.
base 단계: 검출이 없으므로 정답 실패 + 그 인스턴스 마지막 prepare errno 0 을 silent 로 센다.
사용: a110c.py <out_dir> | a110c.py --selftest
"""
import datetime
import json
import re
import statistics
import sys
from pathlib import Path

ROM = 0x0280
EXPECTED = 0x0500  # dual 2560x720 → ISP별 1280
KST = datetime.timedelta(hours=9)
GAP_S = 2.0


def parse_state(text):
    lines = text.splitlines()
    st = {'boot': lines[0][:8] if lines else '', 'src': lines[1] if len(lines) > 1 else '',
          'cam': lines[3] if len(lines) > 3 else '', 'uptime': None, 'boot_wall': None,
          'isp_boot': {}, 'truth': {}, 'truth_text': {}}
    if len(lines) > 2:
        try:
            st['uptime'] = float(lines[2].split()[0])
        except (ValueError, IndexError):
            pass
    cur = None
    for line in lines:
        m = re.match(r'boot_wall (\S+)', line)
        if m:
            st['boot_wall'] = datetime.datetime.fromisoformat(m.group(1))
            continue
        m = re.match(r'== ([12])-0048', line)
        if m:
            cur = ('inst', m.group(1))
            continue
        if line.startswith('== '):
            cur = ('truth', None) if line.startswith('== r0x2000') else None
            continue
        if cur and cur[0] == 'inst' and line.startswith('{'):
            try:
                st['isp_boot'][cur[1]] = json.loads(line).get('isp_boot')
            except ValueError:
                st['isp_boot'][cur[1]] = 'BADJSON'
        elif cur and cur[0] == 'truth':
            m = re.match(r'bus([12]) (0x1[12]) (.*)$', line)
            if m:
                key = (m.group(1), 0 if m.group(2) == '0x11' else 1)
                hx = re.findall(r'0x([0-9a-fA-F]{2})', m.group(3))
                st['truth'][key] = int(hx[0] + hx[1], 16) if len(hx) == 2 else None
                st['truth_text'][key] = m.group(3).strip()
    return st


def parse_dmesg(text):
    ev = {'1': [], '2': []}
    for line in text.splitlines():
        m = re.search(r'^\[\s*([\d.]+)\] max9296 ([12])-0048: AP1302 boot check failed: channel (\d+) stage (\S+) '
                      r'addr (0x\w+) R0x2000 (0x\w+).* errno (-?\d+)', line)
        if m:
            ev[m.group(2)].append({'t': float(m.group(1)), 'kind': 'F', 'channel': int(m.group(3)),
                                   'stage': m.group(4), 'addr': m.group(5), 'value': int(m.group(6), 16),
                                   'errno': int(m.group(7))})
            continue
        m = re.search(r'^\[\s*([\d.]+)\] max9296 ([12])-0048: AP1302 boot check passed: channel (\d+) R0x2000 (0x\w+)', line)
        if m:
            ev[m.group(2)].append({'t': float(m.group(1)), 'kind': 'P', 'channel': int(m.group(3)),
                                   'value': int(m.group(4), 16)})
    attempts = {}
    for inst, es in ev.items():
        groups = []
        for e in sorted(es, key=lambda x: x['t']):
            if groups and e['t'] - groups[-1][-1]['t'] <= GAP_S:
                groups[-1].append(e)
            else:
                groups.append([e])
        attempts[inst] = [{'t': g[0]['t'], 'kind': 'F' if any(e['kind'] == 'F' for e in g) else 'P', 'events': g}
                          for g in groups]
    return attempts


def parse_prepare(text, boot_wall, uptime):
    """이 부팅 창 안의 인스턴스별 prepare 기록 (action!=2) 목록."""
    res = {'1': [], '2': []}
    if boot_wall is None:
        return res
    end = boot_wall + datetime.timedelta(seconds=(uptime or 0) + 60)
    for line in text.splitlines():
        m = re.match(r'(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+) .*\[MAX9296_PREPARE\].*path=/sys/bus/i2c/devices/'
                     r'([12])-0048/prepare.*action=(\d+) elapsed_ms=(\d+) primary_errno=(-?\d+)', line)
        if not m:
            continue
        ts = datetime.datetime.fromisoformat(m.group(1)) - KST
        if not (boot_wall <= ts <= end) or m.group(3) == '2':
            continue
        res[m.group(2)].append({'s': round((ts - boot_wall).total_seconds(), 1), 'action': int(m.group(3)),
                                'elapsed_ms': int(m.group(4)), 'errno': int(m.group(5)),
                                'kind': 'F' if int(m.group(5)) != 0 else 'P'})
    return res


def booted(val):
    return val is not None and val not in (0, ROM)


def judge(d, phase):
    st = parse_state((d / 'state.txt').read_text(errors='replace'))
    att = parse_dmesg((d / 'dmesg.txt').read_text(errors='replace'))
    pf = d / 'prepare-log.txt'
    prep = parse_prepare(pf.read_text(errors='replace') if pf.exists() else '', st['boot_wall'], st['uptime'])
    vals = [st['truth'].get((i, c)) for i in ('1', '2') for c in (0, 1)]
    link_down = len(vals) == 4 and all(v is None for v in vals)
    row = {'name': d.name, 'boot': st['boot'], 'src': st['src'][:8], 'link_down': link_down,
           'inst': {}, 'issues': [], 'silent': [], 'attempts_f': []}
    for inst in ('1', '2'):
        tv = [st['truth'].get((inst, c)) for c in (0, 1)]
        bad = [c for c in (0, 1) if not booted(tv[c])] if not link_down else []
        ib = st['isp_boot'].get(inst)
        a = att[inst]
        p = prep[inst]
        cell = {'truth': 'n/a' if link_down else ','.join('%04x' % v if v is not None else 'ERR' for v in tv),
                'dm': ''.join(x['kind'] for x in a), 'pl': ''.join(x['kind'] for x in p),
                'pl_errno': [x['errno'] for x in p if x['errno']],
                'hr': (f"{ib['status']}/{ib['stage']}/{'cur' if ib['epoch_current'] else 'old'}"
                       if isinstance(ib, dict) else str(ib))}
        row['inst'][inst] = cell
        for x in a:
            if x['kind'] == 'F':
                for e in x['events']:
                    if e['kind'] == 'F':
                        row['attempts_f'].append((inst, e['channel'], e['stage'], e['addr'], e['value'], e['errno']))
        if phase == 'base':
            if bad and p and p[-1]['kind'] == 'P':
                row['silent'].append((inst, bad, cell['truth']))
            continue
        # new 단계
        if not a:
            row['issues'].append(f'{inst}:no_check_log')
            continue
        if cell['dm'] != cell['pl']:
            row['issues'].append(f"{inst}:seq_mismatch(dm={cell['dm']},pl={cell['pl']})")
        for x in a:
            if x['kind'] == 'F' and any(e['kind'] == 'F' and e['errno'] == 0 for e in x['events']):
                row['issues'].append(f'{inst}:fail_line_errno0')
        if not isinstance(ib, dict):
            row['issues'].append(f'{inst}:no_isp_boot')
            continue
        last = a[-1]
        chans = {c['channel']: c for c in ib['channels']}
        if last['kind'] == 'F':
            fe = [e for e in last['events'] if e['kind'] == 'F']
            if ib['status'] != 'FAILED' or any(e['stage'] != ib['stage'] for e in fe):
                row['issues'].append(f"{inst}:hr_mismatch({cell['hr']})")
            for e in fe:
                c = chans.get(e['channel'])
                if not c or c['errno'] != e['errno'] or c['value'] != e['value']:
                    row['issues'].append(f"{inst}:hr_ch{e['channel']}_mismatch")
            if p and p[-1]['kind'] != 'F':
                row['issues'].append(f'{inst}:last_prepare_errno0')
        else:
            if ib['status'] != 'OK' or ib['stage'] != 'post_config':
                row['issues'].append(f"{inst}:hr_mismatch({cell['hr']})")
            if any(c['errno'] != 0 or c['value'] != EXPECTED for c in ib['channels']):
                row['issues'].append(f'{inst}:hr_value_mismatch')
            if bad:
                row['issues'].append(f'{inst}:missed(truth={cell["truth"]})')
        if not bad and not link_down and ib['status'] == 'FAILED' and ib['epoch_current']:
            row['issues'].append(f'{inst}:false_failure')
    return row, prep


def summarize(out):
    rows = {'base': [], 'new': []}
    for d in Path(out).iterdir():
        m = re.fullmatch(r'(base|new)-(\d+)', d.name)
        if d.is_dir() and m:
            rows[m.group(1)].append((int(m.group(2)), d))
    verdict_issues = 0
    elapsed = {}
    for phase in ('base', 'new'):
        rs = []
        for _, d in sorted(rows[phase]):
            r, prep = judge(d, phase)
            rs.append(r)
            for inst, ps in prep.items():
                for x in ps:
                    if x['errno'] == 0 and x['action'] == 4:
                        elapsed.setdefault((phase, inst), []).append(x['elapsed_ms'])
        valid = [r for r in rs if not r['link_down']]
        print(f'== {phase}: {len(rs)} boots, link_down(정답 없음) {len(rs) - len(valid)}, 정답 있는 부팅 {len(valid)}')
        for r in rs:
            cells = ' | '.join(f"{i}-0048 truth={c['truth']} dm={c['dm'] or '-'} pl={c['pl'] or '-'}{c['pl_errno'] or ''} hr={c['hr']}"
                               for i, c in r['inst'].items())
            flag = ('ISSUES:' + ','.join(r['issues'])) if r['issues'] else ('SILENT:' + str(r['silent']) if r['silent'] else 'ok')
            verdict_issues += bool(r['issues'])
            print(f"  {r['name']:7} {r['boot']} {cells} {flag}")
        if phase == 'base':
            sil = [r for r in valid if r['silent']]
            print(f'  base silent failure boots {len(sil)}/{len(valid)} (정답 있는 부팅 중)')
        else:
            fb = [r for r in rs if r['attempts_f']]
            fa = [x for r in rs for x in r['attempts_f']]
            first_f = sum(1 for r in rs if any(c['dm'][:1] == 'F' for c in r['inst'].values()))
            kinds = {}
            for x in fa:
                kinds[(x[0], x[2], x[3], hex(x[4]), x[5])] = kinds.get((x[0], x[2], x[3], hex(x[4]), x[5]), 0) + 1
            print(f'  new: 실패 시도가 있는 부팅 {len(fb)}/{len(rs)}, 첫 시도 실패 부팅 {first_f}/{len(rs)}, 실패 줄 {len(fa)}')
            for k, v in sorted(kinds.items()):
                print(f'    {k[0]}-0048 {k[1]} addr {k[2]} R0x2000 {k[3]} errno {k[4]}: {v}')
            print(f'  new: 정답 있는 부팅의 missed/false_failure = '
                  f"{sum(1 for r in valid for i in r['issues'] if 'missed' in i or 'false_failure' in i)}")
    for (phase, inst), xs in sorted(elapsed.items()):
        print(f'elapsed_ms cold OK {phase} {inst}-0048: n={len(xs)} median={statistics.median(xs)} min={min(xs)} max={max(xs)}')
    n_new = len(rows['new'])
    n_fail_new = sum(1 for _, d in rows['new'] if judge(d, 'new')[0]['attempts_f'])
    if verdict_issues:
        verdict = 'FAIL'
    elif n_new == 0 or n_fail_new == 0:
        verdict = 'NOT_JUDGED'
    else:
        verdict = 'PASS'
    print('AC1_VERDICT', verdict, f'(boots with issues {verdict_issues}, new boots {n_new}, new boots with failed attempts {n_fail_new})')
    return 0 if verdict == 'PASS' else 1


def selftest():
    import tempfile
    head = 'abcdefgh-1\nNEWSRC\n80.1 1.0\nhealthy\n\nboot_wall 2026-10-19T00:00:00\n'

    def inst(n, status, stage, chans, cur=True):
        js = json.dumps({'schema': 1, 'isp_boot': {'status': status, 'epoch_current': cur, 'stage': stage, 'channels': chans}})
        return f'== {n}-0048\nstate=READY errno=0 x\n{js}\n'
    ok2 = [{'channel': 0, 'checked': True, 'errno': 0, 'value': 1280}, {'channel': 1, 'checked': True, 'errno': 0, 'value': 1280}]
    ok1 = [{'channel': 2, 'checked': True, 'errno': 0, 'value': 1280}, {'channel': 3, 'checked': True, 'errno': 0, 'value': 1280}]
    fail1 = [{'channel': 2, 'checked': True, 'errno': -5, 'value': 640}, {'channel': 3, 'checked': True, 'errno': 0, 'value': 1920}]
    t_ok = '== r0x2000\nbus1 0x11 0x05 0x00 \nbus1 0x12 0x05 0x00 \nbus2 0x11 0x05 0x00 \nbus2 0x12 0x05 0x00 \n'
    t_down = ('== r0x2000\n' + ''.join(f'bus{b} {a} Error: Sending messages failed: No such device or address \n'
                                       for b in (1, 2) for a in ('0x11', '0x12')))
    t_a = t_ok.replace('bus1 0x11 0x05 0x00', 'bus1 0x11 0x02 0x80')
    t_one_err = t_ok.replace('bus2 0x12 0x05 0x00', 'bus2 0x12 Error: Sending messages failed: No such device or address')

    def pl(n, s, action, errno):
        csi = 'CSI1' if n == 1 else 'CSI0'
        return (f'2026-10-19 09:00:{s:02d}.000 gstApp[notice] [MAX9296_PREPARE] generation=1 {csi} '
                f'path=/sys/bus/i2c/devices/{n}-0048/prepare tuple=2560x720@15 enable=3 action={action} '
                f'elapsed_ms=12000 primary_errno={errno} rollback_errno=0\n')
    fl = '[   28.2] max9296 1-0048: AP1302 boot check failed: channel 2 stage post_download addr 0x11 R0x2000 0x0280 errno -5\n'
    fl2 = fl.replace('28.2', '62.9')

    def pas(n, t):
        chs = (2, 3) if n == 1 else (0, 1)
        return ''.join(f'[   {t}] max9296 {n}-0048: AP1302 boot check passed: channel {c} R0x2000 0x0500 (expected 0x0500)\n'
                       for c in chs)
    good_dm = pas(1, 28.5) + pas(2, 28.6)
    good_pl = pl(1, 30, 4, 0) + pl(2, 30, 4, 0)
    cases = {
        'ok': (head + inst(1, 'OK', 'post_config', ok1) + inst(2, 'OK', 'post_config', ok2) + t_ok, good_dm, good_pl, []),
        # 두 번 실패해 마지막이 F, 4개 주소 ENXIO: 정답 없음이지 missed 아님 (a110b 는 여기서 이슈를 냈다)
        'final_fail_link_down': (head + inst(1, 'FAILED', 'post_download', fail1, False) + inst(2, 'OK', 'post_config', ok2, False) + t_down,
                                 fl + pas(2, 28.6) + fl2 + pas(2, 63.3),
                                 pl(1, 30, 5, -5) + pl(2, 30, 4, 0) + pl(1, 59, 5, -5) + pl(2, 59, 4, 0), []),
        'retry_recovered': (head + inst(1, 'OK', 'post_config', ok1) + inst(2, 'OK', 'post_config', ok2) + t_ok,
                            fl + pas(2, 28.6) + pas(1, 58.0) + pas(2, 58.1),
                            pl(1, 30, 5, -5) + pl(2, 30, 4, 0) + pl(1, 59, 4, 0) + pl(2, 59, 4, 0), []),
        'missed': (head + inst(1, 'OK', 'post_config', ok1) + inst(2, 'OK', 'post_config', ok2) + t_a, good_dm, good_pl, ['1:missed']),
        # 한 주소만 ENXIO 면 link_down 이 아니다 — 놓친 유형 B 를 잡아야 한다(양성 대조)
        'one_addr_enxio_missed': (head + inst(1, 'OK', 'post_config', ok1) + inst(2, 'OK', 'post_config', ok2) + t_one_err,
                                  good_dm, good_pl, ['2:missed']),
        'hr_mismatch': (head + inst(1, 'OK', 'post_config', ok1) + inst(2, 'OK', 'post_config', ok2) + t_down,
                        fl + pas(2, 28.6), pl(1, 30, 5, -5) + pl(2, 30, 4, 0), ['1:hr_mismatch']),
        'prepare_errno0': (head + inst(1, 'FAILED', 'post_download', fail1, False) + inst(2, 'OK', 'post_config', ok2) + t_down,
                           fl + pas(2, 28.6), pl(1, 30, 4, 0) + pl(2, 30, 4, 0), ['1:seq_mismatch']),
        'no_check_log': (head + inst(1, 'OK', 'post_config', ok1) + inst(2, 'OK', 'post_config', ok2) + t_ok,
                         pas(2, 28.6), good_pl, ['1:no_check_log']),
        'false_failure': (head + inst(1, 'FAILED', 'post_download', fail1) + inst(2, 'OK', 'post_config', ok2) + t_ok,
                          fl + pas(2, 28.6), pl(1, 30, 5, -5) + pl(2, 30, 4, 0), ['1:false_failure']),
    }
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for name, (state, dm, pls, want) in cases.items():
            d = Path(tmp) / 'new-1'
            d.mkdir(exist_ok=True)
            (d / 'state.txt').write_text(state)
            (d / 'dmesg.txt').write_text(dm)
            (d / 'prepare-log.txt').write_text(pls)
            got = judge(d, 'new')[0]['issues']
            hit = all(any(g.startswith(w) for g in got) for w in want) and bool(got) == bool(want)
            ok &= hit
            print(f'selftest {name}: got={got} want={want} {"ok" if hit else "MISMATCH"}')
    print('SELFTEST', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(selftest() if sys.argv[1:] == ['--selftest'] else summarize(sys.argv[1]))
