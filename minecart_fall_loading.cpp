/*
This is a plugin which allows items that fall on top of minecarts to be loaded into the minecart.
*/

#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "VTableInterpose.h"
#include "MiscUtils.h"

#include <vector>
#include <cmath>
#include <fstream>

#include "df/world.h"
#include "df/vehicle.h"
#include "df/map_block.h"
#include "df/general_ref.h"
#include "df/general_ref_contains_itemst.h"
#include "df/general_ref_contained_in_itemst.h"
#include "df/general_ref_projectile.h"
#include "df/item.h"
#include "df/item_toolst.h"
#include "df/itemdef_toolst.h"

#include "modules/MapCache.h"
#include "modules/Items.h"

using namespace DFHack;



// DFHack plugin preamble

DFHACK_PLUGIN("minecart_fall_loading");
DFHACK_PLUGIN_IS_ENABLED(active);
REQUIRE_GLOBAL(world);



// Debugging macros

//#define _DEBUG__MINECART_FALL_LOADING_

#ifdef _DEBUG__MINECART_FALL_LOADING_
	// logfile appears in root folder of DF directory
	std::ofstream logfile("minecart_fall_loading_log.txt");

	// DEBUG_PRINTLN: print the line of text on a line to the logfile
	#define DEBUG_PRINTLN(text)      ; logfile << (text) << std::endl;
	// DEBUG_PRINTLN_EXPR: print the text of an expression followed by its value on a line to the logfile
	#define DEBUG_PRINTLN_EXPR(expr) ; logfile << (#expr) << ": " << (expr) << std::endl;
#else
	// null the debug macros if debugging turned off
	#define DEBUG_PRINTLN(text)      ;
    #define DEBUG_PRINTLN_EXPR(expr) ;
#endif



// Convenience functions for printing things to text output streams; primarily for debugging

std::ostream& operator<<(std::ostream& o_st, df::coord pos) {
	// Print df::coord <pos> to text output stream <o_st>
	o_st << "(" << pos.x << ", " << pos.y << ", " << pos.z << ")";
	return o_st;
}

template <typename... targs>
std::ostream& operator<<(std::ostream& o_st, const std::set<targs...>& s) {
	// Print the contents of set <s> to text output stream <o_st> a la Python format
	o_st << "{";
	int i = 0;
	for (std::set<targs...>::const_iterator iter = s.cbegin(); iter != s.cend(); ++iter, ++i) {
		o_st << *iter;
		if (i != s.size() - 1) {
			o_st << ", ";
		}
	}
	o_st << "}";
	return o_st;
}



struct minecart_info {
	// struct containing info about a minecart for the purposes of this plugin
	
	df::vehicle* minecart;
	df::item* minecart_item;
	
	// pos: last recorded position
	df::coord pos;
	// next_pos: last recorded predicted next position
	df::coord next_pos; 
	
	// above_pos: last recorded set of items in tile above this->pos
	std::set<df::item*> above_pos;
	// above_next_pos: last recorded set of items in tile above this->next_pos
	std::set<df::item*> above_next_pos;
};

typedef df::vehicle::key_field_type minecart_id_t;

// minecarts: the info records for all minecarts being tracked
std::map<minecart_id_t, minecart_info*> minecarts;
typedef decltype(minecarts)::iterator minecarts_iter_t;

df::map_block* get_map_block(df::coord pos) {
	// get the map_block in which coord <pos> is located
	return world->map.block_index[pos.x/16][pos.y/16][pos.z];
}

std::set<df::item*> get_items_at(df::coord pos) {
	// get the set of items at coord <pos>	
	std::set<df::item*> out;
	
	// loop through all items
	// TODO: more efficient implementation?
	// NOTE: item is not necessarily recorded in corresponding map_block
	for (df::item* item : world->items.all) {
		if (Items::getPosition(item) == pos) {
			out.insert(item);
		}
	}
	return out;
}

int32_t get_item_load_capacity(df::item* item) {
	// get the container capacity of item <item>
	DEBUG_PRINTLN_EXPR(item->_identity.getFullName());
	
	// only df::item_toolst instances have an associated container capacity, I think
	df::item_toolst* item_as_tool = virtual_cast<df::item_toolst>(item);
	// if item is not a df::item_toolst
	if (item_as_tool == nullptr) {
		// item is not a container, so return 0
		return 0;
	}
	df::itemdef_toolst* tool_def = item_as_tool->subtype;
	return tool_def->container_capacity;
}

