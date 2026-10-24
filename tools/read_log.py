#!/usr/bin/env python3
"""读回 CHARGE STATION 的过夜长记录（NVS 里的选 'CST3' blob）。

用法:
  python3 tools/read_log.py                 # 自动按 MAC 找 Cardputer 端口并读
  python3 tools/read_log.py /dev/cu.xxx     # 指定端口
  python3 tools/read_log.py --selftest      # 用合成数据自检解析器

注意: 读 flash 会让设备重启一次（数据都在 NVS 里，不会丢）。
"""
import struct, subprocess, sys, os

MAC = "50:78:7d:ce:af:40"
PY  = "/usr/local/bin/python3"
LOG_N = 144
CTOR = 24              # LogStore 里 rec[] 的偏移
REC  = 6               # LongRec 大小: mv(u16) t10(u16) st(u8) rsv(u8)
STATES = {0: "采样", 1: "正在充", 2: "慢充", 3: "已满", 4: "供不上", 5: "没在充", 6: "无电池"}


def parse(blob: bytes):
    # NVS 里同一个 blob 会留下多份历史副本：必须取"最新"的那份（count 最大，
    # 同 count 取偏移最大），取第一份会读到最旧的一夜开头。
    best = None
    s = 0
    while True:
        i = blob.find(b"CST3", s)
        if i < 0:
            break
        s = i + 1
        if i + CTOR > len(blob):
            continue
        count, head = struct.unpack_from("<HH", blob, i + 4)
        if count > LOG_N or head >= LOG_N:
            continue
        if best is None or count >= best[0]:
            best = (count, i, head)
    if best is None:
        return None
    count, i, head = best
    tplug, ttaper, tfull = struct.unpack_from("<III", blob, i + 8)
    flags = blob[i + 20]
    recs = []
    for k in range(LOG_N):
        o = i + CTOR + k * REC
        mv, t10, st, _ = struct.unpack_from("<HHBB", blob, o)
        recs.append((t10, mv, st, k))
    # head = 下一个写入位置 ⇒ 最老一条在 (head - count) mod LOG_N
    start = (head - count) % LOG_N
    ordered = [recs[(start + k) % LOG_N] for k in range(count)]
    return dict(count=count, head=head, flags=flags, tplug=tplug, ttaper=ttaper,
                tfull=tfull, recs=ordered)


def main():
    if "--selftest" in sys.argv:
        blob = bytearray(4096)
        blob[0:4] = b"CST3"
        struct.pack_into("<HH", blob, 4, 5, 5)                       # count=5, head=5
        struct.pack_into("<III", blob, 8, 300, 1800, 4200)            # 5m / 30m / 70m
        blob[20] = 0x07
        for k, (mv, t10) in enumerate([(3840, 0), (3852, 1), (3861, 2), (3870, 3), (3878, 4)]):
            struct.pack_into("<HHBB", blob, CTOR + k * REC, mv, t10, 1, 0)
        p = parse(bytes(blob))
        assert p and p["count"] == 5, p
        assert [r[1] for r in p["recs"]] == [3840, 3852, 3861, 3870, 3878], p["recs"]
        print("selftest PASS:", p["recs"])
        return

    port = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
    if not port:
        for cand in sorted(os.listdir("/dev")):
            if not cand.startswith("cu.usbmodem"):
                continue
            p = "/dev/" + cand
            r = subprocess.run([PY, "-m", "esptool", "-p", p, "chip-id"],
                               capture_output=True, text=True)
            if MAC in r.stdout:
                port = p
                break
    if not port:
        print("找不到 Cardputer（按 MAC 匹配）。插上 USB 数据线再试。")
        sys.exit(1)
    print("端口:", port)
    subprocess.run([PY, "-m", "esptool", "--chip", "esp32s3", "-p", port,
                    "read_flash", "0x9000", "0x5000", "/tmp/nvs_log.bin"],
                   check=True, stdout=subprocess.DEVNULL)
    p = parse(open("/tmp/nvs_log.bin", "rb").read())
    if not p:
        print("NVS 里没有日志（可能还没写第一条，或刷机后被清过）")
        sys.exit(0)

    print(f"记录 {p['count']} 条")
    if p["flags"]:
        print("事件: 插电台阶=%s 收尾=%s 满电=%s" % (
            f"{p['tplug']}s" if p["flags"] & 1 else "-",
            f"{p['ttaper']}s" if p["flags"] & 2 else "-",
            f"{p['tfull']}s" if p["flags"] & 4 else "-"))
    if not p["recs"]:
        sys.exit(0)
    t0 = p["recs"][0][0]
    print(f"{'时刻':>8}  {'电压':>7}  状态")
    for t10, mv, st, _ in p["recs"]:
        print(f"{(t10 - t0) * 10:>6}分  {mv/1000:>6.3f}V  {STATES.get(st, st)}")
    # 只统计"在充"的连续段（状态 1/2/3），避免把拔线后的跌落算进去
    ch = [r for r in p["recs"] if r[2] in (1, 2, 3)]
    if len(ch) >= 2:
        (ta, ma, _, _), (tb, mb, _, _) = ch[0], ch[-1]
        dt = (tb - ta) * 10
        if dt > 0:
            print(f"\n充电段 {len(ch)} 点 / {dt} 分：{ma/1000:.3f}V → {mb/1000:.3f}V"
                  f"（Δ{mb-ma}mV）平均 {(mb-ma)/dt:+.2f} mV/min")
            # 1750mAh + 电压→电量表（本机 30 分钟采样只能给量级，别当精确值）
            tbl = [(3300,0),(3600,10),(3700,25),(3790,50),(3900,70),(4000,85),(4100,95),(4200,100)]
            def pct(mv):
                if mv <= tbl[0][0]: return 0
                if mv >= tbl[-1][0]: return 100
                for x1, y1 in zip(range(1, len(tbl)), range(1, len(tbl))):
                    if mv < tbl[x1][0]:
                        x0, y0 = tbl[x1-1]; xa, ya = tbl[x1]
                        return y0 + (mv - x0) * (ya - y0) // (xa - x0)
                return 100
            dsoc = pct(mb) - pct(ma)
            mah = 1750 * dsoc / 100.0
            print(f"电量 {pct(ma)}% → {pct(mb)}%（+{dsoc}%），约 {mah:.0f}mAh"
                  f" ⇒ 净充电电流 ≈ {mah/(dt/60.0):.0f} mA")


if __name__ == "__main__":
    main()
