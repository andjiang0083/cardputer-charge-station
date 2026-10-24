// ============================================================
// CHARGE STATION v1.0.11 — M5Cardputer-Adv 充电座固件
//
// 做一件事: 插电充电时，把整机负载压到最低，让 TP4057 的 500mA
// 尽量全进电池；同时把"到底有没有在充"画成电压曲线摆到屏幕上。
//
// 硬件事实（见 SPEC.md）:
//   - 充电 IC = TP4057 线性，硬件上限 500mA，软件不可调
//   - 无 power-path：系统负载直接从电池节点取电
//   - 读不到充电状态/电流，只有 G10 ADC 电压（×2）
//   - 开关 OFF = 电池断开 ⇒ 必须开机才充
//
// v0.1.1: 环缓冲持久化到 NVS（会话秒用累计基线，跨重启斜率继续有效）
// v0.1.2: ① 斜率阈值按 1750mAh 重标 ② 加 40min 曲线 ③ R 常亮 5min 自动回
// v1.0.3: NVS 电压校准偏移（+/- 10mV），STAY 页显示 raw/cal
// v1.0.4: 读数有效性判定（3.0~4.3V + 8 次极差 ≤60mV），脏点不入环
// v1.0.5: 修环缓冲索引（最老样本在 head-count）；阈值降到噪声地板
// v1.0.6: 过夜长记录（10min×144=24h）+ 事件打点 + 斜率锚定插电台阶之后
// v1.0.7: 中英双语 + 开机选语言（1 分钟超时，默认英文）
// v1.0.11: 加净充电电流估算显示；9 键清空长记录
// v1.0.8: 修选择页按键不生效 —— 增加 isKeyPressed 兜底 + 屏幕回显按键 + 1/2 随时切语言
// ============================================================
#include <M5Cardputer.h>
#include <lgfx/v1/LGFXBase.hpp>
#include <Preferences.h>
#include <math.h>

// ---------------- 参数 ----------------
static const uint32_t SAMPLE_MS    = 30000;   // 采样周期
static const int      RING_N       = 80;      // 环缓冲 80 点 = 40 min
static const uint32_t SCREEN_HOLD  = 3000;    // 按键亮屏时长
static const uint32_t STAY_MS      = 300000;  // R 常亮上限 5 min
static const uint32_t LANG_WAIT_MS = 60000;   // 开机选语言等待时间
static const uint8_t  BRIGHT_UI    = 70;
static const int      KEY_INT_PIN  = 11;      // TCA8418 INT (低有效)
static const uint16_t VBAT_FULL    = 4150;    // mV
static const int      WIN_MIN      = 20;      // 斜率窗口最少 20 采样 = 10 min
static const int      WIN_MAX      = 40;      // 优先用 20 min 窗口
static const int      TH_STEP_MV   = 20;      // IR-drop 台阶阈值
// 1750mAh + ~400mA ⇒ 真实斜率量级 1~4 mV/min；20min 窗口噪声地板约 ±0.25mV/min
static const float    TH_FAST      = 1.0f;
static const float    TH_TAPER     = 0.2f;
static const float    TH_DROP      = -0.2f;
static const uint32_t PERSIST_MS   = 300000;  // 5 min 落一次 NVS

// 颜色
static const uint16_t C_AMBER = 0xFD20;
static const uint16_t C_GREEN = 0x07E0;
static const uint16_t C_RED   = 0xF800;
static const uint16_t C_GRAY  = 0x8410;
static const uint16_t C_DIM   = 0x5AEB;
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_BOX   = 0x10A2;

enum CState : uint8_t { CS_SAMPLING, CS_FAST, CS_TAPER, CS_FULL, CS_WEAK, CS_NOUSB, CS_NOBATT };

struct Sample { uint16_t mv; uint32_t t; };   // t = 会话绝对秒（跨重启累加）

// NVS 落盘结构
struct Store {
  uint32_t magic;            // 'CST2'
  uint32_t lastT;
  uint16_t count;
  uint16_t startMv;
  uint16_t peakMv;
  Sample   ring[RING_N];     // 旧→新
};
static const uint32_t STORE_MAGIC = 0x32545343;   // "CST2"

