/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include <vector>
#include "Define.h"

// 伴随型战斗护卫统一 Creature Entry。
// 数据库 creature_template 中的 990001 应定义为友好中立的载体 NPC（不主动索敌、
// 不参与任务交互），全部外观与机制内核均由 BotGuardianAI 在运行时动态注入，
// 模板仅提供最基础的骨架，杜绝为其单独维护多套模板。
static constexpr uint32 NPC_BOT_GUARDIAN = 990001;

enum GuardianVisualType : uint8
{
    GUARDIAN_VISUAL_HUNTER_BEAST     = 0,
    GUARDIAN_VISUAL_DK_UNDEAD        = 1,
    GUARDIAN_VISUAL_WARLOCK_IMP      = 2,
    GUARDIAN_VISUAL_WARLOCK_FELHOUND = 3,
    GUARDIAN_VISUAL_WARLOCK_FELGUARD = 4
};

struct GuardianDisplayEntry
{
    uint32 displayId;
    float  scale;
};

// 1. 猎人陆生野兽池（机制内核：座狼模板）
static const std::vector<GuardianDisplayEntry> HunterBeastDisplays = {
    // 经典狼与恐狼
    { 165, 1.0f }, { 161, 1.0f }, { 644, 1.0f }, { 776, 1.0f }, { 4124, 1.0f },
    { 73, 1.0f }, { 720, 1.0f }, { 9369, 1.0f },
    // 诺森德座狼
    { 22003, 1.0f }, { 22042, 1.0f }, { 22089, 1.0f },
    // 猫科（虎/豹/狮）
    { 320, 1.0f }, { 321, 1.0f }, { 599, 1.0f }, { 616, 1.0f }, { 748, 1.0f }, { 9949, 1.0f },
    { 1057, 1.0f }, { 1934, 1.0f }, { 4424, 1.0f }, { 1933, 1.0f },
    // 熊类
    { 706, 1.0f }, { 806, 1.0f }, { 820, 1.0f }, { 865, 1.0f }, { 982, 1.0f }, { 1082, 1.0f },
    // 野猪类
    { 193, 1.0f }, { 377, 1.0f }, { 381, 1.0f }, { 3026, 1.0f }, { 4713, 1.0f }, { 6121, 1.0f },
    // 迅猛龙（缩放 0.85f 避免模型过大）
    { 180, 0.85f }, { 322, 0.85f }, { 675, 0.85f }, { 787, 0.85f }, { 960, 0.85f }, { 1337, 0.85f }, { 1959, 0.85f },
    // 陆龟与鳄鱼
    { 1244, 1.0f }, { 2307, 1.0f }, { 4829, 1.0f }, { 5026, 1.0f }, { 5126, 1.0f },
    { 807, 1.0f }, { 833, 1.0f }, { 1609, 1.0f },
    // 蜘蛛与蝎子
    { 283, 1.0f }, { 711, 0.85f }, { 963, 1.0f }, { 4456, 1.0f }, { 15937, 1.0f },
    { 2488, 1.0f }, { 2489, 1.0f }, { 2729, 1.0f }, { 2730, 1.0f },
    // 猩猩与陆行鸟
    { 809, 1.0f }, { 838, 1.0f }, { 840, 1.0f }, { 3186, 1.0f },
    { 38, 1.0f }, { 178, 1.0f }, { 1281, 1.0f }, { 1961, 1.0f }
};

// 2. 死亡骑士天灾军团池（机制内核：食尸鬼模板）
static const std::vector<GuardianDisplayEntry> DkUndeadDisplays = {
    // 经典食尸鬼
    { 137, 1.0f }, { 414, 1.0f }, { 519, 1.0f }, { 547, 1.0f },
    // 诺森德高精食尸鬼
    { 24992, 1.0f }, { 24993, 1.0f }, { 24994, 1.0f }, { 24995, 1.0f },
    // 天灾潜伏者
    { 24579, 1.0f }, { 24590, 1.0f }, { 25170, 1.0f }, { 25284, 1.0f },
    // 骷髅战士
    { 158, 1.0f }, { 200, 1.0f }, { 201, 1.0f }, { 9783, 1.0f }, { 9784, 1.0f }, { 9786, 1.0f },
    // 地穴恶魔
    { 3004, 1.0f }, { 6841, 1.0f }
};

// 3. 术士三大恶魔独立池
// 毁灭术：小鬼（统一锁定 0.5f 缩放）
static const std::vector<GuardianDisplayEntry> WarlockImpDisplays = {
    { 4449, 0.5f }, { 7552, 0.5f }, { 10811, 0.5f },
    { 16888, 0.5f }, { 16889, 0.5f }, { 16890, 0.5f }, { 16891, 0.5f }
};

// 痛苦术：地狱猎犬
static const std::vector<GuardianDisplayEntry> WarlockFelhoundDisplays = {
    { 850, 1.0f }, { 1913, 1.0f }, { 6172, 1.0f }, { 7949, 1.0f }, { 10950, 1.0f }
};

// 恶魔术：恶魔卫士
static const std::vector<GuardianDisplayEntry> WarlockFelguardDisplays = {
    { 5048, 1.0f }, { 5049, 1.0f }, { 21365, 1.0f }, { 18342, 1.0f }, { 19901, 1.0f }
};
