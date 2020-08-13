#include "permuteroute.h"
#include <bitcoin/short_channel_id.h>
#include <ccan/array_size/array_size.h>
#include <ccan/tal/str/str.h>
#include <common/features.h>
#include <common/jsonrpc_errors.h>
#include <common/json_stream.h>
#include <common/type_to_string.h>

/*-----------------------------------------------------------------------------
Entry Points
-----------------------------------------------------------------------------*/

static struct node_id my_id;

void permuteroute_init(struct plugin *p,
		       const char *buf, const jsmntok_t *toks)
{
	const char *id;
	/* Get our id.  */
	id = rpc_delve(tmpctx, p, "getinfo",
		       take(json_out_obj(NULL, NULL, NULL)), ".id");
	if (!node_id_from_hexstr(id, strlen(id), &my_id))
		plugin_err(p, "getinfo didn't contain valid id: '%s'", id);
}

static struct command_result *json_permuteroute(struct command *cmd,
						const char *buf,
						const jsmntok_t *params);

const struct plugin_command permuteroute_commands[] =
{
	{
		"permuteroute",
		"channels",
		"Modify a failing route into a hopefully-complete one.",
		"Modify a failing route.",
		&json_permuteroute
	}
};
const size_t num_permuteroute_commands = ARRAY_SIZE(permuteroute_commands);

/*-----------------------------------------------------------------------------
permuteroute Data
-----------------------------------------------------------------------------*/

struct permuteroute_command {
	/* The original command.  */
	struct command *cmd;
	/* The command ID.  */
	u64 id;

	/* The original route.  */
	struct route_hop *route;
	/* The failing index.  */
	size_t erring_index;
	/* Was it a node-level failure?  */
	bool nodefailure;
	/* What is the starting node?  */
	struct node_id startnode;
	/* Nodes that were excluded.  */
	struct node_id *exclude_nodes;
	/* Channels that were excluded.  */
	struct short_channel_id_dir *exclude_chans;

	/* What was the erring channel/node?  */
	const char *erring_thing;

	/* The index after the splice.  */
	size_t dest_index;
	/* The node after the splice.  */
	struct node_id dest_node;
	/* The amount to be delivered to dest_node.  */
	struct amount_msat dest_amount;
	/* The CLTV delay at dest_node.  */
	u32 dest_delay;
	/* The style of dest_node.  */
	enum route_hop_style dest_style;

	/* The index before the splice.  */
	size_t source_index;
	/* The node before the splice.  */
	struct node_id source_node;
	/* The amount originally sent by source_node.  */
	struct amount_msat source_out_amount;

	/* Channels of the node before the splice.  */
	struct permuteroute_channel_data *source_channels;

	/* The route to splice.  */
	struct route_hop *splice_route;
	/* The amount and delay to be delivered to the source
	 * after the splice is inserted.  */
	struct amount_msat prefix_amount;
	u32 prefix_delay;

	/* Command we are currently executing.  */
	const char *last_command;
};

/* Data we need from listchannels.  */
struct permuteroute_channel_data {
	struct node_id source;
	struct node_id destination;

	struct short_channel_id scid;
	/* Need to infer this from source and destination id.  */
	int direction;

	bool active;

	struct amount_msat base_fee;
	u32 fee_per_millionth;
	u32 delay;

	struct amount_msat htlc_minimum_msat;
	struct amount_msat htlc_maximum_msat;
};

/*-----------------------------------------------------------------------------
Parameter validation
-----------------------------------------------------------------------------*/

static struct command_result *param_route(struct command *cmd,
					  const char *field,
					  const char *buf,
					  const jsmntok_t *tok,
					  struct route_hop **route)
{
	*route = json_to_route(cmd, buf, tok);
	if (!(*route))
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "'%s' failed to parse: '%.*s'",
				    field,
				    json_tok_full_len(tok),
				    json_tok_full(buf, tok));
	return NULL;
}

static bool json_to_exclusions(const tal_t *ctx,
			       const char *buf, const jsmntok_t *tok,
			       struct node_id **exclude_nodes,
			       struct short_channel_id_dir **exclude_chans)
{
	size_t i;
	const jsmntok_t *exc;
	if (!tok || tok->type != JSMN_ARRAY)
		return false;

