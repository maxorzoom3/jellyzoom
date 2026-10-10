#pragma once
#include <string>
/* What jf_gate.cpp keeps for the player: the signed-in session and each film's server id. */
extern "C" {
#include "jf_types.h"
}

// false until the worker has logged in
bool jf_gate_session(JfSession *out);
// the server's id for the item at this index of jf_gate's list, or "" when unknown
const char *jf_gate_item_id(int index);
// the id whose backdrop picture belongs to this title: its own, or its series' for an episode or a season
std::string jf_gate_backdrop_id(int index);
