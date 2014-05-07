#include "AppHdr.h"

#include "stairs.h"

#include <sstream>

#include "abyss.h"
#include "areas.h"
#include "branch.h"
#include "chardump.h"
#include "coordit.h"
#include "delay.h"
#include "dgn-overview.h"
#include "directn.h"
#include "env.h"
#include "files.h"
#include "fprop.h"
#include "hints.h"
#include "hiscores.h"
#include "itemname.h"
#include "items.h"
#include "lev-pand.h"
#include "libutil.h"
#include "mapmark.h"
#include "message.h"
#include "misc.h"
#include "mon-iter.h"
#include "mon-transit.h"
#include "notes.h"
#include "options.h"
#include "ouch.h"
#include "output.h"
#include "place.h"
#include "random.h"
#include "spl-clouds.h"
#include "spl-damage.h"
#include "spl-transloc.h"
#include "stash.h"
#include "state.h"
#include "stuff.h"
#include "tagstring.h"
#include "terrain.h"
#ifdef USE_TILE
 #include "tilepick.h"
#endif
#include "traps.h"
#include "travel.h"
#include "view.h"
#include "xom.h"

bool check_annotation_exclusion_warning()
{
    // Players might not realise the implications of teleport mutations
    // in the labyrinth.
    if (grd(you.pos()) == DNGN_ENTER_LABYRINTH
        && player_mutation_level(MUT_TELEPORT))
    {
        mpr("Within the labyrinth you'll only be able to teleport away from "
            "the exit!");
        if (!yesno("Continue anyway?", false, 'N', true, false))
        {
            canned_msg(MSG_OK);
            interrupt_activity(AI_FORCE_INTERRUPT);
            return (false);
        }
        return (true);
    }

    level_id  next_level_id = level_id::get_next_level_id(you.pos());

    crawl_state.level_annotation_shown = false;
    bool might_be_dangerous = false;

    if (level_annotation_has("!", next_level_id)
        && next_level_id != level_id::current()
        && is_connected_branch(next_level_id))
    {
        mpr("Warning, next level annotated: " +
            colour_string(get_level_annotation(next_level_id), YELLOW),
            MSGCH_PROMPT);
        might_be_dangerous = true;
        crawl_state.level_annotation_shown = true;
    }
    else if (is_exclude_root(you.pos())
             && feat_is_travelable_stair(grd(you.pos()))
             && !strstr(get_exclusion_desc(you.pos()).c_str(), "cloud"))
    {
        mpr("This staircase is marked as excluded!", MSGCH_WARN);
        might_be_dangerous = true;
    }

    if (might_be_dangerous
        && !yesno("Enter next level anyway?", true, 'n', true, false))
    {
        canned_msg(MSG_OK);
        interrupt_activity(AI_FORCE_INTERRUPT);
        crawl_state.level_annotation_shown = false;
        return (false);
    }
    return (true);
}

static void _player_change_level_reset()
{
    you.prev_targ  = MHITNOT;
    if (you.pet_target != MHITYOU)
        you.pet_target = MHITNOT;

    you.prev_grd_targ.reset();
}

static level_id _stair_destination_override()
{
    const std::string force_place = (grd(you.pos()) == DNGN_EXIT_ABYSS) ?
        static_cast<std::string>(you.props["abyss_return_desc"]) :
        env.markers.property_at(you.pos(), MAT_ANY, "dstplace");
    if (!force_place.empty())
    {
        try
        {
            const level_id place = level_id::parse_level_id(force_place);
            return (place);
        }
        catch (const std::string &err)
        {
            end(1, false, "Marker set with invalid level name: %s",
                force_place.c_str());
        }
    }

    const level_id invalid;
    return (invalid);
}

static bool _stair_force_destination(const level_id &override)
{
    if (override.is_valid())
    {
        you.where_are_you = override.branch;
        you.absdepth0     = override.absdepth();
        return (true);
    }
    return (false);
}

static void _player_change_level_upstairs(dungeon_feature_type stair_find,
                                          const level_id &place_override)
{
    if (_stair_force_destination(place_override))
        return;

    if (player_in_connected_branch())
        you.absdepth0--;

    // Make sure we return to our main dungeon level... labyrinth entrances
    // in the abyss or pandemonium are a bit trouble (well the labyrinth does
    // provide a way out of those places, its really not that bad I suppose).
    if (branch_exits_up(you.where_are_you))
    {
        // TODO:LEVEL_STACK exit to main dungeon without a marker
        die("branch exit without return destination");
    }

    if (player_in_branch(BRANCH_VESTIBULE_OF_HELL))
    {
        you.where_are_you = you.hell_branch;
        you.absdepth0 = you.hell_exit;
    }

    if (player_in_hell())
    {
        you.where_are_you = BRANCH_VESTIBULE_OF_HELL;
        you.absdepth0 = 27;
    }

    // Did we take a branch stair?
    for (int i = 0; i < NUM_BRANCHES; ++i)
    {
        if (branches[i].exit_stairs == stair_find
            && you.where_are_you == i)
        {
            you.where_are_you = branches[i].parent_branch;

            // If leaving a branch which wasn't generated in this
            // particular game (like the Swamp or Shoals), then
            // its startdepth is set to -1; compensate for that,
            // so we don't end up on "level -1".
            if (brdepth[i] == -1)
                you.absdepth0 += 2;
            break;
        }
    }
}

