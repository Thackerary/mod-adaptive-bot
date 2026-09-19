/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "ScriptedCreature.h"
#include "Creature.h"
#include "Unit.h"
#include "PetDefines.h"
#include "BotGuardianDisplays.h"

/**
 * @brief 伴随型战斗护卫（Combat Guardian）轻量级 AI 基类。
 *
 * 设计原则（见 CONTEXT.md 第 6.3 节）：
 *  - 彻底剥离原生 Player Pet 复杂数据结构，作为伴随型守护 Creature 挂载；
 *  - 不独立索敌开怪，转火严格同步主人，治疗打地鼠雷达绝对排除；
 *  - 外观多样化与体型归一化仅通过 DisplayId / NativeDisplayId / ObjectScale
 *    三条表现层接口实现，严禁改动服务端 combat_reach 与 bounding_radius，
 *    保证底层攻击距离判定与找背几何恒定不变；
 *  - Reset() 为幂等入口：仅在首次生成时 Roll 一次模型并缓存，脱战重聚只重新
 *    应用缓存结果，杜绝频繁重置导致的外观跳变。
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
    // 雷达识别接口
    // =========================================================================
    /// @brief 伴随型战斗护卫标记。
    ///        治疗打地鼠雷达与各类友方目标选取逻辑必须据此将护卫排除在统计之外。
    [[nodiscard]] bool IsCombatGuardian() const { return true; }

    // =========================================================================
    // 外观多样化与体型归一化
    // =========================================================================
    /// @brief 从对应类型的已验证外观池中随机抽取一条并应用（仅首次生成时调用）。
    void ApplyRandomGuardianDisplay(GuardianVisualType type);

    /// @brief 切换外观池类型，可选择是否立即重新随机一次。
    ///        一经显式调用即锁定外观池，不再被主人职业自动推断覆盖。
    void SetGuardianVisualType(GuardianVisualType type, bool applyImmediately = false)
    {
        _visualType = type;
        _visualTypeExplicit = true;
        if (applyImmediately)
            ApplyRandomGuardianDisplay(_visualType);
    }

    [[nodiscard]] GuardianVisualType GetGuardianVisualType() const { return _visualType; }

    /// @brief 以当前外观池强制再随机一次（例如重新召唤或阶段刷新时调用）。
    void ReapplyGuardianDisplay() { ApplyRandomGuardianDisplay(_visualType); }

    /// @brief 幂等刷新：重新应用最近一次 Roll 到的缓存外观，绝不重新摇号。
    void RefreshGuardianDisplay();

    /// @brief 依据主人职业推断默认外观池。
    ///        解析顺序为 魅惑者 -> 拥有者 -> 创建者：随从在刚被召唤、OwnerGUID
    ///        尚未写入的窗口期内只存在 CreatorGUID，必须依赖创建者兜底才能得到
    ///        正确职业，杜绝全职业退化为猎人野兽内核。
    [[nodiscard]] GuardianVisualType ResolveVisualTypeFromMaster() const;

    /// @brief 纯粹的表现层写入：双重设值模型 + 归一化缩放。
    void ApplyGuardianDisplay(GuardianDisplayEntry const& entry);

    // =========================================================================
    // 随从生成初始化阶段
    // =========================================================================
    /// @brief 生成与脱战重聚共用的幂等初始化入口。
    void Reset() override;

    /// @brief 具体护卫专精（座狼 / 食尸鬼 / 地狱猎犬 ...）的初始化钩子。
    virtual void OnGuardianReset() {}

    // =========================================================================
    // 毕业机制内核：核心功能光环注入
    // =========================================================================
    /// @brief 依据外观池类型注入对应的毕业功能光环：
    ///        - 野兽池（座狼内核）    -> 狂怒之嚎（周期增益，由 UpdateAI 定时器维护）
    ///        - 小鬼池（小鬼内核）    -> 血之契印（全队耐力光环）
    ///        - 地狱犬池（地狱犬内核）-> 恶魔智力（全队智力/精神光环）
    ///        并无条件注入全随从通用 AoE 免死被动【回避】(Avoidance)。
    ///
    ///        契约：「外观千人千面，机制内核永远毕业」。外观只影响表现层，
    ///        内核光环必须无条件按等级阶梯满配注入，杜绝低等级随从被注入
    ///        80 级光环导致数值溢出，或高级随从沿用低级光环导致增益缩水。
    void ApplyGuardianCoreAuras();
    // =========================================================================
    /// @brief 伴随型护卫严禁自主警戒引怪，视野内敌对目标一律忽略。
    ///        索敌权完全交由主人控制，仅通过 UpdateAI 同步主人转火。
    void MoveInLineOfSight(Unit* who) override;

    /// @brief 伴随战斗主循环：严格同步主人转火 / 脱战归位 / 超距拉回，并执行白字平砍。
    void UpdateAI(uint32 diff) override;

protected:
    /// @brief 宿主单位回溯解析器。
    ///        解析顺序为 魅惑者 -> 拥有者 -> 创建者：AzerothCore 的 Unit 并未提供
    ///        GetCharmerOrOwnerOrCreator()，且随从在刚生成的窗口期内只写入了
    ///        CreatorGUID，此处必须手工以创建者兜底，才能正确推断宿主职业，
    ///        杜绝术士小鬼 / 死骑食尸鬼退化为猎人野兽（近战白字）内核。
    [[nodiscard]] Unit* GetMaster() const;

    GuardianVisualType   _visualType;
    GuardianDisplayEntry _chosenDisplay{ 0, 1.0f };    ///< 首次生成时缓存的外观，Reset 幂等复用
    bool                 _visualTypeExplicit{ false }; ///< 是否由外部显式指定外观池
    bool                 _displayApplied{ false };     ///< 是否已完成首次 Roll 点

    uint32               _furiousHowlTimer{ 0 };       ///< 座狼内核【狂怒之嚎】周期刷新定时器 (ms)
};