// 长时记录（过夜用）：每 10 分钟一条，144 条 = 24 小时
struct LongRec { uint16_t mv; uint16_t t10; uint8_t st; uint8_t rsv; };
static const int      LOG_N = 144;
struct LogStore {
  uint32_t magic;              // 'CST3'
  uint16_t count;
  uint16_t head;
  uint32_t tPlug, tTaper, tFull;
  uint8_t  flags;              // bit0 有台阶 / bit1 进过收尾 / bit2 到过满电
  uint8_t  rsv[3];
  LongRec  rec[LOG_N];
};
static const uint32_t LOG_MAGIC = 0x33545343;     // "CST3"
static LogStore logStore;

// ---------------- 全局 ----------------
static M5Canvas     canvas(&M5.Display);
static bool         canvasOk = false;
static Preferences  prefs;

static Sample   ring[RING_N];
static int      ringHead = 0, ringCount = 0;
static uint32_t baseSec = 0;

static bool     g_zh = false;              // 界面语言：默认英文
static char     lastKey = 0;               // 选语言页按键回显（诊断用）
static uint16_t curMv = 0, rawMv = 0;
static int16_t  calMv = 0;                 // NVS 校准偏移
static uint16_t sessionStartMv = 0, vbatMax = 0;
static uint32_t sessionSec = 0;
static float    slopeCache = 0.0f;
static bool     slopeOk = false, stepSeen = false, resumed = false;
static bool     invalidRead = false;
static CState   stCache = CS_SAMPLING;

static bool     screenOn = true, stayMode = false;
static uint32_t screenOffAt = 0, stayUntil = 0;
static uint32_t lastSampleMs = 0, lastRenderMs = 0, lastPersistMs = 0;
static uint32_t prevMask = 0;

// 长时记录的前置声明（doSample 里要打点）
static void logLoad();
static void logSave();
static void logAppend(uint16_t mv, uint32_t tSec, uint8_t st);

// ---------------- 环缓冲 ----------------
// ringHead = 下一个写入位置 ⇒ 最老样本在 (head - count + N) % N，不是 head
static inline Sample& ringAt(int i) {
  return ring[((ringHead - ringCount + i) % RING_N + RING_N) % RING_N];
}
static inline uint32_t nowSec() { return baseSec + millis() / 1000; }

static void ringPush(uint16_t mv, uint32_t t) {
  ring[ringHead].mv = mv;
  ring[ringHead].t  = t;
  ringHead = (ringHead + 1) % RING_N;
  if (ringCount < RING_N) ringCount++;
}

// 最近 n 个采样的斜率（mV/min）
static bool slopeOver(int n, float& out) {
  if (n > ringCount) n = ringCount;
  if (n < 2) return false;
  const Sample& a = ringAt(ringCount - n);
  const Sample& b = ringAt(ringCount - 1);
  if (b.t <= a.t) return false;
  uint32_t dt = b.t - a.t;
  if (dt < 120) return false;                   // 至少 2 分钟窗口
  out = ((float)b.mv - (float)a.mv) * 60.0f / (float)dt;
  return true;
}

static bool hasStep(int n) {
  if (n > ringCount) n = ringCount;
  for (int i = ringCount - n + 1; i < ringCount; i++) {
    if (i <= 0) continue;
    const Sample& a = ringAt(i - 1);
    const Sample& b = ringAt(i);
    if ((int)b.mv - (int)a.mv >= TH_STEP_MV) return true;
  }
  return false;
}

// 最近一次"插电台阶"之后的位置；没有台阶返回 -1
static int lastStepIdx() {
  for (int i = ringCount - 1; i >= 1; i--) {
    if ((int)ringAt(i).mv - (int)ringAt(i - 1).mv >= TH_STEP_MV) return i;
  }
  return -1;
}

// 锚定在台阶之后的斜率（mV/min）
static bool slopeAnchored(float& out) {
  int step = lastStepIdx();
  int from = (step >= 0) ? step : 0;
  int avail = ringCount - from;
  if (avail < 2) return false;
  int n = (avail < WIN_MAX) ? avail : WIN_MAX;
  const Sample& a = ringAt(ringCount - n);
  const Sample& b = ringAt(ringCount - 1);
  if (b.t <= a.t) return false;
  uint32_t dt = b.t - a.t;
  if (dt < 120) return false;
  out = ((float)b.mv - (float)a.mv) * 60.0f / (float)dt;
  return true;
}