static bool _marker_vetoes_level_change()
{
    return (marker_vetoes_operation("veto_level_change"));
}

static bool _stair_moves_pre(dungeon_feature_type stair)
{
    if (crawl_state.prev_cmd == CMD_WIZARD)
        return (false);

    if (stair != grd(you.pos()))
        return (false);

    if (feat_stair_direction(stair) == CMD_NO_CMD)
        return (false);

    if (!you.duration[DUR_REPEL_STAIRS_CLIMB])
        return (false);

    int pct;
    if (you.duration[DUR_REPEL_STAIRS_MOVE])
        pct = 29;
    else
        pct = 50;

    // When the effect is still strong, the chance to actually catch a stair
    // is smaller. (Assuming the duration starts out at 500.)
    const int dur = std::max(0, you.duration[DUR_REPEL_STAIRS_CLIMB] - 200);
    pct += dur/20;

    if (!x_chance_in_y(pct, 100))
        return (false);

    // Get feature name before sliding stair over.
    std::string stair_str =
        feature_description(you.pos(), false, DESC_CAP_THE, false);

    if (!slide_feature_over(you.pos(), coord_def(-1, -1), false))
        return (false);

    std::string verb = stair_climb_verb(stair);

    mprf("%s moves away as you attempt to %s it!", stair_str.c_str(),
         verb.c_str());

    you.turn_is_over = true;

    return (true);
}

// Adds a dungeon marker at the point of the level where returning from
// a labyrinth or portal vault should drop the player.
static void _mark_portal_return_point(const coord_def &pos)
{
    // First toss all markers of this type. Stale markers are possible
    // if the player goes to the Abyss from a portal vault /
    // labyrinth, thus returning to this level without activating a
    // previous portal vault exit marker.
    const std::vector<map_marker*> markers = env.markers.get_all(MAT_FEATURE);
    for (int i = 0, size = markers.size(); i < size; ++i)
    {
        if (dynamic_cast<map_feature_marker*>(markers[i])->feat ==
            DNGN_EXIT_PORTAL_VAULT)
        {
            env.markers.remove(markers[i]);
        }
    }

    if (!env.markers.find(pos, MAT_FEATURE))
    {
        map_feature_marker *mfeat =
            new map_feature_marker(pos, DNGN_EXIT_PORTAL_VAULT);
        env.markers.add(mfeat);
    }
}

static void _exit_stair_message(dungeon_feature_type stair, bool /* going_up */)
{
    if (feat_is_escape_hatch(stair))
        mpr("The hatch slams shut behind you.");
}

static void _climb_message(dungeon_feature_type stair, bool going_up,
                           branch_type old_branch)
{
    if (!is_connected_branch(old_branch))
        return;

    if (feat_is_portal(stair))
        mpr("The world spins around you as you enter the gateway.");
    else if (feat_is_escape_hatch(stair))
    {
        if (going_up)
            mpr("A mysterious force pulls you upwards.");
        else
        {
            mprf("You %s downwards.",
                 you.flight_mode() == FL_FLY ? "fly" :
                     (you.airborne() ? "float" : "slide"));
        }
    }
    else if (feat_is_gate(stair))
    {
        mprf("You %s %s through the gate.",
             you.flight_mode() == FL_FLY ? "fly" :
                   (you.airborne() ? "float" : "go"),
             going_up ? "up" : "down");
    }
    else
    {
        mprf("You %s %swards.",
             you.flight_mode() == FL_FLY ? "fly" :
                   (you.airborne() ? "float" : "climb"),
             going_up ? "up" : "down");
    }
}

static void _clear_golubria_traps()
{
    std::vector<coord_def> traps = find_golubria_on_level();
    for (std::vector<coord_def>::const_iterator it = traps.begin(); it != traps.end(); ++it)
    {
        trap_def *trap = find_trap(*it);
        if (trap && trap->type == TRAP_GOLUBRIA)
            trap->destroy();
    }
}

