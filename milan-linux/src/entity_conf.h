// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// entity_conf.h - the entity's ADP fields from /etc/milan/entity.conf.
//
// The file is generated from an end-station config by milan-fpga's own
// adp_entity.py (tools/gen-entity-conf.sh), so the PB2 advertises the shape the
// builder derives, never a restated copy. The entity's MAC is the interface's
// at run time, and its entity_id is that MAC's EUI-64 expansion unless the
// file pins one, the builder's "mac-derived" rule.

#ifndef MILAN_ENTITY_CONF_H
#define MILAN_ENTITY_CONF_H

#include <stdbool.h>
#include <stdint.h>

#include "adp.h"

struct entity_conf {
	struct adp_entity entity;       // mac and entity_id filled by entity_conf_finish
	bool entity_id_pinned;
};

// Parse key=value lines (# comments, numbers in C syntax). 0, or -1 with a
// message on stderr naming the line.
int entity_conf_load(struct entity_conf *c, const char *path);

// The EUI-48 to EUI-64 expansion (FF-FE at the OUI boundary).
uint64_t entity_id_from_mac(const uint8_t mac[6]);

// Set the runtime identity from the interface's MAC.
void entity_conf_finish(struct entity_conf *c, const uint8_t mac[6]);

#endif // MILAN_ENTITY_CONF_H