int32_t get_item_loaded_volume(df::item* item) {
	// get the total volume of all objects inside item <item>
	std::vector<df::item*> contained_items;
	Items::getContainedItems(item, &contained_items);
	int32_t out = 0;
	for (df::item* item : contained_items) {
		out += item->getVolume();
	}
	return out;
}

bool can_item_fit(df::item* container, df::item* check_fit) {
	// returns whether item <check_fit> can go inside item <container> without exceeding <container>'s capacity
	int32_t load_capacity = get_item_load_capacity(container);
	int32_t loaded_volume = get_item_loaded_volume(container);
	int32_t check_fit_volume = check_fit->getVolume();
	DEBUG_PRINTLN_EXPR(load_capacity);
	DEBUG_PRINTLN_EXPR(loaded_volume);
	DEBUG_PRINTLN_EXPR(check_fit_volume);
	return loaded_volume + check_fit_volume <= load_capacity;
}

void make_not_projectile(df::item* item) {
	// makes item <item>, which must currently be a projectile, into not a projectile and puts it on the ground
	DEBUG_PRINTLN("make_not_projectile");
	DEBUG_PRINTLN_EXPR(item->id);
	
	// proj_ref: the stored general_ref of item to a df::projectile object
	df::general_ref_projectile* proj_ref;
	// proj_ref_index: the index of proj_ref in item's container of general_refs
	unsigned int proj_ref_index;
	
	for (unsigned int i = 0; i != item->general_refs.size(); ++i) {
		df::general_ref* ref = item->general_refs[i];
		if (ref->getType() == df::general_ref_type::PROJECTILE) {
			proj_ref = (df::general_ref_projectile*)ref;
			proj_ref_index = i;
			break;
		}
	}
	
	DEBUG_PRINTLN_EXPR(proj_ref);
	DEBUG_PRINTLN_EXPR(proj_ref_index);
	
	// proj_id: the id of item's associated projectile object
	int32_t proj_id = proj_ref->projectile_id;
	DEBUG_PRINTLN_EXPR(proj_id);
	// proj: item's associated projectile object
	df::projectile* proj;
	// link: the linked list link which holds proj
	df::proj_list_link* link = &world->proj_list;
	DEBUG_PRINTLN_EXPR(link);
	DEBUG_PRINTLN_EXPR(link->item);
	DEBUG_PRINTLN_EXPR(link->prev);
	DEBUG_PRINTLN_EXPR(link->next);
	
	// linear search for proj
	while (link->item == nullptr || link->item->id != proj_id) {
		DEBUG_PRINTLN_EXPR(link->item);
		//DEBUG_PRINTLN_EXPR(link->item->id);
		link = link->next;
	}
	
	DEBUG_PRINTLN("finished looking for link");
	
	proj = link->item;
	
	// cut link out of the linked list of which it is part, essentially removing proj from the list of projectiles
	if (link->prev != nullptr) {
		link->prev->next = link->next;
	}
	if (link->next != nullptr) {
		link->next->prev = link->prev;
	}
	
	DEBUG_PRINTLN("finished relinking linked list");
	
	delete link;
	
	DEBUG_PRINTLN("finished deleting link");
	
	// delete proj as void* to avoid calling destructor, which destructor seems to cause a crash
	// TODO: possibly bad? fix?
	delete (void*)proj;
	
	DEBUG_PRINTLN("finished deleting proj");
	
	// erase general_ref to projectile object from vector of general_refs
	vector_erase_at(item->general_refs, proj_ref_index);
	
	DEBUG_PRINTLN("finished erasing proj_ref");
	
	item->flags.bits.on_ground = true;
	
	DEBUG_PRINTLN("finished setting on_ground flag to true");
	
	MapExtras::MapCache mc;
	
	DEBUG_PRINTLN("finished creating MapCache object");
	
	mc.addItemOnGround(item);
	
	DEBUG_PRINTLN("finished adding item on ground");
}

