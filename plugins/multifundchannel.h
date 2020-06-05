#ifndef LIGHTNING_PLUGINS_MULTIFUNDCHANNEL_H
#define LIGHTNING_PLUGINS_MULTIFUNDCHANNEL_H
#include "config.h"

#include <plugins/libplugin.h>

/*
Why does this file even exist?

Because in the future we might want to create a single
plugin binary that includes all these implementations
of commands.

In that case, the unified plugin can just define a
`main` function that allocates its own array of 
`struct plugin_command` and copies the commands from
each of the individual plugin source files,
and provide an `init` function that just calls each
of the individual plugin init functions.
*/

extern const struct plugin_command multifundchannel_commands[];
extern const unsigned int num_multifundchannel_commands;
void multifundchannel_init(struct plugin *plugin, const char *buf, const jsmntok_t *cfg);

#endif /* LIGHTNING_PLUGINS_MULTIFUNDCHANNEL_H */
