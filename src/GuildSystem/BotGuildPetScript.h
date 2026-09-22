/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "ScriptMgr.h"
#include "ScriptedGossip.h"

class BotGuildPetScript : public CreatureScript
{
public:
    BotGuildPetScript();

    CreatureAI* GetAI(Creature* creature) const override;

    bool OnGossipHello(Player* player, Creature* creature) override;
    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override;

private:
    static void ShowPetMainMenu(Player* player, Creature* creature);
    static void ShowTacticalCommands(Player* player, Creature* creature);
    static void ShowBillingStatus(Player* player, Creature* creature);
    static void HandleManualSettle(Player* player, Creature* creature);
    static void HandleDailySupply(Player* player, Creature* creature);
};

void AddSC_BotGuildPetScript();