// ---------------- 判定 ----------------
static int pctFromMv(int mv) {
  static const int16_t tbl[8][2] = {
    {3300,0},{3600,10},{3700,25},{3790,50},{3900,70},{4000,85},{4100,95},{4200,100}
  };
  if (mv <= tbl[0][0]) return 0;
  if (mv >= tbl[7][0]) return 100;
  for (int i = 1; i < 8; i++) {
    if (mv < tbl[i][0]) {
      int x0 = tbl[i-1][0], y0 = tbl[i-1][1], x1 = tbl[i][0], y1 = tbl[i][1];
      return y0 + (mv - x0) * (y1 - y0) / (x1 - x0);
    }
  }
  return 100;
}

static CState classify() {
  slopeOk = slopeAnchored(slopeCache);
  if (!slopeOk) {
    slopeOk = slopeOver(WIN_MAX, slopeCache);
    if (!slopeOk) slopeOk = slopeOver(ringCount, slopeCache);
  }
  stepSeen = hasStep(RING_N);

  if (ringCount < 3) return CS_SAMPLING;
  if (curMv >= VBAT_FULL && (!slopeOk || slopeCache < TH_TAPER)) return CS_FULL;
  if (!slopeOk) return CS_SAMPLING;

  if (slopeCache >= TH_FAST)  return CS_FAST;
  if (slopeCache >= TH_TAPER) return CS_TAPER;
  if (slopeCache <= TH_DROP)  return CS_NOUSB;
  if (stepSeen)               return CS_WEAK;
  return CS_SAMPLING;
}

static uint16_t stateColor(CState s) {
  switch (s) {
    case CS_FAST:  return C_AMBER;
    case CS_TAPER: return C_AMBER;
    case CS_FULL:  return C_GREEN;
    case CS_WEAK:  return C_RED;
    case CS_NOUSB: return C_GRAY;
    case CS_NOBATT: return C_RED;
    default:       return C_DIM;
  }
}

static const char* stateZh(CState s) {
  switch (s) {
    case CS_FAST:  return "正在充";
    case CS_TAPER: return "慢充中";
    case CS_FULL:  return "已充满";
    case CS_WEAK:  return "供不上";
    case CS_NOUSB: return "没在充";
    case CS_NOBATT: return "无电池";
    default:       return "采集中";
  }
}

static const char* stateEn(CState s) {
  switch (s) {
    case CS_FAST:  return "CHARGING";
    case CS_TAPER: return "TRICKLE";
    case CS_FULL:  return "FULL";
    case CS_WEAK:  return "WEAK";
    case CS_NOUSB: return "NO CHARGE";
    case CS_NOBATT: return "NO BATT";
    default:       return "SAMPLING";
  }
}