void load_minecart_with_item(df::vehicle* minecart, df::item* item) {
	// load minecart <minecart> with item <item>
	DEBUG_PRINTLN("load_minecart_with_item");
	
	// shelved_refs: general_refs of <item> that are forbidden by Items::moveToContainer
	// these are removed before the call to Items::moveToContainer and restored after
	// map: index of general_ref -> general_ref itself
	std::map<size_t, df::general_ref*> shelved_refs;
	
	bool is_projectile = false;
	
	for (size_t i = 0; i != item->general_refs.size(); ++i) {
		df::general_ref* ref = item->general_refs[i];
		
        switch (ref->getType())
        {
			case general_ref_type::PROJECTILE:
				// general_refs of PROJECTILE type are not shelved
				is_projectile = true;
				break;
			case general_ref_type::BUILDING_HOLDER:
			case general_ref_type::BUILDING_CAGED:
			case general_ref_type::BUILDING_TRIGGER:
			case general_ref_type::BUILDING_TRIGGERTARGET:
			case general_ref_type::BUILDING_CIVZONE_ASSIGNED:
				shelved_refs.insert(std::make_pair(i, ref));
				break;
			default:
				break;
        }
    }
	
	DEBUG_PRINTLN("finished shelvign general_refs");
	DEBUG_PRINTLN_EXPR(is_projectile);
	
	// if item is a projectile, make it not a projectile and put it on the ground
	// happens in the majority of cases where an item falls from above
	// this is required to move it into a container, like a minecart
	if (is_projectile) {
		make_not_projectile(item);
	}
	
	for (auto pr : shelved_refs) {
		DEBUG_PRINTLN(pr.first);
		DEBUG_PRINTLN(pr.second->getType());
	}
	
	DEBUG_PRINTLN("finished printing shelved_refs");
	
	for (auto pr : shelved_refs) {
		vector_erase_at(item->general_refs, pr.first);
	}
	
	DEBUG_PRINTLN("finished removing shelved_refs");
	
	MapExtras::MapCache mc;
	bool did_succeed = Items::moveToContainer(mc, item, df::item::find(minecart->item_id));
	DEBUG_PRINTLN_EXPR(did_succeed);
	
	// restoration of shelved_refs
	for (auto pr : shelved_refs) {
		item->general_refs.push_back(pr.second);
	}
	
	DEBUG_PRINTLN("finished putting back shelved_refs");
}

int div_floor(int dividend, int divisor) {
	// returns floor(dividend/divisor)
	// (true floor, not rounding towards zero)
	return (dividend >= 0) ? (dividend / divisor) : (dividend / divisor - 1);
}

df::coord get_next_pos(df::vehicle* minecart, df::coord current_pos) {
	// returns the predicted next position of minecart <minecart> (in one tick)
	// <current_pos> is current position of the minecart
	return current_pos + df::coord(
		div_floor(minecart->offset_x + minecart->speed_x + 50000, 100000),
		div_floor(minecart->offset_y + minecart->speed_y + 50000, 100000),
		div_floor(minecart->offset_z + minecart->speed_z + 50000, 100000)
	);
}

minecart_info* create_new_minecart_info(df::vehicle* v) {
	// returns a pointer to a new info record for minecart <v>
	// will be properly initialized later in the update, during the call to update_minecart_info
	minecart_info* out = new minecart_info;
	out->minecart = v;
	out->minecart_item = df::item::find(v->item_id);
	out->pos = df::coord();
	out->next_pos = df::coord();
	out->above_pos = {};
	out->above_next_pos = {};
	return out;
}



// Main three update functions:
// * update_minecart_list
// * perform_minecart_loading
// * update_minecart_info

void update_minecart_list() {
	// updates the list of currently tracked minecarts:
	// * removes no longer existing minecarts
	// * begins tracking new, previously untracked minecarts
	DEBUG_PRINTLN("update_minecart_list");
	
	std::vector<minecarts_iter_t> to_remove;
	for (auto iter = minecarts.begin(); iter != minecarts.end(); ++iter) {
		if (df::vehicle::find(iter->first) == nullptr) {
			to_remove.push_back(iter);
		}
	}
	
	DEBUG_PRINTLN_EXPR(to_remove.size());
	
	for (auto iter : to_remove) {
		delete iter->second;
		minecarts.erase(iter);
	}
	
	unsigned int num_inserted = 0;
	for (df::vehicle* v : world->vehicles.all) {
		auto pr = minecarts.insert(std::make_pair(
			v->id,
			create_new_minecart_info(v)
		));
		if (pr.second) {
			++num_inserted;
		}
	}
	
	DEBUG_PRINTLN_EXPR(num_inserted);
}