	*exclude_nodes = tal_arr(ctx, struct node_id, 0);
	*exclude_chans = tal_arr(ctx, struct short_channel_id_dir, 0);

	json_for_each_arr (i, exc, tok) {
		struct node_id node;
		struct short_channel_id_dir chan;
		if (json_to_node_id(buf, exc, &node))
			tal_arr_expand(exclude_nodes, node);
		else if (short_channel_id_dir_from_str(buf + exc->start,
						       exc->end - exc->start,
						       &chan))
			tal_arr_expand(exclude_chans, chan);
		else
			return false;
	}
	return true;
}

static struct command_result *prc_start(struct permuteroute_command *prc);

static struct command_result *json_permuteroute(struct command *cmd,
						const char *buf,
						const jsmntok_t *params)
{
	struct permuteroute_command *prc;
	unsigned int *erring_index;
	bool *nodefailure;
	struct node_id *startnode;
	const jsmntok_t *excludetok;

	assert(cmd);

	prc = tal(cmd, struct permuteroute_command);
	prc->cmd = cmd;

	if (!param(cmd, buf, params,
		   p_req("route", param_route, &prc->route),
		   p_req("erring_index", param_number, &erring_index),
		   p_req("nodefailure", param_bool, &nodefailure),
		   p_opt_def("source", param_node_id, &startnode, my_id),
		   p_opt("exclude", param_array, &excludetok),
		   NULL))
		return command_param_failed();

	assert(cmd->id);
	prc->id = *cmd->id;

	prc->erring_index = *erring_index;
	prc->nodefailure = *nodefailure;
	prc->startnode = *startnode;

	if (!excludetok) {
		prc->exclude_nodes = tal_arr(prc, struct node_id, 0);
		prc->exclude_chans = tal_arr(prc, struct short_channel_id_dir,
					     0);
	} else if (!json_to_exclusions(cmd, buf, excludetok,
				       &prc->exclude_nodes,
				       &prc->exclude_chans))
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "'excludes' could not be parsed:"
				    "'%.*s'",
				    json_tok_full_len(excludetok),
				    json_tok_full(buf, excludetok));

	if (tal_count(prc->route) == 0)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "'route' cannot be empty.");
	if (prc->nodefailure && prc->erring_index == 0)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "'erring_index' cannot be 0 for "
				    "node failures.");
	if (prc->erring_index >= tal_count(prc->route))
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "'erring_index' cannot exceed "
				    "'route' length.");

	/* Extract information.  */
	if (prc->nodefailure) {
		prc->source_index = prc->erring_index - 1;
		prc->dest_index = prc->erring_index + 1;
		prc->erring_thing = type_to_string(prc, struct node_id,
						   &prc->route[prc->erring_index - 1].nodeid);
	} else {
		prc->source_index = prc->erring_index;
		prc->dest_index = prc->erring_index + 1;
		prc->erring_thing = type_to_string(prc, struct short_channel_id,
						   &prc->route[prc->erring_index].channel_id);
	}

	assert(prc->dest_index != 0);

	if (prc->source_index == 0)
		prc->source_node = *startnode;
	else
		prc->source_node = prc->route[prc->source_index - 1].nodeid;
	prc->source_out_amount = prc->route[prc->source_index].amount;

	prc->dest_node = prc->route[prc->dest_index - 1].nodeid;
	prc->dest_amount = prc->route[prc->dest_index - 1].amount;
	prc->dest_delay = prc->route[prc->dest_index - 1].delay;
	prc->dest_style = prc->route[prc->dest_index - 1].style;

	/* Exclude the nodes on the route, including the start.
	 * Exclusion really only works on the node being inserted,
	 * they do not apply "retroactively" to the existing nodes
	 * on the route.
	 * This exclusion is added simply to prevent `permuteroute`
	 * from generating loops in the resulting route, as loops
	 * do not help reliability nor privacy but end up costing
	 * more.
	 */
	for (size_t i = 0; i < tal_count(prc->route); ++i)
		tal_arr_expand(&prc->exclude_nodes, prc->route[i].nodeid);
	tal_arr_expand(&prc->exclude_nodes, prc->startnode);

	return prc_start(prc);
}

/*-----------------------------------------------------------------------------
Failure handling
-----------------------------------------------------------------------------*/
/*~ Just blindly promote all JSONRPC errors to PAY_ROUTE_NOT_FOUND.  */

