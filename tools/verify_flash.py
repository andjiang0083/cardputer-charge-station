#!/usr/bin/env python3
"""比对设备 flash 与本地构建产物：只比"固件该占"的区域，跳过 app 会写的 NVS/otadata。"""
import hashlib, subprocess, sys, os

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem5"
ROOT = "/Users/jiangweizhong/esp32/charge-station/.pio/build/cardputer"
FACT = os.path.join(ROOT, "firmware.factory.bin")
APP  = os.path.join(ROOT, "firmware.bin")
ESPT = "/usr/local/bin/python3"

def read(port, addr, size, out):
    # 注意: -b 460800 在这台机器上会 "Serial data stream stopped"，只能用默认波特率
    subprocess.run([ESPT, "-m", "esptool", "--chip", "esp32s3", "-p", port,
                    "read_flash", hex(addr), hex(size), out],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return open(out, "rb").read()

def h(b): return hashlib.sha256(b).hexdigest()[:16]

fact = open(FACT, "rb").read()
app  = open(APP, "rb").read()
print(f"本地 factory={len(fact)}B sha256:{h(fact)}")
print(f"本地 app    ={len(app)}B sha256:{h(app)}")

ok = True
# 1) bootloader + 分区表 (0x0..0x9000)：固件固定区，app 不会改
dev = read(PORT, 0x0, 0x9000, "/tmp/dev_boot.bin")
same = h(dev) == h(fact[:0x9000])
print(f"[1] bootloader+分区表 0x0..0x9000 : {'一致' if same else '不一致'}  ({h(dev)} vs {h(fact[:0x9000])})")
ok &= same

# 2) app 区 (0x10000..)：核心校验（分块读，单次大块会中断）
CH = 0x10000
h2 = hashlib.sha256()
got = 0
i = 0
while got < len(app):
    n = min(CH, len(app) - got)
    d = read(PORT, 0x10000 + got, n, f"/tmp/dev_app_{i}.bin")
    if len(d) != n:
        print(f"[2] 分块读取长度异常 @0x{0x10000+got:x}: {len(d)} != {n}")
        break
    h2.update(d); got += n; i += 1
dev_hash = h2.hexdigest()[:16]
same = (got == len(app)) and (dev_hash == h(app))
print(f"[2] app 0x10000 +{len(app)}B ({i} 块) : {'一致' if same else '不一致'}  ({dev_hash} vs {h(app)})")
ok &= same

# 3) otadata + NVS：设备运行时会自己写，只报状态不作判据
dev = read(PORT, 0x9000, 0x5000, "/tmp/dev_nvs.bin")
nonff = len(dev) - dev.count(b"\xff")
print(f"[3] NVS 0x9000 (运行时数据)      : {nonff}B 非空 -> 值会变，不参与比对")

# 4) 设备里真的写着 v1.0.4 ?
blob = b"".join(open(f"/tmp/dev_app_{k}.bin", "rb").read() for k in range(i))
print(f"[4] 设备 app 内含 'v1.0.4' 串    : {'是' if b'v1.0.4' in blob else '否'}")
print("\n结论:", "设备上的固件 == 当前源码构建产物" if ok else "设备内容与本地产物不一致，需要重刷")
