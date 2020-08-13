#include "routetricks.h"

static
void routetricks_init(struct plugin *p, const char *b, const jsmntok_t *t)
{
	permuteroute_init(p, b, t);
}

int main(int argc, char **argv)
{
	char *rootctx = tal(NULL, char);
	struct plugin_command *commands;

	commands = tal_arr(rootctx, struct plugin_command, 0);
	tal_expand(&commands,
		   permuteroute_commands, num_permuteroute_commands);

	plugin_main(argv,
		    &routetricks_init,
		    PLUGIN_RESTARTABLE, true, NULL,
		    commands, tal_count(commands),
		    NULL, 0,
		    NULL, 0,
		    NULL);

	tal_free(rootctx);
	return 0;
}