static struct command_result *
prc_rpc_err(struct command *cmd,
	    const char *buf,
	    const jsmntok_t *e,
	    struct permuteroute_command *prc)
{
	return command_fail(cmd, PAY_ROUTE_NOT_FOUND,
			    "Failed RPC command: %s", prc->last_command);
}

/*-----------------------------------------------------------------------------
Exclusion Checks
-----------------------------------------------------------------------------*/
/*~ These functions check if the given node or channel is excluded.  */

static bool
is_excluded_chan(const struct permuteroute_command *prc,
		 struct short_channel_id scid,
		 int direction)
{
	size_t i;
	for (i = 0; i < tal_count(prc->exclude_chans); ++i)
		if (short_channel_id_eq(&scid, &prc->exclude_chans[i].scid) &&
		    direction == prc->exclude_chans[i].dir)
			return true;
	return false;
}
static bool
is_excluded_node(const struct permuteroute_command *prc,
		 const struct node_id *id)
{
	size_t i;
	for (i = 0; i < tal_count(prc->exclude_nodes); ++i)
		if (node_id_eq(id, &prc->exclude_nodes[i]))
			return true;
	return false;
}

/*-----------------------------------------------------------------------------
listchannels Parsing
-----------------------------------------------------------------------------*/
/*~ This parses a single half-channel entry from `listchannels` command.  */

static bool
json_to_permuteroute_channel_data(const char *buf, const jsmntok_t *tok,
				  struct permuteroute_channel_data *dat)
{
	const jsmntok_t *sub;

	bool ok = true;

	if (tok->type != JSMN_OBJECT)
		return false;

	sub = ok ? json_get_member(buf, tok, "source") : NULL;
	ok = ok && sub;
	ok = ok && json_to_node_id(buf, sub, &dat->source);

	sub = ok ? json_get_member(buf, tok, "destination") : NULL;
	ok = ok && sub;
	ok = ok && json_to_node_id(buf, sub, &dat->destination);

	sub = ok ? json_get_member(buf, tok, "short_channel_id") : NULL;
	ok = ok && sub;
	ok = ok && json_to_short_channel_id(buf, sub, &dat->scid);

	sub = ok ? json_get_member(buf, tok, "active") : NULL;
	ok = ok && sub;
	ok = ok && json_to_bool(buf, sub, &dat->active);

	sub = ok ? json_get_member(buf, tok, "base_fee_millisatoshi") : NULL;
	ok = ok && sub;
	ok = ok && json_to_msat(buf, sub, &dat->base_fee);

	sub = ok ? json_get_member(buf, tok, "fee_per_millionth") : NULL;
	ok = ok && sub;
	ok = ok && json_to_number(buf, sub, &dat->fee_per_millionth);

	sub = ok ? json_get_member(buf, tok, "delay") : NULL;
	ok = ok && sub;
	ok = ok && json_to_number(buf, sub, &dat->delay);

	sub = ok ? json_get_member(buf, tok, "htlc_minimum_msat") : NULL;
	ok = ok && sub;
	ok = ok && json_to_msat(buf, sub, &dat->htlc_minimum_msat);

	sub = ok ? json_get_member(buf, tok, "htlc_maximum_msat") : NULL;
	ok = ok && sub;
	ok = ok && json_to_msat(buf, sub, &dat->htlc_maximum_msat);

	if (ok)
		/* Infer direction.  */
		dat->direction = node_id_idx(&dat->source, &dat->destination);

	return ok;
}

/*-----------------------------------------------------------------------------
Get channels of source
-----------------------------------------------------------------------------*/
/*~ We first get the channels of the source.

After that, we filter the half-channels, ensuring that the half-channel is
in the direction of the source->some node, that the capacity allows the amount,
etc.
*/

static struct command_result *
prc_filter_source_chans(struct command *cmd,
			const char *buf,
			const jsmntok_t *result,
			struct permuteroute_command *prc);

static struct command_result *prc_start(struct permuteroute_command *prc)
{
	struct out_req *req;
	const char *source_txt;

	source_txt = type_to_string(tmpctx, struct node_id,
				    &prc->source_node);

	plugin_log(prc->cmd->plugin, LOG_DBG,
		   "prc %"PRIu64": Healing %s XXX(%s)XXX %s",
		   prc->id,
		   source_txt,
		   prc->erring_thing,
		   type_to_string(tmpctx, struct node_id,
				  &prc->dest_node));

