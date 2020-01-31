#ifndef LIGHTNING_PLUGINS_MULTICONNECT_H
#define LIGHTNING_PLUGINS_MULTICONNECT_H
#include "config.h"

#include <plugins/libplugin.h>

extern const struct plugin_command connect_commands[];
extern const unsigned int num_connect_commands;
void connect_init(struct plugin *plugin, const char *buf, const jsmntok_t *cfg);

#endif /* LIGHTNING_PLUGINS_MULTICONNECT_H */

