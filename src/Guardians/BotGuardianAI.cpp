/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BotGuardianAI.h"
#include "BotGuardianDisplays.h"
#include "Random.h"

void BotGuardianAI::ApplyRandomGuardianDisplay(GuardianVisualType type)
{
    if (!me)
        return;

    std::vector<GuardianDisplayEntry> const* pool = nullptr;

    switch (type)
    {
        case GUARDIAN_VISUAL_HUNTER_BEAST:     pool = &HunterBeastDisplays; break;
        case GUARDIAN_VISUAL_DK_UNDEAD:        pool = &DkUndeadDisplays; break;
        case GUARDIAN_VISUAL_WARLOCK_IMP:      pool = &WarlockImpDisplays; break;
        case GUARDIAN_VISUAL_WARLOCK_FELHOUND: pool = &WarlockFelhoundDisplays; break;
        case GUARDIAN_VISUAL_WARLOCK_FELGUARD: pool = &WarlockFelguardDisplays; break;
        default: return;
    }

    if (!pool || pool->empty())
        return;

    uint32 const index = urand(0, static_cast<uint32>(pool->size()) - 1);
    GuardianDisplayEntry const& entry = (*pool)[index];

    // 双重接口设值：SetDisplayId 负责即时刷新表现层，SetNativeDisplayId 负责在
    // 受控、变形、状态刷新结束后不回退为 creature_template 的原生默认模型。
    me->SetDisplayId(entry.displayId);
    me->SetNativeDisplayId(entry.displayId);

    // 体型归一化必须放在最后：仅改动表现层 ObjectScale，
    // 严禁触碰服务端 combat_reach 与 bounding_radius。
    me->SetObjectScale(entry.scale);
}
