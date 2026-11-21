// b_ui_trace.h — BTRACE1 固定 ring 非阻塞观测跟踪（schema=1）
// 纯观测接缝：不改任何 GPU 调用次数/顺序/参数；不新建线程、不加阻塞等待、不碰 UI。
// 生产线程（worker / render / present）只做 TryAcquireSRWLockExclusive 抢占式记录，
// 抢占失败即丢并计数，绝不等待也不刷盘。
// 唯一刷盘点 = 现有 HotkeyThread 每次循环的有界批量 flush。
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifndef BTRACE1_RING_SLOTS
#define BTRACE1_RING_SLOTS 8192
#endif
#ifndef BTRACE1_REC_MAX
#define BTRACE1_REC_MAX 768
#endif
#ifndef BTRACE1_FLUSH_BATCH
#define BTRACE1_FLUSH_BATCH 256
#endif

struct BtRing {
    SRWLOCK       lock;                                  // 零初始化 = SRWLOCK_INIT（已解锁态）
    char          slot[BTRACE1_RING_SLOTS][BTRACE1_REC_MAX];
    volatile LONG write;                                 // 下一个可写槽（对 SLOTS 取模）
    volatile LONG read;                                  // 下一个待刷槽
    volatile LONG seq;                                   // 成功记录计数（CPU 顺序；drop 也占用形成缺口）
    volatile LONG loss;                                  // 锁抢占失败丢弃
    volatile LONG drop;                                  // ring 满丢弃
    volatile LONG trunc;                                 // 消息超长截断
    volatile LONG flush;                                 // 已刷出计数
};

static BtRing g_btRing;                                  // 单实例（本 .cpp 只 include 一次）
static void (*g_btEmit)(const char* line) = nullptr;     // 刷盘回调：宿主注册到 Log()

static inline void BtSetEmit(void (*emit)(const char* line)) { g_btEmit = emit; }

static inline unsigned long long BtPtr(const void* p) {
    return (unsigned long long)(UINT_PTR)p;
}

// 非阻塞记录：可变字段格式化在锁外做，seq 在锁内分配。
static inline void BtTrace(const char* event, int frame, const char* phase, const char* extraFmt, ...) {
    char body[BTRACE1_REC_MAX]; body[0] = '\0';
    if (extraFmt && extraFmt[0]) {
        va_list ap; va_start(ap, extraFmt);
        if (_vsnprintf_s(body, sizeof(body), _TRUNCATE, extraFmt, ap) < 0)
            InterlockedIncrement(&g_btRing.trunc);
        va_end(ap);
    }
    const ULONGLONG tick = GetTickCount64();
    const DWORD tid = GetCurrentThreadId();
    if (!TryAcquireSRWLockExclusive(&g_btRing.lock)) {
        InterlockedIncrement(&g_btRing.loss);
        return;
    }
    const long s = InterlockedIncrement(&g_btRing.seq);
    if (g_btRing.write - g_btRing.read >= BTRACE1_RING_SLOTS) {
        InterlockedIncrement(&g_btRing.drop);
        ReleaseSRWLockExclusive(&g_btRing.lock);
        return;
    }
    char rec[BTRACE1_REC_MAX];
    if (_snprintf_s(rec, sizeof(rec), _TRUNCATE,
            "BTRACE1 seq=%ld tick=%llu tid=%lu event=%s frame=%d phase=%s%s%s",
            s, (unsigned long long)tick, (unsigned long)tid, event, frame, phase,
            (body[0] ? " " : ""), body) < 0)
        InterlockedIncrement(&g_btRing.trunc);
    memcpy(g_btRing.slot[g_btRing.write % BTRACE1_RING_SLOTS], rec, BTRACE1_REC_MAX);
    InterlockedIncrement(&g_btRing.write);
    ReleaseSRWLockExclusive(&g_btRing.lock);
}

// 丢失/截断的 LOSS / STATS 事件：meta=1 且【无 seq】，不占用/伪造真实事件 seq。
// 统计键统一 lost / dropped / truncated（分析器识别这三键）。
static inline void BtEmitStats() {
    if (!g_btEmit) return;
    const long lost = g_btRing.loss, dropped = g_btRing.drop, truncated = g_btRing.trunc;
    const long pending = g_btRing.write - g_btRing.read;
    static long lastLost = 0, lastDropped = 0, lastTruncated = 0;
    const bool changed = (lost != lastLost) || (dropped != lastDropped) || (truncated != lastTruncated);
    static ULONGLONG lastT = 0;
    const ULONGLONG nowT = GetTickCount64();
    const bool periodic = (nowT - lastT) >= 5000;   // 有丢失立即打，否则每 5 秒最多一次
    if (!changed && !periodic) return;
    lastT = nowT; lastLost = lost; lastDropped = dropped; lastTruncated = truncated;
    char line[BTRACE1_REC_MAX];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "BTRACE1 meta=1 tick=%llu tid=%lu event=STATS frame=0 phase=flush pending=%ld lost=%ld dropped=%ld truncated=%ld flushed=%ld",
        (unsigned long long)nowT, (unsigned long)GetCurrentThreadId(),
        pending, lost, dropped, truncated, g_btRing.flush);
    g_btEmit(line);
    if (lost || dropped || truncated) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
            "BTRACE1 meta=1 tick=%llu tid=%lu event=LOSS frame=0 phase=flush lost=%ld dropped=%ld truncated=%ld",
            (unsigned long long)GetTickCount64(), (unsigned long)GetCurrentThreadId(),
            lost, dropped, truncated);
        g_btEmit(line);
    }
}

// 有界批量 flush：每条记录单独 lock/unlock（锁内只 memcpy，不碰 I/O）。
static inline void BtFlush(int batch) {
    if (!g_btEmit) return;
    for (int i = 0; i < batch; ++i) {
        char rec[BTRACE1_REC_MAX];
        bool have = false;
        if (TryAcquireSRWLockExclusive(&g_btRing.lock)) {
            if (g_btRing.read < g_btRing.write) {
                memcpy(rec, g_btRing.slot[g_btRing.read % BTRACE1_RING_SLOTS], BTRACE1_REC_MAX);
                InterlockedIncrement(&g_btRing.read);
                InterlockedIncrement(&g_btRing.flush);
                have = true;
            }
            ReleaseSRWLockExclusive(&g_btRing.lock);
        } else {
            return;   // 抢占失败：下次循环再刷
        }
        if (!have) break;
        g_btEmit(rec);
    }
    BtEmitStats();
}

// SESSION：启动时立即落盘（不走 ring，避免被启动前竞争吞掉）。
static inline void BtSession(const char* buildId, int schema, const char* baselineSha, const char* tag) {
    if (!g_btEmit) return;
    char rec[BTRACE1_REC_MAX];
    _snprintf_s(rec, sizeof(rec), _TRUNCATE,
        "BTRACE1 seq=%ld tick=%llu tid=%lu event=SESSION frame=0 phase=init build_id=%s schema=%d baseline_sha=%s tag=%s",
        (long)InterlockedIncrement(&g_btRing.seq), (unsigned long long)GetTickCount64(),
        (unsigned long)GetCurrentThreadId(), buildId, schema, baselineSha, tag);
    g_btEmit(rec);
}