static void _leaving_level_now(dungeon_feature_type stair_used)
{
    if (player_in_branch(BRANCH_ZIGGURAT)
        && stair_used == DNGN_EXIT_PORTAL_VAULT
        && player_branch_depth() == 27)
    {
        you.zigs_completed++;
    }

    // Note the name ahead of time because the events may cause markers
    // to be discarded.
    const std::string newtype =
        env.markers.property_at(you.pos(), MAT_ANY, "dst");

    const std::string oldname_abbrev = you.old_level_type_name_abbrev;
    std::string newname_abbrev =
        env.markers.property_at(you.pos(), MAT_ANY, "dstname_abbrev");

    dungeon_events.fire_position_event(DET_PLAYER_CLIMBS, you.pos());
    dungeon_events.fire_event(DET_LEAVING_LEVEL);

    // Lua scripts explicitly set level_type_name_abbrev, so use that.
    if (you.old_level_type_name_abbrev != oldname_abbrev)
        newname_abbrev = you.old_level_type_name_abbrev;

    if (strwidth(newname_abbrev) > MAX_NOTE_PLACE_LEN)
    {
        mprf(MSGCH_ERROR, "'%s' is too long for a portal vault name "
                          "abbreviation, truncating", newname_abbrev.c_str());
        newname_abbrev = chop_string(newname_abbrev, MAX_NOTE_PLACE_LEN, false);
    }

    _clear_golubria_traps();

    if (grd(you.pos()) == DNGN_EXIT_ABYSS)
    {
        // TODO:LEVEL_STACK
        you.old_level_type_name_abbrev =
            static_cast<std::string>(you.props["abyss_return_abbrev"]);
        you.props.erase("abyss_return_desc");
        you.props.erase("abyss_return_name");
        you.props.erase("abyss_return_abbrev");
        you.props.erase("abyss_return_origin");
        you.props.erase("abyss_return_tag");
        you.props.erase("abyss_return_ext");
    }
}

static void _set_entry_cause(entry_cause_type default_cause,
                             branch_type old_branch)
{
    ASSERT(default_cause != NUM_ENTRY_CAUSE_TYPES);

    if (old_branch == you.where_are_you && you.entry_cause != EC_UNKNOWN)
        return;

    if (crawl_state.is_god_acting())
    {
        if (crawl_state.is_god_retribution())
            you.entry_cause = EC_GOD_RETRIBUTION;
        else
            you.entry_cause = EC_GOD_ACT;

        you.entry_cause_god = crawl_state.which_god_acting();
    }
    else if (default_cause != EC_UNKNOWN)
    {
        you.entry_cause     = default_cause;
        you.entry_cause_god = GOD_NO_GOD;
    }
    else
    {
        you.entry_cause     = EC_SELF_EXPLICIT;
        you.entry_cause_god = GOD_NO_GOD;
    }
}

static void _update_travel_cache(bool collect_travel_data,
                                 const level_id& old_level,
                                 const coord_def& stair_pos)
{
    if (collect_travel_data)
    {
        // Update stair information for the stairs we just ascended, and the
        // down stairs we're currently on.
        level_id  new_level_id    = level_id::current();

        if (can_travel_interlevel())
        {
            LevelInfo &old_level_info =
                        travel_cache.get_level_info(old_level);
            LevelInfo &new_level_info =
                        travel_cache.get_level_info(new_level_id);
            new_level_info.update();

            // First we update the old level's stair.
            level_pos lp;
            lp.id  = new_level_id;
            lp.pos = you.pos();

            bool guess = false;
            // Ugly hack warning:
            // The stairs in the Vestibule of Hell exhibit special behaviour:
            // they always lead back to the dungeon level that the player
            // entered the Vestibule from. This means that we need to pretend
            // we don't know where the upstairs from the Vestibule go each time
            // we take it. If we don't, interlevel travel may try to use portals
            // to Hell as shortcuts between dungeon levels, which won't work,
            // and will confuse the dickens out of the player (well, it confused
            // the dickens out of me when it happened).
            if (new_level_id == BRANCH_MAIN_DUNGEON
                && old_level == BRANCH_VESTIBULE_OF_HELL)
            {
                old_level_info.clear_stairs(DNGN_EXIT_HELL);
            }
            else
            {
                old_level_info.update_stair(stair_pos, lp, guess);
            }

            // We *guess* that going up a staircase lands us on a downstair,
            // and that we can descend that downstair and get back to where we
            // came from. This assumption is guaranteed false when climbing out
            // of one of the branches of Hell.
            if (new_level_id != BRANCH_VESTIBULE_OF_HELL
                || !is_hell_subbranch(old_level.branch))
            {
                // Set the new level's stair, assuming arbitrarily that going
                // downstairs will land you on the same upstairs you took to
                // begin with (not necessarily true).
                lp.id = old_level;
                lp.pos = stair_pos;
                new_level_info.update_stair(you.pos(), lp, true);
            }
        }
    }
    else // !collect_travel_data
    {
        travel_cache.erase_level_info(old_level);
    }
}