	prc->last_command = tal_fmt(prc, "listchannels %s # source",
				    source_txt);

	req = jsonrpc_request_start(prc->cmd->plugin, prc->cmd,
				    "listchannels",
				    &prc_filter_source_chans,
				    &prc_rpc_err,
				    prc);
	json_add_string(req->js, "source", source_txt);
	return send_outreq(prc->cmd->plugin, req);
}

static struct command_result *
prc_get_dest_channels(struct permuteroute_command *prc);

static struct command_result *
prc_filter_source_chans(struct command *cmd,
			const char *buf,
			const jsmntok_t *result,
			struct permuteroute_command *prc)
{
	const jsmntok_t *channels;
	const jsmntok_t *c;
	size_t i;

	channels = json_get_member(buf, result, "channels");
	if (!channels)
		plugin_err(cmd->plugin,
			   "'listchannels' did not return 'channels': %.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));
	if (channels->type != JSMN_ARRAY)
		plugin_err(cmd->plugin,
			   "'listchannels' 'channels' is not an array: %.*s",
			   json_tok_full_len(channels),
			   json_tok_full(buf, channels));

	prc->source_channels = tal_arr(prc, struct permuteroute_channel_data,
				       0);

	json_for_each_arr (i, c, channels) {
		struct permuteroute_channel_data one_chan;
		if (!json_to_permuteroute_channel_data(buf, c, &one_chan))
			plugin_err(cmd->plugin,
				   "'listchannels' returned unparseable "
				   "'channels' entry: %.*s",
				   json_tok_full_len(c),
				   json_tok_full(buf, c));

		/* Only care about the direction which starts with
		 * the source.  */
		if (!node_id_eq(&one_chan.source, &prc->source_node))
			continue;

		/* Check capacity.  */
		if (amount_msat_less(prc->source_out_amount,
				     one_chan.htlc_minimum_msat))
			continue;
		if (amount_msat_greater(prc->source_out_amount,
					one_chan.htlc_maximum_msat))
			continue;

		/* Check if it is an excluded channel or node.  */
		if (is_excluded_chan(prc,
				     one_chan.scid, one_chan.direction))
			continue;
		if (is_excluded_node(prc, &one_chan.destination))
			continue;

		tal_arr_expand(&prc->source_channels, one_chan);
	}

	/* If no channels left after exclusions, exit early.  */
	if (tal_count(prc->source_channels) == 0) {
		plugin_log(prc->cmd->plugin, LOG_DBG,
			   "prc %"PRIu64": source had no alternate routes.",
			   prc->id);
		return command_fail(prc->cmd, PAY_ROUTE_NOT_FOUND,
				    "No other usable channels before "
				    "'erring_index'.");
	}

	return prc_get_dest_channels(prc);
}

/*-----------------------------------------------------------------------------
Get channels of destination
-----------------------------------------------------------------------------*/
/*~ We then get the channels of the destination.

We do not bother creating a new array for the destination node channels.
Instead, as we parse each channel of the destination node, we also immediately
evaluate it for suitability.

If the channel can deliver the specified amount to the destination, and is
not excluded, and it matches up with a channel of the source node, we consider
it a success and go splice the channels into the route!
*/

static struct command_result *
prc_check_destination_chans(struct command *cmd,
			    const char *buf,
			    const jsmntok_t *result,
			    struct permuteroute_command *prc);

static struct command_result *
prc_get_dest_channels(struct permuteroute_command *prc)
{
	struct out_req *req;
	const char *destination_txt;

	destination_txt = type_to_string(tmpctx, struct node_id,
					 &prc->dest_node);
	prc->last_command = tal_fmt(prc, "listchannels %s # destination",
				    destination_txt);

	req = jsonrpc_request_start(prc->cmd->plugin, prc->cmd,
				    "listchannels",
				    &prc_check_destination_chans,
				    &prc_rpc_err,
				    prc);
	json_add_string(req->js, "source", destination_txt);
	return send_outreq(prc->cmd->plugin, req);
}

static struct command_result *
prc_splice(struct permuteroute_command *prc,
	   const struct permuteroute_channel_data *hop1,
	   const struct permuteroute_channel_data *hop2);

static struct command_result *
prc_check_destination_chans(struct command *cmd,
			    const char *buf,
			    const jsmntok_t *result,
			    struct permuteroute_command *prc)
{
	const jsmntok_t *channels;
	const jsmntok_t *c;
	size_t i;

