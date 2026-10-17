#!/usr/bin/env python3
"""#111 수정 AC 판정: fix+T111 오버레이의 journalctl -o short-iso-precise 캡처를 읽는다.

사용: fix111_check.py <follow.log> [<since ISO> <until ISO>] [--gap <v111 out dir> --gap-node <3|4>]

A1  정지한 버스에 대한 RUNNING 전환 (s_stream(0) 반환 뒤 ~ 다음 s_stream(1) 반환 전) = 0
    s_stream(0)/s_stream(1) 반환 스냅샷의 자기 f (state.fsync) = 0, STALE 표시 = 0
A2  s_stream(1) ret=0 사이클마다 그 버스의 첫 RUNNING 전환이 첫 'CSI output enabled' 보다 먼저
G   (--gap) --gap-node 로 고른 계속 도는 노드의 CSI IRQ 를 상대 정지 뒤 4 s, 상대 재시작 뒤 6 s 구간별로
    본다. 구간마다 IRQ 증가 1회 이상, 최대 공백이 정지 구간 GAP_STOP_MAX_S·재시작 구간
    GAP_START_MAX_S 이하여야 한다.
"""
import re
import sys
from datetime import datetime, timezone

TS = re.compile(r'^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+)')
RET = re.compile(r'T111 #\d+ s_stream\((\d)\) ret=(-?\d+) hw\d+ b(\d)\[st(\d) on(\d) rs(\d) i(\d) e(\d) f(\d)')
CSI = re.compile(r'\[I2C:(\d)\]\[max9296\.c:\d+\] CSI output enabled')
RUN = re.compile(r'fsync->RUNNING tgt=b(\d) via=(\S+) prev=(\d)( STALE)?')


def ts(line):
    m = TS.match(line)
    return datetime.fromisoformat(m.group(1)) if m else None


def journal(path, since, until):
    ev = []
    for line in open(path, errors='replace'):
        t = ts(line)
        if t is None or (since and t < since) or (until and t > until):
            continue
        if m := RET.search(line):
            ev.append((t, 'ret', int(m.group(3)), int(m.group(1)), int(m.group(2)), int(m.group(9))))
        elif m := CSI.search(line):
            ev.append((t, 'csi', int(m.group(1)), None, None, None))
        elif m := RUN.search(line):
            ev.append((t, 'run', int(m.group(1)), None, bool(m.group(4)), int(m.group(3))))
    return ev


def check_journal(ev):
    streaming = {1: None, 2: None}  # None = 아직 모름, True/False
    cycles = {1: [], 2: []}
    cur = {1: None, 2: None}
    bad = []
    stale = 0
    f_on_start = f_on_stop = 0
    starts = stops = start_fail = stop_fail = 0
    for t, kind, bus, en, x, f in ev:
        if kind == 'ret':
            ret = x
            if en == 1:
                if ret != 0:
                    start_fail += 1
                    bad.append(f'{t.time()} b{bus} s_stream(1) ret={ret}')
                    continue
                starts += 1
                if f != 0:
                    f_on_start += 1
                    bad.append(f'{t.time()} b{bus} s_stream(1) 반환 시 f={f} (정지 상태에서 RUNNING 승계)')
                streaming[bus] = True
                cur[bus] = {'t': t, 'run': None, 'csi': None}
                cycles[bus].append(cur[bus])
            else:
                stops += 1
                if ret != 0:
                    stop_fail += 1
                    bad.append(f'{t.time()} b{bus} s_stream(0) ret={ret} (정지 실패)')
                if f != 0:
                    f_on_stop += 1
                    bad.append(f'{t.time()} b{bus} s_stream(0) 반환 시 f={f}')
                streaming[bus] = False
                cur[bus] = None
        elif kind == 'run':
            if x:
                stale += 1
                bad.append(f'{t.time()} b{bus} RUNNING STALE')
            if streaming[bus] is False:
                bad.append(f'{t.time()} b{bus} 정지 중 RUNNING 전환')
            elif cur[bus] is not None and cur[bus]['run'] is None:
                cur[bus]['run'] = t
        elif kind == 'csi':
            if cur[bus] is not None and cur[bus]['csi'] is None:
                cur[bus]['csi'] = t
    order_bad = no_csi = 0
    d_run, d_csi = [], []
    for bus in (1, 2):
        for c in cycles[bus]:
            if c['csi'] is None:
                no_csi += 1
                bad.append(f"{c['t'].time()} b{bus} s_stream(1) 사이클에 0x0313 기록 없음")
                continue
            if c['run'] is None or c['run'] > c['csi']:
                order_bad += 1
                bad.append(f"{c['csi'].time()} b{bus} 0x0313 이 첫 RUNNING 전환보다 먼저 (commit {c['t'].time()})")
                continue
            d_run.append((c['run'] - c['t']).total_seconds())
            d_csi.append((c['csi'] - c['t']).total_seconds())
    if not d_run:
        # 빈 입력, T111 이 없는 캡처, 모든 이벤트를 뺀 시간 범위는 증거가 아니다.
        bad.append('판정할 완결 시작 사이클(s_stream(1) ret=0 → RUNNING → 0x0313)이 없음')
    print(f'starts(ret=0)={starts} start_fail={start_fail} stops={stops} stop_fail={stop_fail}')
    print(f'A1 정지 중 RUNNING 전환={sum("정지 중 RUNNING" in b for b in bad)} STALE={stale} '
          f'f!=0@start={f_on_start} f!=0@stop={f_on_stop}')
    print(f'A2 사이클={sum(len(v) for v in cycles.values())} 순서위반={order_bad} CSI없음={no_csi}')
    if d_run:
        print(f'   commit→첫 RUNNING  min {min(d_run):.3f} max {max(d_run):.3f} s')
        print(f'   commit→0x0313      min {min(d_csi):.3f} max {max(d_csi):.3f} s')
    for b in bad[:30]:
        print('  !', b)
    return not bad


