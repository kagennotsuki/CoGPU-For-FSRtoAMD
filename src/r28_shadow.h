// r28 块1（2026-11-16）：影子选槽纯函数。核心 ffx_core_reuse.cpp 与主机单测
// rebuild\r28-b1-20261116\_r28_test.cpp 共用这一份实现，避免两份拷贝漂移。
// 只做计算，不读写任何全局量，不改运行行为。
#pragma once

// 影子选择器：票据环"会选哪个槽"的上界估计（没有 LEASED/DROP 语义，见拆分文档 §7 第 6 条）。
// 条件：valid[i] 且 tags[i] > lastCons；在其中取最大 tag。返回槽号 0..5，没有新票返回 -1（NO_NEW）。
// 同值时取下标小的（正常情况下 slot = T%6，同一 T 不会出现在两个槽）。
inline int R28Pick(const int tags[6], const bool valid[6], int lastCons) {
    int best = -1, bestT = lastCons;
    for (int i = 0; i < 6; ++i) {
        if (!valid[i]) continue;
        if (tags[i] > bestT) { bestT = tags[i]; best = i; }
    }
    return best;
}

// EpochFirstT 规则（D1=A：epoch 只在 CPU 侧判定）：T < EpochFirstT 的捕获一律丢弃。
// Expand 失败返回的 -1 也落在这里被丢弃（EpochFirstT >= 0）。块1 只供单测使用，块2 接入。
inline bool R28EpochAccept(int T, int epochFirstT) {
    return T >= 0 && T >= epochFirstT;
}
