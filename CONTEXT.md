# AzerothCore Hierarchical Adaptive Bot System - Context

## 1. Architecture Overview
- Layer 1 (Active): Expert Action Priority List (APL) for baseline bots. Non-intrusive, no 71-point talent tree, dynamic scaling via master ilvl in `AdaptiveBotAI`.
- Layer 2 (Planned): Async Combat Attribution Analyzer (triggered at wipe/kill, analyzes fatal spell/missed interrupts/OT).
- Layer 3 (Planned): SQLite persistence (`Boss_Entry_ID + Bot_ID`) for dynamic parameter evolution (interrupt delay, positioning blacklist).

## 2. Global Development Invariants
- Headers: `#include "Pet.h"` and `#include "Chat.h"` must be included in AI source files.
- Base Inheritance: Subclasses MUST call `AdaptiveBotAI::OnLevelSynced(level);` in `OnLevelSynced`.
- Stat/Combat Engine: Level 80 bots are injected with -5.6% crit taken, +8% physical hit, +17% spell hit, and +26 expertise.
- Rank Downscaling: Always use `GetAppropriateRank(SPELL_ID)` where `SPELL_ID` is the max level rank anchor.
- On-Next-Swing Skills (Maul, Rune Strike, Cleave): NEVER `return;` after casting. Allow execution to flow to subsequent GCD abilities.
- Resource Fallbacks: Creature bots do not have standard player rage/runic power generation. Provide `SupplementRage` / `SupplementRunicPower` loops to prevent 0-resource lockouts.

## 3. Completed Classes (Stage 1 Tanks)
- Protection Warrior: `src/Classes/Warrior/` (`BotProtectionWarriorAI.cpp`, `ProtectionWarriorSpells.h`)
- Protection Paladin: `src/Classes/Paladin/` (`BotProtectionPaladinAI.cpp`, `ProtectionPaladinSpells.h`)
  - Features: 969 rotation, Ardent Defender cheat-death, Divine Plea mana loop, Divine Sacrifice team guard, Ally radar.
- Blood Death Knight: `src/Classes/DeathKnight/` (`BotBloodDeathKnightAI.cpp`, `BloodDeathKnightSpells.h`)
  - Features: Frost Presence tank stance, 10s Pestilence throttle, dual-disease strict check on Death Strike, RP safe-floor, 30yd Dark Command.
- Feral Bear Druid: `src/Classes/Druid/` (`BotBearDruidAI.cpp`, `BearDruidSpells.h`)
  - Features: Dire Bear form lock, Swipe (Self-only AoE targeting fix), Lacerate 5-stack smart refresh (<=4500ms), Maul rage safeguard (>35%), Survival instincts.

## 4. Immediate Goal
- Expanding Stage 1 into Healer Bots (Holy Priest, Resto Shaman, Holy Paladin) or database registration SQL (`creature_template`).