# 15 fps 에서 CSI IRQ 는 약 33 ms 마다 온다(50 ms 샘플). 2026-10-17 측정: 정지 구간 최대 공백
# 0.07~0.14 s, 재시작 구간 1.01~1.08 s(dual 분기 워밍업 1 s). 상한은 그 위에 여유를 둔 값이다.
GAP_STOP_MAX_S = 0.5
GAP_START_MAX_S = 1.5


def check_gap(outdir, node):
    marks = []
    for line in open(f'{outdir}/summary.txt', errors='replace'):
        m = re.match(r'(\S+) G(\d) (?:k\d )?(stop|start) video(\d)', line)
        if m:
            t = datetime.fromisoformat(m.group(1)).replace(tzinfo=timezone.utc).timestamp()
            marks.append((t, int(m.group(2)), m.group(3), int(m.group(4))))
    col = 2 if node == 4 else 1  # irq 파일: epoch csi0 csi1, video3=CSI0, video4=CSI1
    rows = []
    try:
        irq_lines = open(f'{outdir}/irq-g{node}.txt', errors='replace').readlines()
    except FileNotFoundError:
        irq_lines = []
    for line in irq_lines:
        p = line.split()
        if len(p) == 3:
            rows.append((float(p[0]), int(p[col])))
    inc = [rows[i][0] for i in range(1, len(rows)) if rows[i][1] > rows[i - 1][1]]
    print(f'G{node} (video{node} 계속 스트리밍, CSI{col - 1}) 샘플 {len(rows)}')
    windows = failed = 0
    for t0, g, what, other in marks:
        if g != node:
            continue
        span, limit = (4.0, GAP_STOP_MAX_S) if what == 'stop' else (6.0, GAP_START_MAX_S)
        w = [t for t in inc if t0 <= t <= t0 + span]
        gap = max(b - a for a, b in zip([t0] + w, w + [t0 + span]))
        bad = not w or gap > limit
        failed += bad
        print(f'   video{other} {what:5s} 뒤 {span:.0f}s: 증가 {len(w)}회, 최대 공백 {gap:.2f}s '
              f'(상한 {limit:.1f}s){" ! 기준 밖" if bad else ""}')
        windows += 1
    if windows == 0:
        print(f'  ! G{node} 공백을 잴 구간(summary 의 G{node} 표식과 irq-g{node}.txt)이 없음')
    return windows > 0 and failed == 0


def main(argv):
    gap = node = None
    if '--gap' in argv:
        i = argv.index('--gap')
        gap = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if '--gap-node' in argv:
        i = argv.index('--gap-node')
        node = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if (gap is None) != (node is None) or node not in (None, '3', '4'):
        print('usage: --gap <dir> 와 --gap-node <3|4> 는 함께 준다', file=sys.stderr)
        return 2
    since = datetime.fromisoformat(argv[2]) if len(argv) > 2 else None
    until = datetime.fromisoformat(argv[3]) if len(argv) > 3 else None
    ok = check_journal(journal(argv[1], since, until))
    if gap and not check_gap(gap, int(node)):
        ok = False
    print('JOURNAL_VERDICT', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
