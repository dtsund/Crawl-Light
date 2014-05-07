#include "AppHdr.h"

#include "branch.h"
#include "externs.h"
#include "place.h"
#include "player.h"
#include "spl-transloc.h"
#include "traps.h"
#include "travel.h"
#include "branch-data.h"

Branch& your_branch()
{
    return branches[you.where_are_you];
}

bool at_branch_bottom()
{
    return your_branch().depth == player_branch_depth();
}

level_id branch_entry_level(branch_type branch)
{
    // Hell and its subbranches need obnoxious special-casing:
    if (branch == BRANCH_VESTIBULE_OF_HELL)
    {
        return level_id(you.hell_branch,
                        subdungeon_depth(you.hell_branch, you.hell_exit));
    }
    else if (is_hell_subbranch(branch))
    {
        return level_id(BRANCH_VESTIBULE_OF_HELL, 1);
    }

    const branch_type parent = branches[branch].parent_branch;
    const int subdepth = branches[branch].startdepth;

    // This may be invalid if the branch doesn't exist this game --
    // it's the caller's job to check.
    return level_id(parent, subdepth);
}

level_id current_level_parent()
{
    // TODO:LEVEL_STACK: go up the level stack
    return find_up_level(level_id::current());
}

bool is_hell_subbranch(branch_type branch)
{
    return (branch >= BRANCH_FIRST_HELL
            && branch <= BRANCH_LAST_HELL
            && branch != BRANCH_VESTIBULE_OF_HELL);
}

bool is_random_lair_subbranch(branch_type branch)
{
    return branches[branch].parent_branch == BRANCH_LAIR
        && branch != BRANCH_SLIME_PITS;
}

bool is_connected_branch(branch_type branch)
{
    ASSERT(branch >= 0 && branch < NUM_BRANCHES);
    return !(branches[branch].branch_flags & BFLAG_NO_XLEV_TRAVEL);
}

bool is_connected_branch(level_id place)
{
    return is_connected_branch(place.branch);
}

branch_type str_to_branch(const std::string &branch, branch_type err)
{
    for (int i = 0; i < NUM_BRANCHES; ++i)
        if (branches[i].abbrevname && branches[i].abbrevname == branch)
            return (static_cast<branch_type>(i));

    return (err);
}

int current_level_ambient_noise()
{
    return branches[you.where_are_you].ambient_noise;
}

bool branch_has_monsters(branch_type branch)
{
    return branches[branch].mons_rarity_function != mons_null_rare;
}

#if TAG_MAJOR_VERSION == 32
static const char *old_level_type_names[] =
{
    "D", "Lab", "Abyss", "Pan", "Port"
};

const char *old_level_area_type_name(int old_level_type)
{
    if (old_level_type >= 0 && old_level_type < NUM_LEVEL_AREA_TYPES)
        return old_level_type_names[old_level_type];
    return ("");
}

old_level_area_type str_to_old_level_area_type(const std::string &s)
{
    for (int i = 0; i < NUM_LEVEL_AREA_TYPES; ++i)
        if (s == old_level_type_names[i])
            return (static_cast<old_level_area_type>(i));

    return (OLD_LEVEL_DUNGEON);
}
#endif

bool set_branch_flags(uint32_t flags, bool silent, branch_type branch)
{
    if (branch == NUM_BRANCHES)
        branch = you.where_are_you;

    bool could_control = allow_control_teleport(true);
    bool could_map     = player_in_mappable_area();

    uint32_t old_flags = branches[branch].branch_flags;
    branches[branch].branch_flags |= flags;

    bool can_control = allow_control_teleport(true);
    bool can_map     = player_in_mappable_area();

    if (branch == you.where_are_you
        && could_control && !can_control && !silent)
    {
        mpr("You sense the appearance of a powerful magical force "
            "which warps space.", MSGCH_WARN);
    }

    if (branch == you.where_are_you && could_map && !can_map && !silent)
    {
        mpr("A powerful force appears that prevents you from "
            "remembering where you've been.", MSGCH_WARN);
    }

    return (old_flags != branches[branch].branch_flags);
}

bool unset_branch_flags(uint32_t flags, bool silent, branch_type branch)
{
    if (branch == NUM_BRANCHES)
        branch = you.where_are_you;

    const bool could_control = allow_control_teleport(true);
    const bool could_map     = player_in_mappable_area();

    uint32_t old_flags = branches[branch].branch_flags;
    branches[branch].branch_flags &= ~flags;

    const bool can_control = allow_control_teleport(true);
    const bool can_map     = player_in_mappable_area();

    if (branch == you.where_are_you
        && !could_control && can_control && !silent)
    {
        // Isn't really a "recovery", but I couldn't think of where
        // else to send it.
        mpr("Space seems to straighten in your vicinity.", MSGCH_RECOVERY);
    }

    if (branch == you.where_are_you
        && !could_map && can_map && !silent)
    {
        // Isn't really a "recovery", but I couldn't think of where
        // else to send it.
        mpr("An oppressive force seems to lift.", MSGCH_RECOVERY);
    }

    return (old_flags != branches[branch].branch_flags);
}

uint32_t get_branch_flags(branch_type branch)
{
    if (branch == NUM_BRANCHES)
        branch = you.where_are_you;

    return branches[branch].branch_flags;
}

bool is_portal_vault(branch_type branch)
{
    switch (branch)
    {
    // somehow not BRANCH_LABYRINTH
    case BRANCH_ZIGGURAT:
    case BRANCH_BAZAAR:
    case BRANCH_TROVE:
    case BRANCH_SEWER:
    case BRANCH_OSSUARY:
    case BRANCH_BAILEY:
    case BRANCH_ICE_CAVE:
    case BRANCH_VOLCANO:
    case BRANCH_WIZLAB:
        return true;
    default:
        return false;
    }
}