	channels = json_get_member(buf, result, "channels");
	if (!channels)
		plugin_err(cmd->plugin,
			   "'listchannels' did not return 'channels': %.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));
	if (channels->type != JSMN_ARRAY)
		plugin_err(cmd->plugin,
			   "'listchannels' 'channels' is not an array: %.*s",
			   json_tok_full_len(channels),
			   json_tok_full(buf, channels));

	json_for_each_arr (i, c, channels) {
		struct permuteroute_channel_data one_chan;
		if (!json_to_permuteroute_channel_data(buf, c, &one_chan))
			plugin_err(cmd->plugin,
				   "'listchannels' returned unparseable "
				   "'channels' entry: %.*s",
				   json_tok_full_len(c),
				   json_tok_full(buf, c));

		/* Only care about the direction that ends at the
		 * destination.  */
		if (!node_id_eq(&one_chan.destination, &prc->dest_node))
			continue;

		/* Check capacity.  */
		if (amount_msat_less(prc->dest_amount,
				     one_chan.htlc_minimum_msat))
			continue;
		if (amount_msat_greater(prc->dest_amount,
					one_chan.htlc_maximum_msat))
			continue;

		/* Check if it is an excluded channel.  */
		if (is_excluded_chan(prc,
				     one_chan.scid, one_chan.direction))
			continue;
		/* Notice how we do not check if the source is an excluded
		 * node?
		 * This is because if the source of this channel is already
		 * an excluded node, it would not appear on the destination
		 * of any source node channel below anyway.
		 */

		for (size_t j = 0; j < tal_count(prc->source_channels); ++j)
			if (node_id_eq(&one_chan.source,
				       &prc->source_channels[j].destination))
				return prc_splice(prc,
						  &prc->source_channels[j],
						  &one_chan);
	}

	plugin_log(prc->cmd->plugin, LOG_DBG,
		   "prc %"PRIu64": No route to fix XXX(%s)XXX",
		   prc->id,
		   prc->erring_thing);

	return command_fail(cmd, PAY_ROUTE_NOT_FOUND,
			    "No route to fix `erring_index`.");
}

/*-----------------------------------------------------------------------------
Splice the break
-----------------------------------------------------------------------------*/
/*~ We have selected two channels --- one from the source, one from the
destination --- that can heal the broken route.
At this point, we then generate a two-hop splice for those two channels,
filling in the information for the hops from the channel data.

Note that this inserts a new node into the route.
We do not know if this node supports `OPT_VAR_ONION`, so we also have to
query the features of that node from `lightningd`.
*/

static struct command_result *
prc_get_listnodes_features(struct command *cmd,
			   const char *buf,
			   const jsmntok_t *result,
			   struct permuteroute_command *prc);

static struct command_result *
prc_splice(struct permuteroute_command *prc,
	   const struct permuteroute_channel_data *hop1,
	   const struct permuteroute_channel_data *hop2)
{
	const char *intermediate_txt;
	struct out_req *req;

	plugin_log(prc->cmd->plugin, LOG_DBG,
		   "prc %"PRIu64": Got splice: %s %s %s",
		   prc->id,
		   type_to_string(tmpctx, struct short_channel_id,
				  &hop1->scid),
		   type_to_string(tmpctx, struct node_id,
				  &hop1->destination),
		   type_to_string(tmpctx, struct short_channel_id,
				  &hop2->scid));

	prc->splice_route = tal_arrz(prc, struct route_hop, 2);

	/* Fill in hop2.  */
	prc->splice_route[1].channel_id = hop2->scid;
	prc->splice_route[1].direction = hop2->direction;
	prc->splice_route[1].nodeid = hop2->destination;
	prc->splice_route[1].amount = prc->dest_amount;
	prc->splice_route[1].delay = prc->dest_delay;
	prc->splice_route[1].style = prc->dest_style;

	/* Fill in hop1.  */
	prc->splice_route[0].channel_id = hop1->scid;
	prc->splice_route[0].direction = hop1->direction;
	prc->splice_route[0].nodeid = hop1->destination;
	prc->splice_route[0].amount = prc->dest_amount;
	if (!amount_msat_add_fee(&prc->splice_route[0].amount,
				 hop2->base_fee.millisatoshis, /* Raw: u32 */
				 hop2->fee_per_millionth)) {
		plugin_log(prc->cmd->plugin, LOG_BROKEN,
			   "Route fee overflow.");
		return command_fail(prc->cmd, PAY_ROUTE_NOT_FOUND,
				    "Route fee overflow.");
	}
	prc->splice_route[0].delay = prc->dest_delay + hop2->delay;
	/* Crucially, we do not know the style of the intermediate
	 * node.
	 * We will have a separate `listnodes` call later to fill
	 * that in.
	 */

	/* Fill in data for the beginning of the splice.  */
	prc->prefix_amount = prc->splice_route[0].amount;
	if (!amount_msat_add_fee(&prc->prefix_amount,
				 hop1->base_fee.millisatoshis, /* Raw: u32 */
				 hop1->fee_per_millionth)) {
		plugin_log(prc->cmd->plugin, LOG_BROKEN,
			   "Route fee overflow.");
		return command_fail(prc->cmd, PAY_ROUTE_NOT_FOUND,
				    "Route fee overflow.");
	}
	prc->prefix_delay = prc->splice_route[0].delay + hop1->delay;

	/* Now initiate the `listnodes` call for the new intermediate hop.  */
	intermediate_txt = type_to_string(tmpctx, struct node_id,
					  &hop1->destination);
	prc->last_command = tal_fmt(prc, "listnodes %s # hop",
				    intermediate_txt);
	req = jsonrpc_request_start(prc->cmd->plugin, prc->cmd,
				    "listnodes",
				    &prc_get_listnodes_features,
				    &prc_rpc_err,
				    prc);
	json_add_string(req->js, "id", intermediate_txt);
	return send_outreq(prc->cmd->plugin, req);
}

static struct command_result *prc_complete(struct permuteroute_command *prc);

static struct command_result *
prc_get_listnodes_features(struct command *cmd,
			   const char *buf,
			   const jsmntok_t *result,
			   struct permuteroute_command *prc)
{
	const jsmntok_t *nodes, *node, *featurestok;
	u8 *features;
	bool ok = true;

