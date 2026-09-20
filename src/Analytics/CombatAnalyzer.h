/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"
#include "ObjectGuid.h"
#include "Movement/DangerZones.h"
#include <array>
#include <cstddef>
#include <vector>

enum class BotCombatEventType : uint8
{
    NONE = 0,
    DAMAGE_TAKEN,       // 受到伤害
    LETHAL_DAMAGE,      // 致死伤害
    DAMAGE_DEALT,       // 造成伤害
    SPELL_HIT_TAKEN,    // 承受敌方法术命中
    THREAT_OT_WARNING,  // 起手 OT 警告
    TANK_THREAT_SAMPLE  // 主坦前 10 秒仇恨采样
};

struct BotCombatEvent
{
    uint32 combatTimeMs{ 0 };
    uint32 spellId{ 0 };
    uint32 amount{ 0 };
    float x{ 0.0f };              // 发生时坐标 (用于逆向提炼危险源)
    float y{ 0.0f };
    float z{ 0.0f };
    ObjectGuid sourceGuid;
    BotCombatEventType eventType{ BotCombatEventType::NONE };
    uint8 schoolMask{ 0 };
};

template <std::size_t Capacity = 256>
class BotCombatRingBuffer
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
public:
    BotCombatRingBuffer() = default;

    void Push(BotCombatEvent const& event)
    {
        _buffer[_head & (Capacity - 1)] = event;
        ++_head;
        if (_size < Capacity)
            ++_size;
    }

    void Clear()
    {
        _head = 0;
        _size = 0;
    }

    [[nodiscard]] std::size_t Size() const { return _size; }
    [[nodiscard]] bool Empty() const { return _size == 0; }

    template <typename Func>
    void ForEach(Func&& func) const
    {
        if (_size == 0)
            return;

        std::size_t const start = (_head >= _size) ? (_head - _size) : 0;
        for (std::size_t i = 0; i < _size; ++i)
            func(_buffer[(start + i) & (Capacity - 1)]);
    }

    template <typename Func>
    void ForEachReverse(Func&& func) const
    {
        if (_size == 0)
            return;

        for (std::size_t i = 0; i < _size; ++i)
        {
            std::size_t const index = (_head - 1 - i) & (Capacity - 1);
            if (!func(_buffer[index]))
                break;
        }
    }

private:
    std::array<BotCombatEvent, Capacity> _buffer{};
    std::size_t _head{ 0 };
    std::size_t _size{ 0 };
};

struct AttributionReport
{
    uint32 bossEntry{ 0 };
    uint32 totalCombatTimeMs{ 0 };
    bool isWipe{ false };

    // 维度 1: 承伤峰值与致死
    uint32 fatalSpellId{ 0 };
    uint32 fatalDamage{ 0 };
    ObjectGuid fatalSourceGuid;
    uint32 peakDamageSpellId{ 0 };
    uint32 peakDamage{ 0 };

    // 维度 2: 漏打断统计
    uint32 missedInterruptsCount{ 0 };
    uint32 lastMissedSpellId{ 0 };

    // 维度 3: 起手 OT 归因
    bool earlyOtDetected{ false };
    uint32 otTimeMs{ 0 };

    // 维度 4: 主坦前 10 秒 TPS
    float tankFirst10sTps{ 0.0f };

    // 衍生空间避险源：由致死与高额尖刺逆向生成的危险禁区
    std::vector<DangerZone> derivedDangerZones;
};

class CombatAnalyzer
{
public:
    static AttributionReport Analyze(
        BotCombatRingBuffer<256> const& ringBuffer,
        uint32 combatDurationMs,
        bool victory,
        uint32 bossEntry = 0);
};
