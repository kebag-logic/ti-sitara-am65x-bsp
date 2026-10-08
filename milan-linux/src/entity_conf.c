// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// entity_conf.c - see entity_conf.h.

#define _GNU_SOURCE
#include "entity_conf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t entity_id_from_mac(const uint8_t mac[6])
{
	return ((uint64_t)mac[0] << 56) | ((uint64_t)mac[1] << 48) | ((uint64_t)mac[2] << 40) |
	       (0xFFull << 32) | (0xFEull << 24) | ((uint64_t)mac[3] << 16) | ((uint64_t)mac[4] << 8) | mac[5];
}

void entity_conf_finish(struct entity_conf *c, const uint8_t mac[6])
{
	c->entity.mac = ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) | ((uint64_t)mac[2] << 24) |
			((uint64_t)mac[3] << 16) | ((uint64_t)mac[4] << 8) | mac[5];
	if (!c->entity_id_pinned) {
		c->entity.entity_id = entity_id_from_mac(mac);
	}
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t') {
		++s;
	}
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) {
		*--e = '\0';
	}
	return s;
}

int entity_conf_load(struct entity_conf *c, const char *path)
{
	memset(c, 0, sizeof *c);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "entity: %s: %s\n", path, strerror(errno));
		return -1;
	}
	// the fields adp.h's struct adp_entity carries, as adp_entity.py names them
	unsigned seen = 0;
	char line[256];
	int lineno = 0;
	int rc = 0;
	while (rc == 0 && fgets(line, sizeof line, f) != NULL) {
		++lineno;
		char *hash = strchr(line, '#');
		if (hash != NULL) {
			*hash = '\0';
		}
		char *s = trim(line);
		if (*s == '\0') {
			continue;
		}
		char *eq = strchr(s, '=');
		if (eq == NULL) {
			fprintf(stderr, "entity: %s:%d: not key=value\n", path, lineno);
			rc = -1;
			break;
		}
		*eq = '\0';
		char *key = trim(s);
		char *val = trim(eq + 1);
		char *end;
		errno = 0;
		unsigned long long v = strtoull(val, &end, 0);
		if (errno != 0 || *val == '\0' || *end != '\0') {
			fprintf(stderr, "entity: %s:%d: %s: not a number\n", path, lineno, key);
			rc = -1;
			break;
		}
		struct adp_entity *e = &c->entity;
		if (strcmp(key, "entity_id") == 0) {
			e->entity_id = v;
			c->entity_id_pinned = true;
		} else if (strcmp(key, "entity_model_id") == 0) {
			e->entity_model_id = v;
			seen |= 1u;
		} else if (strcmp(key, "entity_capabilities") == 0) {
			e->entity_capabilities = (uint32_t)v;
			seen |= 2u;
		} else if (strcmp(key, "talker_stream_sources") == 0) {
			e->talker_stream_sources = (uint16_t)v;
			seen |= 4u;
		} else if (strcmp(key, "talker_capabilities") == 0) {
			e->talker_capabilities = (uint16_t)v;
			seen |= 8u;
		} else if (strcmp(key, "listener_stream_sinks") == 0) {
			e->listener_stream_sinks = (uint16_t)v;
			seen |= 16u;
		} else if (strcmp(key, "listener_capabilities") == 0) {
			e->listener_capabilities = (uint16_t)v;
			seen |= 32u;
		} else if (strcmp(key, "identify_control_index") == 0) {
			e->identify_control_index = (uint16_t)v;
			seen |= 64u;
		} else {
			fprintf(stderr, "entity: %s:%d: unknown key %s\n", path, lineno, key);
			rc = -1;
		}
	}
	fclose(f);
	if (rc == 0 && seen != 127u) {
		fprintf(stderr, "entity: %s: missing fields (have mask 0x%x of 0x7f)\n", path, seen);
		rc = -1;
	}
	return rc;
}
