/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"
#include <cmath>

enum class DangerZoneType : uint8
{
    CIRCLE = 0,     // 地面圆形火圈 / 毒池 / 冰霜陷阱
    FRONTAL_CONE    // 首领正面顺劈 / 吐息锥形区
};

struct DangerZone
{
    DangerZoneType type{ DangerZoneType::CIRCLE };
    uint32 mapId{ 0 };            // 归属地图 ID (防跨地图残留)
    float x{ 0.0f };
    float y{ 0.0f };
    float z{ 0.0f };
    float radius{ 5.0f };         // 影响半径
    float orientation{ 0.0f };    // 锥形朝向
    float coneAngle{ static_cast<float>(M_PI / 2.0) }; // 锥形开角 (默认 90 度)
    uint32 spellId{ 0 };          // 对应技能 ID
    uint32 durationMs{ 15000 };   // 留存时间
};