	nodes = ok ? json_get_member(buf, result, "nodes") : NULL;
	ok = ok && nodes && nodes->type == JSMN_ARRAY;
	/* There is an edge case here where the gossipd knew of a node with
	 * exactly two channels, which are the ones selected here, but in
	 * between our call to `listchannels` and our call here to `listnodes`
	 * both of them got closed, making gossipd forget the node completely.
	 * Rather than crash into a `plugin_err` we should just fail the
	 * routefinding.
	 */
	if (ok && nodes->size != 1) {
		plugin_log(prc->cmd->plugin, LOG_UNUSUAL,
			   "prc %"PRIu64": Node disappeared after being "
			   "selected from channels: %s",
			   prc->id,
			   type_to_string(tmpctx, struct node_id,
					  &prc->splice_route[0].nodeid));
		return command_fail(cmd, PAY_ROUTE_NOT_FOUND,
				    "Intermediate node disappeared in a "
				    "race condition, cowardly failing.");
	}
	node = ok ? json_get_arr(nodes, 0) : NULL;
	ok = ok && node && node->type == JSMN_OBJECT;
	featurestok = ok ? json_get_member(buf, node, "features") : NULL;
	ok = ok && featurestok;
	features = ok ? json_tok_bin_from_hex(prc, buf, featurestok) : NULL;
	ok = ok && features;

	if (!ok)
		plugin_err(prc->cmd->plugin,
			   "Unexpected result from listnodes: %.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));

	if (feature_offered(features, OPT_VAR_ONION))
		prc->splice_route[0].style = ROUTE_HOP_TLV;
	else
		prc->splice_route[0].style = ROUTE_HOP_LEGACY;

	return prc_complete(prc);
}

/*-----------------------------------------------------------------------------
Complete the modified route
-----------------------------------------------------------------------------*/
/*~ And we are done!

A twist here is that the splice might have increased the amount that needed
to be delivered over the channels before the erring thing.
We handle
*/

static struct command_result *prc_output(struct permuteroute_command *prc,
					 struct route_hop *prefix);

static struct command_result *prc_complete(struct permuteroute_command *prc)
{
	struct route_hop *prefix;
	struct amount_msat amount_last;
	u32 delay_last;
	struct amount_msat amount_delta;
	u32 delay_delta;
	size_t i;