void up_stairs(dungeon_feature_type force_stair,
               entry_cause_type entry_cause)
{
    dungeon_feature_type stair_find = (force_stair ? force_stair
                                       : grd(you.pos()));
    const level_id  old_level = level_id::current();

    // Up and down both work for shops.
    if (stair_find == DNGN_ENTER_SHOP)
    {
        shop();
        return;
    }

    // Up and down both work for portals.
    if (feat_is_bidirectional_portal(stair_find))
    {
        if (!(stair_find == DNGN_ENTER_HELL && player_in_hell()))
        {
            down_stairs(force_stair, entry_cause);
            return;
        }
    }
    // Probably still need this check here (teleportation) -- bwr
    else if (feat_stair_direction(stair_find) != CMD_GO_UPSTAIRS)
    {
        if (stair_find == DNGN_STONE_ARCH)
            mpr("There is nothing on the other side of the stone arch.");
        else if (stair_find == DNGN_ABANDONED_SHOP)
            mpr("This shop appears to be closed.");
        else
            mpr("You can't go up here.");
        return;
    }

    if (_stair_moves_pre(stair_find))
        return;

    // Since the overloaded message set turn_is_over, I'm assuming that
    // the overloaded character makes an attempt... so we're doing this
    // check before that one. -- bwr
    if (!you.airborne()
        && you.confused()
        && !feat_is_escape_hatch(stair_find)
        && coinflip())
    {
        const char* fall_where = "down the stairs";
        if (!feat_is_staircase(stair_find))
            fall_where = "through the gate";

        mprf("In your confused state, you trip and fall back %s.", fall_where);
        if (!feat_is_staircase(stair_find))
            ouch(1, NON_MONSTER, KILLED_BY_FALLING_THROUGH_GATE);
        else
            ouch(1, NON_MONSTER, KILLED_BY_FALLING_DOWN_STAIRS);
        you.turn_is_over = true;
        return;
    }

    const level_id destination_override(_stair_destination_override());

    // Bail if any markers veto the move.
    if (_marker_vetoes_level_change())
        return;

    // Magical level changes (don't exist yet in this direction)
    // need this.
    clear_trapping_net();

    // Checks are done, the character is committed to moving between levels.
    _leaving_level_now(stair_find);

    // Interlevel travel data.
    const bool collect_travel_data = can_travel_interlevel();
    if (collect_travel_data)
    {
        LevelInfo &old_level_info = travel_cache.get_level_info(old_level);
        old_level_info.update();
    }

    _player_change_level_reset();
    _player_change_level_upstairs(stair_find, destination_override);

    if (you.absdepth0 < 0)
    {
        mpr("You have escaped!");

        for (int i = 0; i < ENDOFPACK; i++)
        {
            if (you.inv[i].defined()
                && you.inv[i].base_type == OBJ_ORBS)
            {
                ouch(INSTANT_DEATH, NON_MONSTER, KILLED_BY_WINNING);
            }
        }

        ouch(INSTANT_DEATH, NON_MONSTER, KILLED_BY_LEAVING);
    }

    if (old_level.branch == BRANCH_VESTIBULE_OF_HELL
        && !player_in_branch(BRANCH_VESTIBULE_OF_HELL))
    {
        mpr("Thank you for visiting Hell. Please come again soon.");
    }

    // Fixup exits from the Hell branches.
    if (player_in_branch(BRANCH_VESTIBULE_OF_HELL))
    {
        switch (old_level.branch)
        {
        case BRANCH_COCYTUS:  stair_find = DNGN_ENTER_COCYTUS;  break;
        case BRANCH_DIS:      stair_find = DNGN_ENTER_DIS;      break;
        case BRANCH_GEHENNA:  stair_find = DNGN_ENTER_GEHENNA;  break;
        case BRANCH_TARTARUS: stair_find = DNGN_ENTER_TARTARUS; break;
        default: break;
        }
    }

    const dungeon_feature_type stair_taken = stair_find;

    if (you.flight_mode() == FL_LEVITATE && !feat_is_gate(stair_find))
        mpr("You float upwards... And bob straight up to the ceiling!");
    else if (you.flight_mode() == FL_FLY && !feat_is_gate(stair_find))
        mpr("You fly upwards.");
    else
        _climb_message(stair_find, true, old_level.branch);

    _exit_stair_message(stair_find, true);

    if (old_level.branch != you.where_are_you)
    {
        mprf("Welcome back to %s!",
             branches[you.where_are_you].longname);
        
        //When leaving Tartarus, restore lost experience.
        if(old_level.branch == BRANCH_TARTARUS)
        {
            if(you.experience < you.undrained_experience)
            {
                mpr("You sigh with relief as your memories come flooding back.");
                you.experience = you.undrained_experience;
                level_change();
            }
        }
    }

    const coord_def stair_pos = you.pos();

    load_level(stair_taken, LOAD_ENTER_LEVEL, old_level);

    _set_entry_cause(entry_cause, old_level.branch);
    entry_cause = you.entry_cause;

    you.turn_is_over = true;

    save_game_state();

    new_level();

    _update_travel_cache(collect_travel_data, old_level, stair_pos);

    env.map_shadow = env.map_knowledge;
    // Preventing obvious finding of stairs at your position.
    env.map_shadow(you.pos()).flags |= MAP_SEEN_FLAG;

    viewwindow();

    // Checking new squares for interesting features.
    if (!you.running)
        check_for_interesting_features();

    seen_monsters_react();

    if (!allow_control_teleport(true))
        mpr("You sense a powerful magical force warping space.", MSGCH_WARN);
    
    check_relatively_safe(false);

    request_autopickup();
    
    you.entry_coords = you.pos();
}