void perform_minecart_loading() {
	// loads any items that should be loaded into minecarts because:
	// * they have fallen from above
	// * they fit in the minecart
	DEBUG_PRINTLN("perform_minecart_loading");
	
	for (auto pr : minecarts) {
		minecart_id_t id = pr.first;
		minecart_info* info = pr.second;
		
		DEBUG_PRINTLN("LOOP");
		DEBUG_PRINTLN_EXPR(id);
		
		df::vehicle* minecart = info->minecart;
		df::item* minecart_item = info->minecart_item;      
		df::coord current_pos = Items::getPosition(minecart_item);
		
		DEBUG_PRINTLN_EXPR(current_pos);
		
		// above_set: the set of items that was *last recorded* being above the minecart's *current* position
		// this is above_pos if the minecart hasn't moved since the last update, and above_next_pos if it has moved
		std::set<df::item*>& above_set = (info->pos == current_pos) ? info->above_pos : info->above_next_pos;
		
		DEBUG_PRINTLN_EXPR(above_set);
		
		if (above_set.size() != 0) {
			DEBUG_PRINTLN("perform_minecart_loading: minecart INTEREST 1");
		}
		
		for (df::item* item : above_set) {
			DEBUG_PRINTLN_EXPR(item->id);
			// if item has moved onto the minecart's current position since the last update
			if (Items::getPosition(item) == current_pos) {
				DEBUG_PRINTLN("item fell onto minecart");
				// if the item can fit in the minecart
				if (can_item_fit(minecart_item, item)) {
					DEBUG_PRINTLN("item can fit");
					DEBUG_PRINTLN("item to be loaded");
					// load the minecart with the item
					load_minecart_with_item(minecart, item);
				}
			}
		}
	}
}

void update_minecart_info() {
	// updates the info recorded for each minecart being tracked
	DEBUG_PRINTLN("update_minecart_info");
	
	for (auto pr : minecarts) {
		minecart_id_t id = pr.first;
		minecart_info* info = pr.second;
		
		DEBUG_PRINTLN_EXPR(id);
		
		df::vehicle* minecart = df::vehicle::find(id);
		df::item* minecart_item = df::item::find(minecart->item_id);
		df::coord current_pos = Items::getPosition(minecart_item);
		
		DEBUG_PRINTLN_EXPR(current_pos);
		
		info->pos = current_pos;
		info->next_pos = get_next_pos(minecart, current_pos);
		
		DEBUG_PRINTLN_EXPR(info->next_pos);
		
		info->above_pos = get_items_at(info->pos + df::coord(0, 0, 1));
		DEBUG_PRINTLN_EXPR(info->above_pos);
		info->above_next_pos = get_items_at(info->next_pos + df::coord(0, 0, 1));
		DEBUG_PRINTLN_EXPR(info->above_next_pos);
		
		if (info->above_pos.size() != 0) {
			DEBUG_PRINTLN("info->above_pos nonempty");
		}
		if (info->above_next_pos.size() != 0) {
			DEBUG_PRINTLN("info->above_next_pos nonempty");
		}
	}
}

// counter: counter to next update
unsigned int counter;

DFhackCExport command_result plugin_init(color_ostream& out, std::vector<PluginCommand>& commands) {
	DEBUG_PRINTLN("plugin_init");
	CoreSuspender suspend;
	counter = 1;
	// not active until world is loaded
	active = false;
	DEBUG_PRINTLN("plugin_init: counter = 0;");
	return CR_OK;
}

DFhackCExport command_result plugin_onupdate(color_ostream& out) {
	if (!active) {
		return CR_OK;
	}
	
	DEBUG_PRINTLN("plugin_onupdate");
	
	CoreSuspender suspend;
	
	// TICKS: number of ticks between updates when active
	static const unsigned int TICKS = 1;
	
	// if the counter has loop around to zero
	if (counter == 0) {
		DEBUG_PRINTLN("plugin_onupdate: counter == 0");
		update_minecart_list();
		perform_minecart_loading();
		update_minecart_info();
	}
	
	// update counter
	++counter;
	counter %= TICKS;
	
	DEBUG_PRINTLN("plugin_onupdate: update counter");
	
	return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream& out) {
	DEBUG_PRINTLN("plugin_shutdown");
	
	CoreSuspender suspend;
	
	active = false;
	return CR_OK;
}

DFhackCExport command_result plugin_onstatechange(color_ostream& out, state_change_event event) {
	DEBUG_PRINTLN("plugin_onstatechange");
	
	switch (event) {
		case SC_MAP_LOADED:
			// world is loaded
			// become active
			active = true;
			break;
		case SC_MAP_UNLOADED:
			// world is unloaded
			// become inactive
			active = false;
			break;
	}
	
	return CR_OK;
}