	/* If no prefix, we can jump straight to the outputting bit.  */
	if (prc->source_index == 0)
		return prc_output(prc, NULL);

	/* We need to create a copy of the prefix of the original
	 * route, and tweak their fees and delays.  */
	prefix = tal_dup_arr(prc, struct route_hop,
			     prc->route, prc->source_index, 0);

	/* Figure out how much amount/delay was sent on the last hop of
	 * the prefix.  */
	amount_last = prefix[tal_count(prefix) - 1].amount;
	delay_last = prefix[tal_count(prefix) - 1].delay;

	/* Determine how much we need to add to each preceding hop.  */
	if (!amount_msat_sub(&amount_delta, prc->prefix_amount, amount_last))
		/* If the subtraction fails, it means the prefix delivers
		 * more money than what the splice needs, and we can just 
		 * not add anything to the prefix, hence amount_delta = 0.
		 */
		amount_delta = AMOUNT_MSAT(0);
	if (prc->prefix_delay > delay_last)
		delay_delta = prc->prefix_delay - delay_last;
	else
		delay_delta = 0;

	/* If both amount delta and delay delta are 0, then the unmodified
	 * prefix can deliver what the spliced route needs anyway, so we can
	 * jump to outputting.  */
	if (amount_msat_eq(amount_delta, AMOUNT_MSAT(0)) && delay_delta == 0)
		return prc_output(prc, prefix);

	/* Otherwise we need to adjust the amounts and delays of all the
	 * prefix items.  */
	for (i = 0; i < tal_count(prefix); ++i) {
		struct route_hop *e = &prefix[tal_count(prefix) - 1 - i];
		if (!amount_msat_add(&e->amount, e->amount, amount_delta))
			/* Not gonna happen.  */
			plugin_err(prc->cmd->plugin,
				   "Overflow in e->amount.");
		e->delay += delay_delta;
		/* The increase in amount of later hops might have crossed
		 * a roundoff boundary for the fee_millionths computation.
		 * i.e. if the original payment was 999,999 msat, if the new
		 * route charges 1,000,000 msat now, then even a hop node
		 * charging fee_millionths of 1 will expect the fee to be
		 * higher by 1 msat now.
		 *
		 * Rather than spend RPC bandwidth querying the exact
		 * fee_millionths from each hop in the prefix, we just
		 * increment amount_delta at each hop, overpaying fees by
		 * 1 millisatoshi (a very tiny amount, even for the expected
		 * future where a Big Mac costs about a hundred satoshi).
		 */
		if (!amount_msat_add(&amount_delta,
				     amount_delta, AMOUNT_MSAT(1)))
			/* Not gonna happen.  */
			plugin_err(prc->cmd->plugin,
				   "Overflow in amount_delta.");
	}

	return prc_output(prc, prefix);
}

static void json_add_route_hop(struct json_stream *r, char const *n,
			       const struct route_hop *h)
{
	/* Imitate what getroute/sendpay use */
	json_object_start(r, n);
	json_add_node_id(r, "id", &h->nodeid);
	json_add_short_channel_id(r, "channel",
				  &h->channel_id);
	json_add_num(r, "direction", h->direction);
	json_add_amount_msat_compat(r, h->amount, "msatoshi", "amount_msat");
	json_add_num(r, "delay", h->delay);
	json_add_string(r, "style",
			h->style == ROUTE_HOP_TLV ? "tlv" : "legacy");
	json_object_end(r);
}

static struct command_result *prc_output(struct permuteroute_command *prc,
					 struct route_hop *prefix)
{
	struct json_stream *js;
	size_t i;

	js = jsonrpc_stream_success(prc->cmd);

	json_array_start(js, "route");
	for (i = 0; i < tal_count(prefix); ++i)
		json_add_route_hop(js, NULL, &prefix[i]);
	for (i = 0; i < 2; ++i)
		json_add_route_hop(js, NULL, &prc->splice_route[i]);
	for (i = prc->dest_index; i < tal_count(prc->route); ++i)
		json_add_route_hop(js, NULL, &prc->route[i]);
	json_array_end(js);

	return command_finished(prc->cmd, js);
}