// All changes to you.where_are_you and you.absdepth0
// for descending stairs should happen here.
static void _player_change_level_downstairs(dungeon_feature_type stair_find,
                                            const level_id &place_override,
                                            bool shaft,
                                            int shaft_level,
                                            const level_id &shaft_dest)
{
    if (_stair_force_destination(place_override))
        return;

    const branch_type old_branch = you.where_are_you;

    if (!player_in_connected_branch()
        && (you.where_are_you != BRANCH_PANDEMONIUM
            || stair_find != DNGN_TRANSIT_PANDEMONIUM)
        && (you.where_are_you != BRANCH_ZIGGURAT
            || !feat_is_stone_stair(stair_find)))
    {
        // TODO:LEVEL_STACK exit to main dungeon without a marker
        die("branch exit without return destination");
    }

    if (stair_find == DNGN_ENTER_HELL)
    {
        you.hell_branch = you.where_are_you;
        you.where_are_you = BRANCH_VESTIBULE_OF_HELL;
        you.hell_exit = you.absdepth0;

        you.absdepth0 = 26;
    }

    // Welcome message.
    // Try to find a branch stair.
    for (int i = 0; i < NUM_BRANCHES; ++i)
    {
        if (branches[i].entry_stairs == stair_find)
        {
            you.where_are_you = branches[i].id;
            break;
        }
    }

    if (stair_find == DNGN_ENTER_LABYRINTH)
        you.where_are_you = BRANCH_LABYRINTH;
    else if (stair_find == DNGN_ENTER_ABYSS)
        you.where_are_you = BRANCH_ABYSS;
    else if (stair_find == DNGN_ENTER_ABYSS_DIRECTED)
    {
        you.where_are_you = BRANCH_ABYSS;
        //Let the player find the hell key from directed portals,
        //but not the Abyssal rune.
        env.can_find_hell_key = true;
    }
    else if (stair_find == DNGN_ENTER_PANDEMONIUM)
        you.where_are_you = BRANCH_PANDEMONIUM;
    else if (stair_find == DNGN_ENTER_PORTAL_VAULT)
        die("TODO: portal vault entrance");

    if (shaft)
    {
        you.absdepth0    = shaft_level;
        you.where_are_you = shaft_dest.branch;
    }
    else if (is_connected_branch(old_branch)
             && player_in_connected_branch())
    {
        you.absdepth0++;
    }
}

static void _maybe_destroy_trap(const coord_def &p)
{
    trap_def* trap = find_trap(p);
    if (trap)
        trap->destroy();
}

static bool _is_portal_exit(dungeon_feature_type stair)
{
    return stair == DNGN_EXIT_HELL
        || stair == DNGN_EXIT_ABYSS
        || stair == DNGN_EXIT_PORTAL_VAULT;
}