// ---------------- 采样 ----------------
// 读 8 次：既求平均，也看极差。开关 OFF 时电池被断开、分压点浮空 → 读数不作数
static uint16_t readVbatAvg() {
  uint32_t sum = 0; int n = 0;
  int lo = 9999, hi = 0;
  for (int i = 0; i < 8; i++) {
    int v = M5.Power.getBatteryVoltage();
    if (v > 1500 && v < 5000) {
      sum += (uint32_t)v; n++;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    delay(5);
  }
  if (n < 6) { invalidRead = true; return 0; }
  int mean = (int)(sum / n);
  int spread = hi - lo;
  if (mean < 3000 || mean > 4300 || spread > 60) { invalidRead = true; return 0; }
  invalidRead = false;
  rawMv = (uint16_t)mean;
  int v = mean + (int)calMv;
  if (v < 0) v = 0;
  return (uint16_t)v;
}

static void doSample(uint32_t now) {
  uint16_t mv = readVbatAvg();
  if (!mv) { stCache = CS_NOBATT; lastSampleMs = now; return; }
  curMv = mv;
  if (!sessionStartMv) sessionStartMv = mv;
  if (mv > vbatMax) vbatMax = mv;
  ringPush(mv, nowSec());
  if (ringCount >= 2) sessionSec = ringAt(ringCount - 1).t - ringAt(0).t;
  stCache = classify();

  // 关键事件打点：插电台阶 / 进收尾 / 到满电（各只记第一次）
  uint32_t tNow = ringAt(ringCount - 1).t;
  bool dirty = false;
  if (stepSeen && !(logStore.flags & 0x01)) { logStore.flags |= 0x01; logStore.tPlug  = tNow; dirty = true; }
  if (stCache == CS_TAPER && !(logStore.flags & 0x02)) { logStore.flags |= 0x02; logStore.tTaper = tNow; dirty = true; }
  if (stCache == CS_FULL  && !(logStore.flags & 0x04)) { logStore.flags |= 0x04; logStore.tFull  = tNow; dirty = true; }
  if (dirty) logSave();

  lastSampleMs = now;
}

// ---------------- NVS 持久化 ----------------
static void persistAll() {
  Store s;
  s.magic   = STORE_MAGIC;
  s.lastT   = ringCount ? ringAt(ringCount - 1).t : 0;
  s.count   = (uint16_t)ringCount;
  s.startMv = sessionStartMv;
  s.peakMv  = vbatMax;
  memset(s.ring, 0, sizeof(s.ring));
  for (int i = 0; i < ringCount; i++) s.ring[i] = ringAt(i);
  prefs.putBytes("store", &s, sizeof(s));
}

static bool restoreAll() {
  if (prefs.getBytesLength("store") != (size_t)sizeof(Store)) return false;
  Store s;
  prefs.getBytes("store", &s, sizeof(s));
  if (s.magic != STORE_MAGIC || s.count < 1 || s.count > RING_N) return false;

  uint16_t lastMv = s.ring[s.count - 1].mv;
  uint16_t nowMv  = readVbatAvg();
  if (nowMv) {
    int diff = (int)nowMv - (int)lastMv;
    if (diff > 150 || diff < -150) return false;   // 中间拔过电/放了一夜
  }

  for (int i = 0; i < s.count; i++) ring[i] = s.ring[i];
  ringCount      = s.count;
  ringHead       = s.count % RING_N;
  baseSec        = s.lastT;
  sessionStartMv = s.startMv;
  vbatMax        = s.peakMv;
  return true;
}

// ---------------- 长时记录（过夜） ----------------
static void logLoad() {
  if (prefs.getBytesLength("log") == (size_t)sizeof(LogStore)) {
    prefs.getBytes("log", &logStore, sizeof(LogStore));
  }
  if (logStore.magic != LOG_MAGIC || logStore.count > LOG_N) {
    memset(&logStore, 0, sizeof(logStore));
    logStore.magic = LOG_MAGIC;
  }
}

static void logSave() { prefs.putBytes("log", &logStore, sizeof(LogStore)); }

static void logAppend(uint16_t mv, uint32_t tSec, uint8_t st) {
  LongRec& r = logStore.rec[logStore.head];
  r.mv  = mv;
  r.t10 = (uint16_t)(tSec / 600);      // 10 分钟一格
  r.st  = st;
  r.rsv = 0;
  logStore.head = (uint16_t)((logStore.head + 1) % LOG_N);
  if (logStore.count < LOG_N) logStore.count++;
}

// ---------------- 文案 ----------------
static inline lgfx::LGFXBase& target() {
  return canvasOk ? (lgfx::LGFXBase&)canvas : (lgfx::LGFXBase&)M5.Display;
}

static void drawZh(lgfx::LGFXBase& g, int x, int y, const char* s, uint16_t col) {
  g.setFont(&fonts::efontCN_12);
  g.setTextSize(1);
  g.setTextColor(col);
  g.setCursor(x, y - 2);        // 汉字墨水比 Font0 低 2px
  g.print(s);
  g.setFont(&fonts::Font0);
}

// 按当前语言画一行标签：中文走 efont(12px)，英文走 Font0 size1(6x8)
static void drawLabel(lgfx::LGFXBase& g, int x, int y, const char* zh, const char* en, uint16_t col) {
  if (g_zh) {
    drawZh(g, x, y, zh, col);
  } else {
    g.setFont(&fonts::Font0);
    g.setTextSize(1);
    g.setTextColor(col);
    g.setCursor(x, y);
    g.print(en);
  }
}

// ---------------- 绘制 ----------------
static void fmtDur(char* b, size_t n, uint32_t sec) {
  uint32_t h = sec / 3600, m = (sec % 3600) / 60;
  if (h) snprintf(b, n, "%uh%02um", (unsigned)h, (unsigned)m);
  else   snprintf(b, n, "%um", (unsigned)m);
}

// 电压曲线（整个环，最多 40 分钟）
static void drawSpark(lgfx::LGFXBase& g, int x0, int y0, int w, int h) {
  g.fillRect(x0, y0, w, h, C_BOX);
  if (ringCount < 2 || h < 12) return;

  int vmin = ringAt(0).mv, vmax = vmin;
  for (int i = 0; i < ringCount; i++) {
    int v = ringAt(i).mv;
    if (v < vmin) vmin = v;
    if (v > vmax) vmax = v;
  }
  int span = vmax - vmin;
  if (span < 40) {
    int mid = (vmax + vmin) / 2;
    vmin = mid - 20; vmax = mid + 20; span = 40;
  }

  g.drawFastHLine(x0 + 1, y0 + h / 2, w - 2, 0x2124);

  int px = -1, py = -1;
  for (int i = 0; i < ringCount; i++) {
    int x = x0 + 1 + (int)((long)(w - 2) * i / (ringCount - 1));
    int y = y0 + h - 2 - (int)((long)(h - 4) * (ringAt(i).mv - vmin) / span);
    if (py >= 0) g.drawLine(px, py, x, y, C_AMBER);
    px = x; py = y;
  }
  if (px >= 0) g.fillRect(px - 1, py - 1, 3, 3, C_WHITE);

  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextColor(C_GRAY);
  char b[20];
  snprintf(b, sizeof(b), "%.2f", (double)vmax / 1000.0);
  g.setCursor(x0 + 2, y0 + 1); g.print(b);
  snprintf(b, sizeof(b), "%.2f", (double)vmin / 1000.0);
  g.setCursor(x0 + 2, y0 + h - 9); g.print(b);
}

static void drawUI(lgfx::LGFXBase& g) {
  g.fillRect(0, 0, 240, 135, TFT_BLACK);
  g.setFont(&fonts::Font0);
  g.setTextSize(1);

  // 顶部
  g.setTextColor(C_DIM);
  g.setCursor(4, 3); g.print("CHARGE STATION");
  char buf[20];
  if (stayMode) {
    uint32_t now = millis();
    uint32_t remain = (stayUntil > now) ? (stayUntil - now) / 1000 : 0;
    snprintf(buf, sizeof(buf), "STAY %u:%02u", (unsigned)(remain / 60), (unsigned)(remain % 60));
    g.setTextColor(C_AMBER);
  } else {
    fmtDur(buf, sizeof(buf), sessionSec);
    g.setTextColor(C_GRAY);
  }
  int w = (int)strlen(buf) * 6;
  g.setCursor(236 - w, 3); g.print(buf);
  g.drawFastHLine(0, 14, 240, 0x2965);

  // 大电压
  uint16_t col = stateColor(stCache);
  g.setTextColor(col);
  g.setTextSize(3);
  g.setCursor(6, 18);
  if (stCache == CS_NOBATT) g.print("--.--V");
  else g.printf("%u.%02uV", (unsigned)(curMv / 1000), (unsigned)((curMv % 1000) / 10));
  g.setTextSize(2);
  g.setTextColor(C_WHITE);
  g.setCursor(6, 44);
  if (stCache == CS_NOBATT) g.print("--%");
  else                      g.printf("~%d%%", pctFromMv(curMv));

  // 右列状态
  if (g_zh) {
    drawZh(g, 130, 20, stateZh(stCache), col);
    g.setFont(&fonts::Font0);
    g.setTextSize(1);
    g.setTextColor(col);
    g.setCursor(130, 36); g.print(stateEn(stCache));
  } else {
    g.setFont(&fonts::Font0);
    g.setTextSize(2);
    g.setTextColor(col);
    g.setCursor(130, 22); g.print(stateEn(stCache));
  }
  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextColor(C_GRAY);
  g.setCursor(130, 48);
  if (slopeOk) g.printf("%+.2fmV/m", slopeCache);
  else         g.print("-- mV/min");
  // 净充电电流估算（用本机电压表反推，只给量级；不是库仑计）
  if (slopeOk && slopeCache > TH_TAPER) {
    int p1 = pctFromMv(curMv);
    int p0 = pctFromMv(curMv > 50 ? curMv - 50 : 0);
    int dp = p1 - p0;
    if (dp > 0) {
      int ma = (int)(slopeCache * dp * 21.0f + 0.5f);
      if (ma > 999) ma = 999;
      g.setTextColor(C_AMBER);
      g.setCursor(130, 58);
      g.printf("~%dmA", ma);
    }
  }

  // 说明行
  g.drawFastHLine(0, 68, 240, 0x2965);
  int etaMin = -1;
  if ((stCache == CS_FAST || stCache == CS_TAPER) && slopeOk && slopeCache > TH_TAPER) {
    etaMin = (int)((float)(VBAT_FULL - (int)curMv) / slopeCache);
    if (etaMin < 0) etaMin = 0;
    if (etaMin > 600) etaMin = 600;
  }
  bool waiting = (stCache == CS_SAMPLING && etaMin < 0);
  if (stayMode) {
    snprintf(buf, sizeof(buf), "raw %u mV  cal %+d mV", (unsigned)rawMv, (int)calMv);
    g.setTextColor(C_GRAY);
    g.setCursor(4, 80); g.print(buf);
  } else if (stCache == CS_FULL) {
    drawLabel(g, 4, 80, "已充满，可以拔线", "FULL - safe to unplug", C_GREEN);
  } else if (etaMin >= 0) {
    char d[16]; fmtDur(d, sizeof(d), (uint32_t)etaMin * 60);
    if (g_zh) {
      drawZh(g, 4, 80, "满电还需", C_WHITE);
      g.setFont(&fonts::Font0);
      g.setTextSize(1);
      g.setTextColor(C_WHITE);
      g.setCursor(56, 80); g.printf("~%s", d);
    } else {
      g.setTextColor(C_WHITE);
      g.setCursor(4, 80); g.printf("FULL IN ~%s", d);
    }
  } else if (stCache == CS_NOUSB) {
    drawLabel(g, 4, 80, "没在充：查开关/线/电源口", "No charge: switch/cable/port", C_GRAY);
  } else if (stCache == CS_WEAK) {
    drawLabel(g, 4, 80, "接着但电压不涨：口供不上", "Plugged, no rise: weak source", C_RED);
  } else if (stCache == CS_NOBATT) {
    drawLabel(g, 4, 80, "开关 OFF / 电池未接，读数不作数", "Switch OFF or no battery", C_RED);
  } else {
    if (ringCount < WIN_MIN) snprintf(buf, sizeof(buf), "wait %d/%d pts", ringCount, WIN_MIN);
    else                     snprintf(buf, sizeof(buf), "%d pts, slope noisy", ringCount);
    g.setTextColor(C_DIM);
    g.setCursor(4, 80); g.print(buf);
  }

  int sy = waiting ? 96 : 94;
  drawSpark(g, 4, sy, 232, 135 - sy - 2);
}

static void render() {
  if (canvasOk) { drawUI(canvas); canvas.pushSprite(0, 0); }
  else          { drawUI(M5.Display); }
}

static void drawBootHint() {
  lgfx::LGFXBase& g = target();
  g.fillRect(0, 0, 240, 135, TFT_BLACK);
  g.setFont(&fonts::Font0);
  g.setTextSize(2);
  g.setTextColor(C_AMBER);
  g.setCursor(8, 14); g.print("CHARGE STATION");
  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  drawLabel(g, 8, 52, "插电后自动灭屏，电流全给电池", "Auto-dim: all current to battery", C_WHITE);
  drawLabel(g, 8, 72, "按任意键看数据（3 秒）",      "Any key: 3s readout", C_WHITE);
  drawLabel(g, 8, 92, "R常亮 +/-校准 L语言 9清记录",
                        "R stay  +/- cal  L lang  9 clear", C_GRAY);
  g.setTextColor(C_DIM);
  g.setCursor(8, 118);
  if (logStore.count) {
    uint32_t t0 = ringCount ? ringAt(0).t : (logStore.tPlug ? logStore.tPlug : 0);
    g.printf("log %dpts", logStore.count);
    if ((logStore.flags & 0x01) && logStore.tPlug  > t0) g.printf(" P%um", (unsigned)((logStore.tPlug  - t0) / 60));
    if ((logStore.flags & 0x02) && logStore.tTaper > t0) g.printf(" T%um", (unsigned)((logStore.tTaper - t0) / 60));
    if ((logStore.flags & 0x04) && logStore.tFull  > t0) g.printf(" F%um", (unsigned)((logStore.tFull  - t0) / 60));
  } else if (resumed) {
    g.printf("resume %d pts", ringCount);
  } else {
    g.print("v1.0.11  fresh session");
  }
  if (canvasOk) canvas.pushSprite(0, 0);
}

// ---------------- 开机选语言 ----------------
static int takeKeyEdge();     // 定义在下面（键盘节），选语言页要复用同一套取键逻辑

static void drawLangSelect(uint32_t remainSec) {
  lgfx::LGFXBase& g = target();
  g.fillRect(0, 0, 240, 135, TFT_BLACK);
  g.setFont(&fonts::Font0);
  g.setTextSize(2);
  g.setTextColor(C_AMBER);
  g.setCursor(8, 12); g.print("CHARGE STATION");

  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextColor(C_WHITE);
  g.setCursor(10, 48);  g.print("[1]");
  drawZh(g, 44, 50, "中文", C_WHITE);
  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextColor(C_WHITE);
  g.setCursor(10, 68);  g.print("[2]");     // 之前漏了这一行，只画了 English
  g.setTextSize(2);
  g.setCursor(44, 66);  g.print("English");

  g.setTextSize(1);
  g.setTextColor(C_DIM);
  g.setCursor(10, 104);
  g.printf("default: English   auto in %us", (unsigned)remainSec);
  if (lastKey) {
    g.setCursor(10, 118);
    g.printf("key seen: %c", lastKey);       // 诊断：按键真的进来了吗
  } else {
    g.setCursor(10, 118);
    g.print("press 1 = 中文   2 = English");
  }
  if (canvasOk) canvas.pushSprite(0, 0);
}

static void selectLanguage() {
  M5.Display.setBrightness(BRIGHT_UI);
  uint32_t start = millis();
  uint32_t lastDraw = 0;
  int choice = 0;
  prevMask = 0;
  lastKey = 0;
  while (millis() - start < LANG_WAIT_MS) {
    // 用和运行时完全相同的取键函数（已证明 L 键可用），不要另写一套
    int got = takeKeyEdge();
    if (got) {
      lastKey = (char)got;                    // 屏幕上回显，便于判断按键是否送到
      prefs.putChar("klast", (int8_t)got);     // 也落盘，方便回读诊断
      char k = (char)got;
      if (k >= 'A' && k <= 'Z') k += 32;
      if (k == '1' || k == 'c')      choice = 1;
      else if (k == '2' || k == 'e') choice = 2;
      else if (k == '\r' || k == '\n' || k == ' ') choice = 2;   // Enter/空格 = 默认英文
      if (choice) break;
    }
    uint32_t el = (millis() - start) / 1000;
    if (millis() - lastDraw >= 300) { lastDraw = millis(); drawLangSelect((LANG_WAIT_MS / 1000) - el); }
    delay(20);
  }
  g_zh = (choice == 1);
  prefs.putChar("lang", g_zh ? 1 : 0);

  lgfx::LGFXBase& g = target();
  g.fillRect(0, 0, 240, 135, TFT_BLACK);
  g.setFont(&fonts::Font0);
  g.setTextSize(2);
  g.setTextColor(C_AMBER);
  g.setCursor(8, 50);
  g.print(g_zh ? "Language: Chinese" : "Language: English");
  if (canvasOk) canvas.pushSprite(0, 0);
  delay(1000);
}

// ---------------- 屏幕 ----------------
static void displayWake() {
  M5.Display.wakeup();
  M5.Display.setBrightness(BRIGHT_UI);
  screenOn = true;
}

static void displaySleepNow() {
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  screenOn = false;
}

static void wakeScreen(uint32_t holdMs) {
  if (!screenOn) displayWake();
  screenOffAt = millis() + holdMs;
  render();
  lastRenderMs = millis();
}

// ---------------- 键盘 ----------------
static int takeKeyEdge() {
  M5Cardputer.update();
  auto ks = M5Cardputer.Keyboard.keysState();
  if (ks.word.empty()) { prevMask = 0; return 0; }

  uint32_t mask = 0;
  for (char c : ks.word) {
    char k = c; if (k >= 'A' && k <= 'Z') k += 32;
    if (k >= 'a' && k <= 'z')      mask |= 1UL << (k - 'a');
    else if (k >= '0' && k <= '9') mask |= 1UL << (26 + (k - '0'));
  }
  uint32_t edge = mask & ~prevMask;
  prevMask = mask;

  for (char c : ks.word) {
    char k = c; if (k >= 'A' && k <= 'Z') k += 32;
    uint32_t bit = 0;
    if (k >= 'a' && k <= 'z')      bit = 1UL << (k - 'a');
    else if (k >= '0' && k <= '9') bit = 1UL << (26 + (k - '0'));
    if (bit && (edge & bit)) return c;
  }
  return ks.word[0];
}

// ---------------- setup / loop ----------------
void setup() {
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5Cardputer;
  cfg.clear_display  = true;
  cfg.internal_mic   = false;
  cfg.internal_spk   = false;
  cfg.output_power   = false;
  M5Cardputer.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(BRIGHT_UI);
  canvasOk = canvas.createSprite(240, 135);

  pinMode(KEY_INT_PIN, INPUT_PULLUP);

  prefs.begin("chargestation", false);
  int8_t lang = prefs.getChar("lang", -1);
  if (lang < 0) {
    selectLanguage();                       // 首次开机：选语言，60s 超时默认英文
  } else {
    g_zh = (lang == 1);
  }

  calMv   = prefs.getShort("cal", 0);
  resumed = restoreAll();
  logLoad();

  lastSampleMs = millis();
  doSample(millis());
  persistAll();
  lastPersistMs = millis();

  drawBootHint();
  delay(5000);
  displaySleepNow();

  setCpuFrequencyMhz(80);
}

void loop() {
  uint32_t now = millis();

  static uint32_t lastKbPoll = 0;
  bool poll = (digitalRead(KEY_INT_PIN) == LOW);
  if (!poll && now - lastKbPoll >= 500) { poll = true; lastKbPoll = now; }
  if (poll) {
    int k = takeKeyEdge();
    if (k) {
      char lk = (k >= 'A' && k <= 'Z') ? (char)(k + 32) : (char)k;
      if (lk == 'r') {
        stayMode = !stayMode;
        if (stayMode) { stayUntil = now + STAY_MS; wakeScreen(STAY_MS); }
        else          { wakeScreen(SCREEN_HOLD); }
      } else if (lk == '9') {
        memset(&logStore, 0, sizeof(logStore));   // 清空过夜长记录
        logStore.magic = LOG_MAGIC;
        logSave();
        stayMode = false;
        wakeScreen(SCREEN_HOLD);
      } else if (lk == '0') {
        prefs.remove("lang");               // 清掉语言选择 → 重启回到选语言页
        ESP.restart();
      } else if (lk == 'l' || lk == '1' || lk == '2') {
        // L 切换；1/2 直接指定（用户直觉就是按 1 中文、2 英文）
        if (lk == 'l')      g_zh = !g_zh;
        else if (lk == '1') g_zh = true;
        else                g_zh = false;
        prefs.putChar("lang", g_zh ? 1 : 0);
        stayMode = false;
        wakeScreen(SCREEN_HOLD);
      } else if (lk == '+' || lk == '=') {
        calMv = (int16_t)min((int)calMv + 10, 200);
        prefs.putShort("cal", calMv);
        stayMode = false;
        wakeScreen(SCREEN_HOLD);
      } else if (lk == '-' || lk == '_') {
        calMv = (int16_t)max((int)calMv - 10, -200);
        prefs.putShort("cal", calMv);
        stayMode = false;
        wakeScreen(SCREEN_HOLD);
      } else {
        stayMode = false;
        wakeScreen(SCREEN_HOLD);
      }
    }
  }

  if (now - lastSampleMs >= SAMPLE_MS) doSample(now);

  if (now - lastPersistMs >= PERSIST_MS) { persistAll(); lastPersistMs = now; }

  static uint32_t lastLogMs = 0;
  if (lastLogMs == 0) lastLogMs = now;
  if (now - lastLogMs >= 600000UL) {
    logAppend(curMv, ringCount ? ringAt(ringCount - 1).t : nowSec(), (uint8_t)stCache);
    logSave();
    lastLogMs = now;
  }

  if (stayMode && (int32_t)(now - stayUntil) >= 0) {
    stayMode = false;
    displaySleepNow();
  }
  if (stayMode) {
    if (!screenOn) displayWake();
    if (now - lastRenderMs >= 1000) { lastRenderMs = now; render(); }
  } else if (screenOn && (int32_t)(now - screenOffAt) >= 0) {
    displaySleepNow();
  } else if (screenOn && now - lastRenderMs >= 1000) {
    lastRenderMs = now; render();
  }

  delay(50);
}
