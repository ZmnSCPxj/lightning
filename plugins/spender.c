#include "multiwithdraw.h"

static void spender_init(struct plugin *plugin,
			 const char *buf,
			 const jsmntok_t *config)
{
	/* Nil.  */
}

int main(int argc, char **argv)
{
	struct plugin_command *spender_commands;

	spender_commands = tal_arr(NULL, struct plugin_command, 0);

	tal_expand(&spender_commands,
		   multiwithdraw_commands, num_multiwithdraw_commands);

	setup_locale();

	plugin_main(argv, &spender_init, PLUGIN_STATIC, NULL,
		    spender_commands, tal_count(spender_commands),
		    NULL, 0, NULL, 0, NULL);

	tal_free(spender_commands);
}