void down_stairs(dungeon_feature_type force_stair,
                 entry_cause_type entry_cause, const level_id* force_dest)
{
    const level_id old_level = level_id::current();
    const dungeon_feature_type old_feat = grd(you.pos());
    const dungeon_feature_type stair_find =
        force_stair? force_stair : old_feat;

    const bool shaft = (!force_stair
                            && get_trap_type(you.pos()) == TRAP_SHAFT
                        || force_stair == DNGN_TRAP_NATURAL);
    level_id shaft_dest;
    int      shaft_level = -1;

    // Up and down both work for shops.
    if (stair_find == DNGN_ENTER_SHOP)
    {
        shop();
        return;
    }

    // Up and down both work for portals.
    if (feat_is_bidirectional_portal(stair_find))
    {
        if (stair_find == DNGN_ENTER_HELL && player_in_hell())
        {
            up_stairs(force_stair, entry_cause);
            return;
        }
    }
    // Probably still need this check here (teleportation) -- bwr
    else if (feat_stair_direction(stair_find) != CMD_GO_DOWNSTAIRS && !shaft)
    {
        if (stair_find == DNGN_STONE_ARCH)
            mpr("There is nothing on the other side of the stone arch.");
        else if (stair_find == DNGN_ABANDONED_SHOP)
            mpr("This shop appears to be closed.");
        else
            mpr("You can't go down here!");
        return;
    }

    if (stair_find == DNGN_ENTER_HELL && !you.found_hell_key)
    {
        mpr("The massive gates leading into Hell are barred, and you lack the "
            "key.");
        return;
    }
    
    // Block the player from entering Pandemonium if playing on Easy.
    if (stair_find == DNGN_ENTER_PANDEMONIUM && you.difficulty_level == 0)
    {
        mpr("The gate appears to be sealed; there isn't enough ambient evil "
            "to pass through it.");
        return;
    }

    if (stair_find > DNGN_ENTER_LABYRINTH
        && stair_find <= DNGN_ESCAPE_HATCH_DOWN
        && player_in_branch(BRANCH_VESTIBULE_OF_HELL))
    {
        // Down stairs in vestibule are one-way!
        // This doesn't make any sense. Why would there be any down stairs
        // in the Vestibule? {due, 9/2010}
        mpr("A mysterious force prevents you from descending the staircase.");
        return;
    }

    if (stair_find == DNGN_STONE_ARCH)
    {
        mpr("There is nothing on the other side of the stone arch.");
        return;
    }

    if (_stair_moves_pre(stair_find))
        return;

    if (shaft)
    {
        const bool known_trap = (grd(you.pos()) != DNGN_UNDISCOVERED_TRAP
                                 && !force_stair);

        if (you.flight_mode() == FL_LEVITATE && !force_stair)
        {
            if (known_trap)
                mpr("You can't fall through a shaft while levitating.");
            return;
        }

        if (!is_valid_shaft_level())
        {
            if (known_trap)
                mpr("The shaft disappears in a puff of logic!");
            _maybe_destroy_trap(you.pos());
            return;
        }

        shaft_dest = you.shaft_dest(known_trap);
        if (shaft_dest == level_id::current())
        {
            if (known_trap)
            {
                mpr("Strange, the shaft seems to lead back to this level.");
                mpr("The strain on the space-time continuum destroys the "
                    "shaft!");
            }
            _maybe_destroy_trap(you.pos());
            return;
        }
        shaft_level = absdungeon_depth(shaft_dest.branch, shaft_dest.depth);

        if (!known_trap && shaft_level - you.absdepth0 > 1)
            mark_milestone("shaft", "fell down a shaft to " +
                                    short_place_name(shaft_dest) + ".");

        if (you.flight_mode() != FL_FLY || force_stair)
            mpr("You fall through a shaft!");
        if (you.flight_mode() == FL_FLY && !force_stair)
            mpr("You dive down through the shaft.");

        // Shafts are one-time-use.
        mpr("The shaft crumbles and collapses.");
        _maybe_destroy_trap(you.pos());
    }

    if (stair_find == DNGN_ENTER_ZOT && !you.opened_zot)
    {
        std::vector<int> runes;
        for (int i = 0; i < NUM_RUNE_TYPES; i++)
            if (you.runes[i])
                runes.push_back(i);

        if (runes.size() < NUMBER_OF_RUNES_NEEDED)
        {
            switch (NUMBER_OF_RUNES_NEEDED)
            {
            case 1:
                mpr("You need a rune to enter this place.");
                break;

            default:
                mprf("You need at least %d runes to enter this place.",
                     NUMBER_OF_RUNES_NEEDED);
            }
            return;
        }

        ASSERT(runes.size() >= 3);

        std::random_shuffle(runes.begin(), runes.end());
        mprf("You insert the %s rune into the lock.", rune_type_name(runes[0]));
#ifdef USE_TILE
        tiles.add_overlay(you.pos(), tileidx_zap(GREEN));
        update_screen();
#else
        flash_view(LIGHTGREEN);
#endif
        mpr("The lock glows an eerie green colour!");
        more();

        mprf("You insert the %s rune into the lock.", rune_type_name(runes[1]));
        big_cloud(CLOUD_BLUE_SMOKE, &you, you.pos(), 20, 7 + random2(7));
        viewwindow();
        mpr("Heavy smoke blows from the lock!");
        more();

        mprf("You insert the %s rune into the lock.", rune_type_name(runes[2]));

        if (truly_silenced(you.pos()))
            mpr("The gate opens wide!");
        else
            mpr("With a loud hiss the gate opens wide!");
        more();

        you.opened_zot = true;
    }

    // Bail if any markers veto the move.
    if (_marker_vetoes_level_change())
        return;

    level_id destination_override = _stair_destination_override();
    if (force_dest)
        destination_override = *force_dest;

    // All checks are done, the player is on the move now.

    // Magical level changes (Portal, Banishment) need this.
    clear_trapping_net();

    // Fire level-leaving trigger.
    _leaving_level_now(stair_find);

    if (!force_stair && !crawl_state.game_is_arena())
    {
        // Not entirely accurate - the player could die before
        // reaching the Abyss.
        if (grd(you.pos()) == DNGN_ENTER_ABYSS)
            mark_milestone("abyss.enter", "entered the Abyss!");
        else if (grd(you.pos()) == DNGN_EXIT_ABYSS
                 && you.char_direction != GDT_GAME_START)
        {
            mark_milestone("abyss.exit", "escaped from the Abyss!");
        }
    }

    // Interlevel travel data.
    const bool collect_travel_data = can_travel_interlevel();
    if (collect_travel_data)
    {
        LevelInfo &old_level_info = travel_cache.get_level_info(old_level);
        old_level_info.update();
    }
    const coord_def stair_pos = you.pos();

    // XXX: Obsolete, now that labyrinth entrances are only placed via Lua
    //      with timed markers. Leaving in to reduce the chance of an
    //      accidental permanent labyrinth entry. [rob]
    if (stair_find == DNGN_ENTER_LABYRINTH)
        dungeon_terrain_changed(you.pos(), DNGN_STONE_ARCH);

    if (stair_find == DNGN_ENTER_LABYRINTH
        || stair_find == DNGN_ENTER_PORTAL_VAULT)
    {
        _mark_portal_return_point(you.pos());
    }

    const int shaft_depth = (shaft ? shaft_level - you.absdepth0 : 1);
    _player_change_level_reset();
    _player_change_level_downstairs(stair_find, destination_override, shaft,
                                    shaft_level, shaft_dest);

    // When going downstairs into a special level, delete any previous
    // instances of it.
    if (!player_in_connected_branch() && !_is_portal_exit(stair_find))
    {
        // FIXME: this needs to be done only on exit
        std::string lname = level_id::current().describe();
        dprf("Deleting: %s", lname.c_str());
        you.save->delete_chunk(lname);
    }

    // Did we enter a new branch.
    const bool entered_branch(
        you.where_are_you != old_level.branch
        && branches[you.where_are_you].parent_branch == old_level.branch);

    if (stair_find == DNGN_EXIT_ABYSS || stair_find == DNGN_EXIT_PANDEMONIUM)
    {
        mpr("You pass through the gate.");
        if (!you.wizard || !crawl_state.is_replaying_keys())
            more();
    }
    
    if (stair_find == DNGN_EXIT_ABYSS)
    {
        //Make sure the player can't automatically find the key to hell on future
        //Abyss trips.
        env.can_find_hell_key = false;
    }

    if (!is_connected_branch(old_level.branch)
        && player_in_connected_branch()
        && old_level.branch != you.where_are_you)
    {
        mprf("Welcome back to %s!", branches[you.where_are_you].longname);
    }

    if (!you.airborne()
        && you.confused()
        && !feat_is_escape_hatch(stair_find)
        && force_stair != DNGN_ENTER_ABYSS
        && coinflip())
    {
        const char* fall_where = "down the stairs";
        if (!feat_is_staircase(stair_find))
            fall_where = "through the gate";

        mprf("In your confused state, you trip and fall %s.", fall_where);
        // Note that this only does damage; it doesn't cancel the level
        // transition.
        if (!feat_is_staircase(stair_find))
            ouch(1, NON_MONSTER, KILLED_BY_FALLING_THROUGH_GATE);
        else
            ouch(1, NON_MONSTER, KILLED_BY_FALLING_DOWN_STAIRS);
    }

    dungeon_feature_type stair_taken = stair_find;

    if (player_in_branch(BRANCH_ABYSS))
        stair_taken = DNGN_ENTER_ABYSS;

    if (shaft)
        stair_taken = DNGN_ESCAPE_HATCH_DOWN;

    switch (you.where_are_you)
    {
    case BRANCH_LABYRINTH:
        // XXX: Ideally, we want to hint at the wall rule (rock > metal),
        //      and that the walls can shift occasionally.
        // Are these too long?
        mpr("As you enter the labyrinth, previously moving walls settle noisily into place.");
        mpr("You hear the metallic echo of a distant snort before it fades into the rock.");
        mark_milestone("br.enter", "entered a Labyrinth.");
        break;

    case BRANCH_ABYSS:
        if (!force_stair)
            mpr("You enter the Abyss!");

        mpr("To return, you must find a gate leading back.");
        if (you.religion == GOD_CHEIBRIADOS)
        {
            mpr("You feel Cheibriados slowing down the madness of this place.",
                MSGCH_GOD, GOD_CHEIBRIADOS);
        }
        learned_something_new(HINT_ABYSS);
        break;

    case BRANCH_PANDEMONIUM:
        if (old_level.branch == BRANCH_PANDEMONIUM)
            mpr("You pass into a different region of Pandemonium.");
        else
        {
            mpr("You enter the halls of Pandemonium!");
            mpr("To return, you must find a gate leading back.");
        }
        break;

    default:
        if (shaft)
            handle_items_on_shaft(you.pos(), false);
        else
            _climb_message(stair_find, false, old_level.branch);
        break;
    }

    if (!shaft)
        _exit_stair_message(stair_find, false);

    if (entered_branch)
    {
        if (branches[you.where_are_you].entry_message)
            mpr(branches[you.where_are_you].entry_message);
        else
            mprf("Welcome to %s!", branches[you.where_are_you].longname);
            
        //Handle the just-entered-Tartarus stuff here.
        if(you.where_are_you == BRANCH_TARTARUS)
        {
            mpr("You shudder as the unholy atmosphere begins claiming your memories...");
            you.undrained_experience = you.experience;
            
            //This will almost never happen in practice.
            if(you.experience_level == 1)
                you.experience = 0;
            else
                you.experience = exp_needed(you.experience_level) - 1;
            level_change();
        }
        else if(you.where_are_you == BRANCH_COCYTUS)
        {
            mpr("You feel the numbing cold slowing your every movement!");
        }
        else if(you.where_are_you == BRANCH_GEHENNA)
        {
            mpr("You shove your potions and scrolls deeper in your knapsack against the searing heat.");
        }
        else if(you.where_are_you == BRANCH_DIS)
        {
            mpr("The metal walls around you radiate an unnatural silence, broken only by screams of agony.");
        }
    }

    if (stair_find == DNGN_ENTER_HELL)
    {
        mpr("Welcome to Hell!");
        mpr("Please enjoy your stay.");

        // Kill -more- prompt if we're traveling.
        if (!you.running && !force_stair)
            more();
    }

    const bool newlevel = load_level(stair_taken, LOAD_ENTER_LEVEL, old_level);

    _set_entry_cause(entry_cause, old_level.branch);
    entry_cause = you.entry_cause;

    if (newlevel)
    {
        // When entering a new level, reset friendly_pickup to default.
        you.friendly_pickup = Options.default_friendly_pickup;

        switch (you.where_are_you)
        {
        default:
            // Xom thinks it's funny if you enter a new level via shaft
            // or escape hatch, for shafts it's funnier the deeper you fell.
            if (shaft || feat_is_escape_hatch(stair_find))
                xom_is_stimulated(shaft_depth * 50);
            else if (!is_connected_branch(you.where_are_you))
                xom_is_stimulated(25);
            else
                xom_is_stimulated(10);
            break;

        case BRANCH_ZIGGURAT:
            // The best way to die currently.
            xom_is_stimulated(50);
            break;

        case BRANCH_LABYRINTH:
            // Finding the way out of a labyrinth interests Xom,
            // but less so for Minotaurs. (though not now, as they cannot
            // map the labyrinth any more {due})
            xom_is_stimulated(75);
            break;

        case BRANCH_PANDEMONIUM:
            xom_is_stimulated(100);
            break;

        case BRANCH_ABYSS:
            generate_random_blood_spatter_on_level();
            // The old code had a bizarre list where a risky act was value
            // _less_, etc.  Let's look just at these two cases:
            if (entry_cause == EC_SELF_EXPLICIT)
                xom_is_stimulated(100, XM_INTRIGUED);
            else
                xom_is_stimulated(200);
            break;
        }
    }

    switch (you.where_are_you)
    {
    case BRANCH_ABYSS:
        grd(you.pos()) = DNGN_FLOOR;

        init_pandemonium();     // colours only
        break;

    case BRANCH_PANDEMONIUM:
        init_pandemonium();

        for (int pc = random2avg(28, 3); pc > 0; pc--)
            pandemonium_mons();
        break;

    default:
        break;
    }

    you.turn_is_over = true;

    save_game_state();

    new_level();

    moveto_location_effects(old_feat);

    // Clear list of beholding monsters.
    you.clear_beholders();
    you.clear_fearmongers();

    if (!allow_control_teleport(true))
        mpr("You sense a powerful magical force warping space.", MSGCH_WARN);
    
    check_relatively_safe(false);

    trackers_init_new_level(true);

    // XXX: Using force_dest to decide whether to save stair info.
    //      Currently it's only used for Portal, where we don't
    //      want to mark the destination known.
    if (!force_dest)
        _update_travel_cache(collect_travel_data, old_level, stair_pos);

    env.map_shadow = env.map_knowledge;
    // Preventing obvious finding of stairs at your position.
    env.map_shadow(you.pos()).flags |= MAP_SEEN_FLAG;

    viewwindow();

    // Checking new squares for interesting features.
    if (!you.running)
        check_for_interesting_features();

    maybe_update_stashes();

    request_autopickup();

    // Zotdef: returning from portals (e.g. bazaar) paralyses the player in
    // place for 5 moves.  Nasty, but punishes players for using portals as
    // quick-healing stopovers.
    if (crawl_state.game_is_zotdef())
        start_delay(DELAY_UNINTERRUPTIBLE, 5);

    you.entry_coords = you.pos();
}

