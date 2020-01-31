#include "multiconnect.h"
#include <ccan/array_size/array_size.h>
#include <ccan/compiler/compiler.h>
#include <ccan/json_out/json_out.h>
#include <common/json.h>
#include <common/json_tok.h>
#include <common/jsonrpc_errors.h>
#include <common/utils.h>
#include <plugins/libplugin_spark.h>

struct connect_single_spark {
	/* The id for this connection attempt.  */
	const char *id;
	/* The features for this connection attempt.  */
	const char *features;

	/* The completion of this spark.  */
	struct plugin_spark_completion *completion;
};
struct connect_multi {
	/* The sparks.  */
	struct plugin_spark **sparks;
	/* The individual connect_single commands.  */
	struct connect_single_spark *subcommands;
};

static struct command_result *
multiconnect_done(struct command *cmd,
		  struct connect_multi *cm)
{
	size_t i;
	struct json_out *out;

	out = json_out_new(NULL);
	json_out_start(out, NULL, '{');

	json_out_start(out, "id", '[');
	for (i = 0; i < tal_count(cm->subcommands); ++i) {
		json_out_add(out, NULL, true, "%s", cm->subcommands[i].id);
	}
	json_out_end(out, ']');

	json_out_start(out, "features", '[');
	for (i = 0; i < tal_count(cm->subcommands); ++i) {
		json_out_add(out, NULL, true, "%s", cm->subcommands[i].features);
	}
	json_out_end(out, ']');

	json_out_end(out, '}');

	return command_success(cmd, out);
}

static struct command_result *
connect_single_spark_done(struct command *cmd,
			  const char *buf,
			  const jsmntok_t *result,
			  struct connect_single_spark *css)
{
	const jsmntok_t *idtok;
	const jsmntok_t *featurestok;

	/* Retrieve the `id` from the base command.
	 * This automatically removes any @host:port
	 * for this connection.  */
	idtok = json_get_member(buf, result, "id");
	if (!idtok)
		plugin_err(cmd->plugin, "'connect' missing 'id' field");

	/* Replace whatever string was originally in here.  */
	css->id = tal_free(css->id);
	css->id = json_strdup(cmd, buf, idtok);

	/* Retrieve the `features` from the base command.  */
	featurestok = json_get_member(buf, result, "features");
	if (!featurestok)
		plugin_err(cmd->plugin, "'connect' missing 'features' field");
	css->features = json_strdup(cmd, buf, featurestok);

	return plugin_spark_complete(cmd, css->completion);
}
static struct command_result *
connect_single_spark_start(struct command *cmd,
			   struct plugin_spark_completion *completion,
			   struct connect_single_spark *css)
{
	struct out_req *req;

	css->completion = completion;

	req = jsonrpc_request_start(cmd->plugin, cmd, "connect",
				    &connect_single_spark_done,
				    &forward_error,
				    css);
	json_add_string(req->js, "id", css->id);
	return send_outreq(cmd->plugin, req);
}

static struct command_result *multiconnect(struct command *cmd,
					   const char *buf,
					   const jsmntok_t *ids)
{
	size_t i;
	const jsmntok_t *t;
	struct connect_multi *cm;
	size_t num_sparks;

	num_sparks = ids->size;

	cm = tal(cmd, struct connect_multi);
	cm->sparks = tal_arr(cm, struct plugin_spark *, num_sparks);
	cm->subcommands = tal_arr(cm, struct connect_single_spark, num_sparks);

	/* We know at this point that ids is an array of strings.  */
	json_for_each_arr(i, t, ids) {
		cm->subcommands[i].id = json_strdup(cm, buf, t);
		cm->sparks[i] = plugin_start_spark(cmd,
						   &connect_single_spark_start,
						   &cm->subcommands[i]);
	}

	return plugin_wait_all_sparks(cmd, num_sparks, cm->sparks,
				      &multiconnect_done, cm);
}

static struct command_result *json_connect(struct command *cmd,
					   const char *buf,
					   const jsmntok_t *params)
{
	const jsmntok_t *idtok;
	const char *host;
	u32 *port;
	struct out_req *req;

	if (!param(cmd, buf, params,
		   p_req("id", param_tok, &idtok),
		   p_opt("host", param_string, &host),
		   p_opt("port", param_number, &port),
		   NULL))
		return command_param_failed();

	/* Is id an array?  If so use multi-connect.  */
	if (idtok->type == JSMN_ARRAY) {
		size_t i;
		const jsmntok_t *t;

		if (host)
			return command_done_err(cmd, JSONRPC2_INVALID_PARAMS,
						"Cannot specify parameter "
						"'host' when 'id' parameter "
						"is an array.",
						NULL);
		if (port)
			return command_done_err(cmd, JSONRPC2_INVALID_PARAMS,
						"Cannot specify parameter "
						"'port' when 'id' parameter "
						"is an array.",
						NULL);

		if (idtok->size == 0)
			return command_done_err(cmd, JSONRPC2_INVALID_PARAMS,
						"Empty 'id' array: "
						"nothing to connect.",
						NULL);

		json_for_each_arr(i, t, idtok) {
			if (t->type != JSMN_STRING)
				return command_done_err(cmd,
							JSONRPC2_INVALID_PARAMS,
							"All items in 'id' "
							"array "
							"must be strings.",
							NULL);
		}

		return multiconnect(cmd, buf, idtok);
	} else if (idtok->type != JSMN_STRING)
		return command_done_err(cmd, JSONRPC2_INVALID_PARAMS,
					"'id' must be either a string or an "
					"array of strings.",
					NULL);

	/* If ID is string, just forward to connect_single command.  */
	req = jsonrpc_request_start(cmd->plugin, cmd, "connect",
				    &forward_result, &forward_error,
				    NULL);
	json_add_string(req->js, "id", json_strdup(tmpctx, buf, idtok));
	if (host)
		json_add_string(req->js, "host", host);
	if (port)
		json_add_u32(req->js, "port", *port);
	return send_outreq(cmd->plugin, req);
}

const struct plugin_command connect_commands[] = {
	{
		"multiconnect",
		"network",
		"Connect to {id} at {host} (which can end in ':port' if not default). "
		"{id} can also be of the form id@host[:port].",
		"Alternately, {id} can be an array of strings of the form id[@host[:port]] "
		"to connect to multiple peers simultaneously "
		"(and you should not specify {host} or {port}).",
		json_connect
	}
};
const unsigned int num_connect_commands = ARRAY_SIZE(connect_commands);

void connect_init(struct plugin *plugin UNUSED,
		  const char *buf UNUSED, const jsmntok_t *config UNUSED)
{
	/* Do nothing.  */
}

int main(int argc, char **argv)
{
	setup_locale();
	plugin_main(argv,
		    &connect_init, PLUGIN_RESTARTABLE,
		    NULL,
		    connect_commands, num_connect_commands,
		    NULL, 0, NULL, 0, NULL);
}
