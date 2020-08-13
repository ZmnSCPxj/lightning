#ifndef LIGHTNING_PLUGINS_ROUTETRICKS_PERMUTEROUTE_H
#define LIGHTNING_PLUGINS_ROUTETRICKS_PERMUTEROUTE_H
#include "config.h"

#include <plugins/libplugin.h>

extern const struct plugin_command permuteroute_commands[];
extern const size_t num_permuteroute_commands;

extern
void permuteroute_init(struct plugin *p,
		       const char *buf, const jsmntok_t *toks);

#endif /* LIGHTNING_PLUGINS_ROUTETRICKS_PERMUTEROUTE_H */
