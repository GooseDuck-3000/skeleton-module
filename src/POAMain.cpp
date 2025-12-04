/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "Spell.h"
#include "ScriptedGossip.h"
#include "SpellMgr.h"

struct POA
{
    uint32 message;
    bool enableSkillTransfer;
    bool enableLearnSpells;
};

static const uint32 POA_SKILLS[] =
{
    129, // First Aid
    164, // Blacksmithing
    165, // Leatherworking
    171, // Alchemy
    182, // Herbalismo
    185, // Cooking
    186, // Mining
    197, // Tailoring
    202, // Engineering
    333, // Enchanting
    356, // Fishing
    393, // Skinning
    755, // Jewelcrafting
    773, // Inscription
};

static std::string BuildSkillList()
{
    std::ostringstream ss;

    for (size_t i = 0; i < std::size(POA_SKILLS); ++i)
    {
        ss << POA_SKILLS[i];
        if (i + 1 < std::size(POA_SKILLS))
            ss << ", ";
    }

    return ss.str();
}

POA poa;

class POAPlayer : public PlayerScript
{
public:
    POAPlayer() : PlayerScript("POAPlayer") {}

    void OnPlayerLogin(Player* player) override
    {
        if (poa.enableSkillTransfer)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(poa.message);

            std::string skillList = BuildSkillList();

            QueryResult resultSkills = CharacterDatabase.Query("SELECT skill, guid, value, max FROM (SELECT cs.skill, cs.guid, cs.value, cs.max, ROW_NUMBER() OVER (PARTITION BY cs.skill ORDER BY cs.value DESC) AS rn FROM character_skills cs JOIN characters c_all ON cs.guid = c_all.guid JOIN characters c_current ON c_current.account = c_all.account WHERE c_current.guid = {} AND cs.skill IN (" + skillList + ")) ranked WHERE rn = 1;", player->GetGUID().GetRawValue());

            if (resultSkills)
            {
                uint32 skillID;
                uint32 skillVal;
                uint32 skillMax;
                do
                {
                    skillID = (*resultSkills)[0].Get<int32>();  //[0] indicates index to set, 0 is skillID
                    skillVal = (*resultSkills)[2].Get<int32>(); //[2] indicates index to set, 2 is skillValue
                    skillMax = (*resultSkills)[3].Get<int32>(); //[3] indicates index to set, 3 is skillMax

                    if (player->GetPureSkillValue(skillID) < skillVal)
                    {
                        player->SetSkill(skillID, 0, skillVal, skillMax);
                    }
                } while (resultSkills->NextRow());
            }
        }

        if (poa.enableLearnSpells)
        {
            uint32 accountId = player->GetSession()->GetAccountId();
            uint32 team = player->GetTeamId();

            // Pull all recorded spells for this account
            QueryResult resultSpells = LoginDatabase.Query("SELECT `spell_id`, `team_id` FROM `mod_profs_on_account` WHERE `account_id`={};", accountId);

            if (resultSpells)
            {
                do
                {
                    uint32 spellId = (*resultSpells)[0].Get<uint32>();
                    uint32 storedTeam = (*resultSpells)[1].Get<uint32>();

                    if (player->HasSpell(spellId)) //skip if known
                        continue;

                    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                    if (!info)
                        continue;

                    player->learnSpell(spellId, false);

                } while (resultSpells->NextRow());
            }
        }
    }

    void CustomLearnSpellPoa(Player* player, uint32 spellID)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
        if (!spellInfo)
            return;

        bool isSpellEffectMatch = false;    // CREATE_ITEM, TRADE_SKILL, ENCHANT_ITEM
        bool hasSkillEffect = false;        // SPELL_EFFECT_SKILL
        bool hasOpenLockEffect = false;     // SPELL_EFFECT_OPEN_LOCK

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 eff = spellInfo->Effects[i].Effect;

            //primarily recipes
            if (eff == SPELL_EFFECT_CREATE_ITEM ||
                eff == SPELL_EFFECT_CREATE_ITEM_2 ||
                eff == SPELL_EFFECT_TRADE_SKILL ||
                eff == SPELL_EFFECT_ENCHANT_ITEM)
            {
                isSpellEffectMatch = true;
            }

            //primarily profession spells (those that bring up the recipe list)
            if (eff == SPELL_EFFECT_SKILL)
            {
                hasSkillEffect = true;
            }

            if (eff == SPELL_EFFECT_OPEN_LOCK)
            {
                hasOpenLockEffect = true;
            }
        }

        bool isTradeskillAttribute =
            spellInfo->Attributes & SPELL_ATTR0_IS_TRADESKILL;

        uint32 accountID = player->GetSession()->GetAccountId();
        uint32 playerTeam = player->GetTeamId();

        bool acceptSpell = false;

        // Normal recipe + tradeskill attribute
        if (isSpellEffectMatch && isTradeskillAttribute)
        {
            acceptSpell = true;
        }
        // Missing attribute but still a recipe + skill effect
        else if (isSpellEffectMatch && hasSkillEffect)
        {
            acceptSpell = true;
        }
        // Open-Lock + Skill effect (herbing & mining)
        else if (hasOpenLockEffect && hasSkillEffect)
        {
            acceptSpell = true;
        }

        if (!acceptSpell)
            return;
        {
            QueryResult resultSpell = LoginDatabase.Query("SELECT `spell_id` FROM `mod_profs_on_account` WHERE `account_id`={} AND `spell_id`={};", accountID, spellID);

            if (!resultSpell)
            {
                LoginDatabase.Query("INSERT INTO `mod_profs_on_account` (`account_id`, `team_id`, `spell_id`) VALUES ({}, {}, {});", accountID, playerTeam, spellID);
            }
        }
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (poa.enableLearnSpells)
        {
            uint32 spellID = spell->GetSpellInfo()->Id;
            CustomLearnSpellPoa(player, spellID);
        }
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellID) override
    {
        if (poa.enableLearnSpells)
        {
            CustomLearnSpellPoa(player, spellID);
        }
    }
};

class POAWorld : public WorldScript
{
public:
    POAWorld() : WorldScript("POAWorld") {}

    void OnBeforeConfigLoad(bool reload) override
    {
        if (!reload)
        {
            sConfigMgr->LoadModulesConfigs();
            poa.enableSkillTransfer = sConfigMgr->GetOption<bool>("poa.enableSkillTransfer", true);
            poa.message = sConfigMgr->GetOption<uint32>("poa.message.id", 45001);
            poa.enableLearnSpells = sConfigMgr->GetOption<bool>("poa.enableLearnSpells", true);
        }
    }
};

void AddPOAPlayerScripts()
{
    new POAPlayer();
    new POAWorld();
}