static bool _any_glowing_mold()
{
    for (rectangle_iterator ri(0); ri; ++ri)
        if (glowing_mold(*ri))
            return true;
    for (monster_iterator mon_it; mon_it; ++mon_it)
        if (mon_it->type == MONS_HYPERACTIVE_BALLISTOMYCETE)
            return true;

    return false;
}

static void _update_level_state()
{
    env.level_state = 0;

    std::vector<coord_def> golub = find_golubria_on_level();
    if (!golub.empty())
        env.level_state += LSTATE_GOLUBRIA;

    if (_any_glowing_mold())
        env.level_state += LSTATE_GLOW_MOLD;
}

void new_level(bool restore)
{
    print_stats_level();
#ifdef DGL_WHEREIS
    whereis_record();
#endif

    if (restore)
        return;

    _update_level_state();

    cancel_tornado();

    take_note(Note(NOTE_DUNGEON_LEVEL_CHANGE));

    if (player_in_branch(BRANCH_ZIGGURAT))
        you.zig_max = std::max(you.zig_max, player_branch_depth());
}

// Returns a hatch or stair (up or down)
dungeon_feature_type random_stair(bool do_place_check)
{
    if (do_place_check)
    {
        // Only place stairs in a direction that's actually applicable.
        if (at_branch_bottom())
        {
            return (static_cast<dungeon_feature_type>(
                DNGN_STONE_STAIRS_UP_I+random2(
                    DNGN_ESCAPE_HATCH_UP-DNGN_STONE_STAIRS_UP_I+1)));
        }
        else if (player_branch_depth() == 1)
        {
            return (static_cast<dungeon_feature_type>(
                DNGN_STONE_STAIRS_DOWN_I+random2(
                    DNGN_ESCAPE_HATCH_DOWN-DNGN_STONE_STAIRS_DOWN_I+1)));
        }
    }
    // else we can go either direction
    return (static_cast<dungeon_feature_type>(
        DNGN_STONE_STAIRS_DOWN_I+random2(
            DNGN_ESCAPE_HATCH_UP-DNGN_STONE_STAIRS_DOWN_I+1)));
}
