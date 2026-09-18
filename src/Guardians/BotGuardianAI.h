/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "ScriptedCreature.h"
#include "Creature.h"
#include "Unit.h"
#include "BotGuardianDisplays.h"

/**
 * @brief 伴随型战斗护卫（Combat Guardian）轻量级 AI 基类。
 *
 * 设计原则（见 CONTEXT.md 第 6.3 节）：
 *  - 彻底剥离原生 Player Pet 复杂数据结构，作为伴随型守护 Creature 挂载；
 *  - 不独立索敌开怪，转火严格同步主人，治疗打地鼠雷达绝对排除；
 *  - 外观多样化与体型归一化仅通过 DisplayId / NativeDisplayId / ObjectScale
 *    三条表现层接口实现，严禁改动服务端 combat_reach 与 bounding_radius，
 *    保证底层攻击距离判定与找背几何恒定不变。
 */
class BotGuardianAI : public ScriptedAI
{
public:
    explicit BotGuardianAI(Creature* creature, GuardianVisualType visualType = GUARDIAN_VISUAL_HUNTER_BEAST)
        : ScriptedAI(creature), _visualType(visualType)
    {
    }

    ~BotGuardianAI() override = default;

    // =========================================================================
    // 外观多样化与体型归一化
    // =========================================================================
    /// @brief 从对应类型的已验证外观池中随机抽取一条并应用。
    void ApplyRandomGuardianDisplay(GuardianVisualType type);

    /// @brief 切换外观池类型，可选择是否立即重新随机一次。
    void SetGuardianVisualType(GuardianVisualType type, bool applyImmediately = false)
    {
        _visualType = type;
        if (applyImmediately)
            ApplyRandomGuardianDisplay(_visualType);
    }

    [[nodiscard]] GuardianVisualType GetGuardianVisualType() const { return _visualType; }

    /// @brief 以当前外观池强制再随机一次（例如重新召唤或阶段刷新时调用）。
    void ReapplyGuardianDisplay() { ApplyRandomGuardianDisplay(_visualType); }

    // =========================================================================
    // 随从生成初始化阶段
    // =========================================================================
    void Reset() override
    {
        ScriptedAI::Reset();

        // 生成/重置阶段统一应用随机外观，受控、变形或状态刷新后仍会被重新归一化
        ApplyRandomGuardianDisplay(_visualType);

        OnGuardianReset();
    }

    /// @brief 具体护卫专精（座狼 / 食尸鬼 / 地狱猎犬 ...）的初始化钩子。
    virtual void OnGuardianReset() {}

protected:
    GuardianVisualType _visualType;
};
