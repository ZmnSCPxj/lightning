#include "multifundchannel.h"
#include <assert.h>
#include <bitcoin/chainparams.h>
#include <bitcoin/script.h>
#include <bitcoin/tx.h>
#include <ccan/array_size/array_size.h>
#include <ccan/crypto/sha256/sha256.h>
#include <ccan/compiler/compiler.h>
#include <ccan/str/str.h>
#include <common/addr.h>
#include <common/amount.h>
#include <common/features.h>
#include <common/json.h>
#include <common/json_stream.h>
#include <common/jsonrpc_errors.h>
#include <common/type_to_string.h>
#include <common/utils.h>
#include <plugins/libplugin_spark.h>
#include <stdarg.h>
#include <string.h>

/* State of whether we have done `fundchannel_start`.  */
enum multifundchannel_start {
	/* We have not yet performed `fundchannel_start`.  */
	multifundchannel_start_not_yet = 0,
	/* The `fundchannel_start` command succeeded.  */
	multifundchannel_started,
	/* The `fundchannel_start` command failed.  */
	multifundchannel_start_failed,
	/* The `fundchannel_complete` command failed.  */
	multifundchannel_complete_failed,
	/* The transaction might now be broadcasted.  */
	multifundchannel_done
};

struct multifundchannel_command;

/* The object for a single destination.  */
struct multifundchannel_destination {
	/* The overall multifundchannel command object.  */
	struct multifundchannel_command *mfc;

	/* ID for this destination.
	Prior to connecting this is the raw string from the
	arguments, afterwards it is just the peer ID in
	string form.
	*/
	const char *id;
	/* The features this destination has.  */
	const u8 *their_features;

	/* Whether we have performed `fundchannel_start`.  */
	enum multifundchannel_start fundchannel_start_state;

	/* The placeholder address of this destination
	used during the initial txprepare dryrun.
	*/
	const char *placeholder_addr_str;
	/* The actual target script and address.  */
	const u8 *funding_script;
	const char *funding_addr;

	/* The amount to be funded for this destination.
	If the specified amount is "all" then the `all`
	flag is set, and the amount is initially 0 until
	we have figured out how much exactly "all" is,
	after the dryrun stage.
	*/
	bool all;
	struct amount_sat amount;

	/* The output index for this destination.  */
	unsigned int outnum;

	/* Whether the channel to this destination will
	be announced.
	*/
	bool announce;
	/* How much of the initial funding to push to
	the destination.
	*/
	struct amount_msat push_msat;

	/* The actual channel_id.  */
	const char *channel_id;

	/* The spark currently running for this destination.  */
	struct plugin_spark_completion *spark;

	/* Any error messages.  */
	const char *error;
};

/* The object for a single multifundchannel command.  */
struct multifundchannel_command {
	/* The plugin-level command.  */
	struct command *cmd;
	/* An array of destinations.  */
	struct multifundchannel_destination *destinations;
	/* An array of sparks running each destination.  */
	struct plugin_spark **sparks;

	/* The feerate desired by the user.  */
	const char *feerate_str;
	/* The minimum number of confirmations for owned
	UTXOs to be selected.
	*/
	u32 minconf;
	/* The set of utxos to be used.  */
	const char *utxos_str;

	/* Flag set when any of the destinations has a value of "all".  */
	bool has_all;

	/* The txid of the funding transaction.
	This can be either the "dry run" transaction which
	reserves the funds, or the final funding transaction.
	*/
	struct bitcoin_txid *txid;

	/* The actual tx of the actual final funding transaction
	that was broadcast.
	*/
	const char *final_tx;
	const char *final_txid;
};

extern const struct chainparams *chainparams;

/*-----------------------------------------------------------------------------
Command Cleanup
-----------------------------------------------------------------------------*/

/*~
We disallow the use of command_fail and forward_error directly
in the rest of the code.

This ensures that if we ever fail a multifundchannel, we do cleanup
by doing fundchannel_cancel and txdiscard.
*/

/* TODO: This is lengthy enough to deserve its own source file,
clocking in at 240 loc.
*/

/* Object for performing cleanup.  */
struct multifundchannel_cleanup {
	struct plugin_spark **sparks;
	struct command_result *(*cb)(void *arg);
	void *arg;
};

/* Spark function that cleans up a `txprepare`d txid.  */
static struct command_result *
mfc_cleanup_txid_spark(struct command *cmd,
		       struct plugin_spark_completion *comp,
		       struct bitcoin_txid *txid);
/* Spark function that cleans up a `fundchannel_start`ed node id.  */
static struct command_result *
mfc_cleanup_fc_spark(struct command *cmd,
		     struct plugin_spark_completion *comp,
		     const char *nodeid);
/* Run at completion of all cleanup sparks.  */
static struct command_result *
mfc_cleanup_complete(struct command *cmd,
		     struct multifundchannel_cleanup *cleanup);

/* Core cleanup function.  */
static struct command_result *
mfc_cleanup_(struct multifundchannel_command *mfc,
	     struct command_result *(*cb)(void *arg),
	     void *arg)
{
	struct multifundchannel_cleanup *cleanup;
	unsigned int i;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: cleanup!", mfc);

	cleanup = tal(mfc, struct multifundchannel_cleanup);
	cleanup->sparks = tal_arr(cleanup, struct plugin_spark *, 0);
	cleanup->cb = cb;
	cleanup->arg = arg;

	if (mfc->txid) {
		plugin_log(mfc->cmd->plugin, LOG_DBG,
			   "mfc %p: txdiscard spark.", mfc);
		tal_arr_expand(&cleanup->sparks,
			       plugin_start_spark(mfc->cmd,
						  &mfc_cleanup_txid_spark,
						  mfc->txid));
	}
	for (i = 0; i < tal_count(mfc->destinations); ++i) {
		struct multifundchannel_destination *dest;
		dest = &mfc->destinations[i];

		/* If not started, nothing to clean up.  */
		if (dest->fundchannel_start_state != multifundchannel_started)
			continue;

		plugin_log(mfc->cmd->plugin, LOG_DBG,
			   "mfc %p, dest %p: fundchannel_cancel spark.",
			   mfc, dest);

		tal_arr_expand(&cleanup->sparks,
			       plugin_start_spark(mfc->cmd,
						  &mfc_cleanup_fc_spark,
						  dest->id));
	}

	return plugin_wait_all_sparks(mfc->cmd,
				      tal_count(cleanup->sparks),
				      cleanup->sparks,
				      &mfc_cleanup_complete,
				      cleanup);
}
static struct command_result *
mfc_cleanup_spark_done(struct command *cmd,
		       const char *buf UNUSED,
		       const jsmntok_t *res UNUSED,
		       struct plugin_spark_completion *comp)
{
	return plugin_spark_complete(cmd, comp);
}
/* Cleans up a txid by doing `txdiscard` on it.  */
static struct command_result *
mfc_cleanup_txid_spark(struct command *cmd,
		       struct plugin_spark_completion *comp,
		       struct bitcoin_txid *txid)
{
	struct out_req *req = jsonrpc_request_start(cmd->plugin,
						    cmd,
						    "txdiscard",
						    &mfc_cleanup_spark_done,
						    &mfc_cleanup_spark_done,
						    comp);
	json_add_string(req->js, "txid",
			type_to_string(tmpctx, struct bitcoin_txid, txid));
	return send_outreq(cmd->plugin, req);
}
/* Cleans up a `fundchannel_start` by doing `fundchannel_cancel` on
the node.
*/
static struct command_result *
mfc_cleanup_fc_spark(struct command *cmd,
		     struct plugin_spark_completion *comp,
		     const char *nodeid)
{
	struct out_req *req = jsonrpc_request_start(cmd->plugin,
						    cmd,
						    "fundchannel_cancel",
						    &mfc_cleanup_spark_done,
						    &mfc_cleanup_spark_done,
						    comp);
	json_add_string(req->js, "id", nodeid);
	return send_outreq(cmd->plugin, req);
}
/* Done when all cleanup operations have completed.  */
static struct command_result *
mfc_cleanup_complete(struct command *cmd UNUSED,
		     struct multifundchannel_cleanup *cleanup)
{
	tal_steal(tmpctx, cleanup);
	return cleanup->cb(cleanup->arg);
}
#define mfc_cleanup(mfc, cb, arg) \
	mfc_cleanup_(mfc, typesafe_cb(struct command_result *, void *, \
				      (cb), (arg)), \
		     (arg))

/* Use this instead of command_fail.  */
static struct command_result *
mfc_fail(struct multifundchannel_command *, errcode_t code,
	 const char *fmt, ...);
/* Use this instead of forward_error.  */
static struct command_result *
mfc_forward_error(struct command *cmd,
		  const char *buf, const jsmntok_t *error,
		  struct multifundchannel_command *);
/* Use this instead of command_finished.  */
static struct command_result *
mfc_finished(struct multifundchannel_command *, struct json_stream *response);
/* Use this instead of command_err_raw.  */
static struct command_result *
mfc_err_raw(struct multifundchannel_command *, const char *json_string);

/*---------------------------------------------------------------------------*/

/* These are the actual implementations of the cleanup entry functions.  */

struct mfc_fail_object {
	struct multifundchannel_command *mfc;
	struct command *cmd;
	errcode_t code;
	const char *msg;
};
static struct command_result *
mfc_fail_complete(struct mfc_fail_object *obj);
static struct command_result *
mfc_fail(struct multifundchannel_command *mfc, errcode_t code,
	 const char *fmt, ...)
{
	struct mfc_fail_object *obj;
	const char *msg;
	va_list ap;

	va_start(ap, fmt);
	msg = tal_vfmt(mfc, fmt, ap);
	va_end(ap);

	obj = tal(mfc, struct mfc_fail_object);
	obj->mfc = mfc;
	obj->cmd = mfc->cmd;
	obj->code = code;
	obj->msg = msg;

	return mfc_cleanup(mfc, &mfc_fail_complete, obj);
}
static struct command_result *
mfc_fail_complete(struct mfc_fail_object *obj)
{
	plugin_log(obj->mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: cleanup done, failing.", obj->mfc);
	return command_fail(obj->cmd, obj->code, "%s", obj->msg);
}

struct mfc_err_raw_object {
	struct multifundchannel_command *mfc;
	const char *error;
};
static struct command_result *
mfc_err_raw_complete(struct mfc_err_raw_object *obj);
static struct command_result *
mfc_err_raw(struct multifundchannel_command *mfc, const char *json_string)
{
	struct mfc_err_raw_object *obj;

	obj = tal(mfc, struct mfc_err_raw_object);
	obj->mfc = mfc;
	obj->error = tal_strdup(obj, json_string);

	return mfc_cleanup(mfc, &mfc_err_raw_complete, obj);
}
static struct command_result *
mfc_err_raw_complete(struct mfc_err_raw_object *obj)
{
	plugin_log(obj->mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: cleanup done, failing raw.", obj->mfc);
	return command_err_raw(obj->mfc->cmd, obj->error);
}
static struct command_result *
mfc_forward_error(struct command *cmd,
		  const char *buf, const jsmntok_t *error,
		  struct multifundchannel_command *mfc)
{
	plugin_log(cmd->plugin, LOG_DBG,
		   "mfc %p: forwarding error, about to cleanup.", mfc);
	return mfc_err_raw(mfc, json_strdup(tmpctx, buf, error));
}

struct mfc_finished_object {
	struct multifundchannel_command *mfc;
	struct command *cmd;
	struct json_stream *response;
};
static struct command_result *
mfc_finished_complete(struct mfc_finished_object *obj);
static struct command_result *
mfc_finished(struct multifundchannel_command *mfc,
	     struct json_stream *response)
{
	struct mfc_finished_object *obj;

	/* The response will be constructed by jsonrpc_stream_success,
	which allocates off the command, so it should be safe to
	just store it here.
	*/
	obj = tal(mfc, struct mfc_finished_object);
	obj->mfc = mfc;
	obj->cmd = mfc->cmd;
	obj->response = response;

	return mfc_cleanup(mfc, &mfc_finished_complete, obj);
}
static struct command_result *
mfc_finished_complete(struct mfc_finished_object *obj)
{
	plugin_log(obj->mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: cleanup done, succeeding.", obj->mfc);
	return command_finished(obj->cmd, obj->response);
}

/*-----------------------------------------------------------------------------
Input Validation
-----------------------------------------------------------------------------*/

/* Validates the destinations input argument.

Returns NULL if checking of destinations array worked,
or non-NULL if it failed (and this function has already
executed mfc_fail).
*/
static struct command_result *
create_destinations_array(struct multifundchannel_command *mfc,
			  struct multifundchannel_destination **destinations,
			  const char *buf,
			  const jsmntok_t *json_destinations,
			  bool *has_all)
{
	/* Needed by p_opt_def for some reason.... */
	struct command *cmd = mfc->cmd;
	unsigned int i;
	const jsmntok_t *json_dest;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: validating input.", mfc);

	if (json_destinations->type != JSMN_ARRAY)
		return mfc_fail(mfc, JSONRPC2_INVALID_PARAMS,
				"'destinations' must be an array.");
	if (json_destinations->size < 1)
		return mfc_fail(mfc, JSONRPC2_INVALID_PARAMS,
				"'destinations' must have at "
				"least one entry.");

	*destinations = tal_arrz(mfc, struct multifundchannel_destination,
				 json_destinations->size);

	*has_all = false;

	json_for_each_arr (i, json_dest, json_destinations) {
		struct multifundchannel_destination *dest;
		struct amount_sat *amount;
		bool *announce;
		struct amount_msat *push_msat;

		dest = &(*destinations)[i];

		if (!param(mfc->cmd, buf, json_dest,
			   p_req("id", param_string, &dest->id),
			   p_req("amount", param_sat_or_all, &amount),
			   p_opt_def("announce", param_bool, &announce, true),
			   p_opt_def("push_msat", param_msat, &push_msat,
				     AMOUNT_MSAT(0)),
			   NULL))
			return command_param_failed();

		dest->mfc = mfc;
		dest->their_features = NULL;
		dest->fundchannel_start_state = multifundchannel_start_not_yet;
		dest->placeholder_addr_str = NULL;
		dest->funding_script = NULL;
		dest->funding_addr = NULL;
		dest->all = amount_sat_eq(*amount, AMOUNT_SAT(-1ULL));
		dest->amount = dest->all ? AMOUNT_SAT(0) : *amount;
		dest->announce = *announce;
		dest->push_msat = *push_msat;
		dest->spark = NULL;
		dest->error = NULL;

		/* Only one destination can have "all" indicator.  */
		if (dest->all) {
			if (*has_all)
				return mfc_fail(mfc,
						JSONRPC2_INVALID_PARAMS,
						"Only one destination "
						"can indicate \"all\" "
						"for 'amount'.");
			*has_all = true;
		}
	}

	/* TODO: in theory we could have one output specify "all"
	and the other outputs specify exact amounts, we just take
	"all" to mean whatever remains after the exact amounts have
	been deducted.
	However, that should probably be implemented in `txprepare`
	first before we can support it out here in `multifundchannel`,
	due to atomicity that is available inside `lightningd` but not
	in plugins.
	*/
	if (*has_all && tal_count(*destinations) > 1)
		return mfc_fail(mfc, JSONRPC2_INVALID_PARAMS,
				"There can only be one destination "
				"if you specify \"all\".");

	return NULL;
}

/*-----------------------------------------------------------------------------
Command Processing
-----------------------------------------------------------------------------*/

static struct command_result *
perform_multiconnect(struct multifundchannel_command *mfc);

/* Initiate the multifundchannel execution.  */
static struct command_result *
perform_multifundchannel(struct multifundchannel_command *mfc)
{
	return perform_multiconnect(mfc);
}

/*---------------------------------------------------------------------------*/
/*~
First, connect to all the peers.

This is a convenience both to us and to the user.

We delegate parsing for valid node IDs to the
`multiconnect`.
In addition, this means the user does not have to
connect to the specified nodes.

In particular, some implementations (including some
versions of C-Lightning) will disconnect in case
of funding channel failure.
And with a *multi* funding, it is more likely to
fail due to having to coordinate many more nodes.
*/

static struct command_result *
after_multiconnect(struct command *cmd,
		   const char *buf,
		   const jsmntok_t *result,
		   struct multifundchannel_command *mfc);

/* Initiate the multiconnect.  */
static struct command_result *
perform_multiconnect(struct multifundchannel_command *mfc)
{
	struct out_req *req;
	unsigned int i;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: multiconnect.", mfc);

	req = jsonrpc_request_start(mfc->cmd->plugin,
				    mfc->cmd,
				    "multiconnect",
				    &after_multiconnect,
				    &mfc_forward_error,
				    mfc);
	json_array_start(req->js, "id");
	for (i = 0; i < tal_count(mfc->destinations); ++i)
		json_add_string(req->js, NULL, mfc->destinations[i].id);
	json_array_end(req->js);

	return send_outreq(mfc->cmd->plugin, req);
}

static struct command_result *
perform_dryrun_txprepare(struct multifundchannel_command *mfc);

/* Extract id and features.  */
static struct command_result *
after_multiconnect(struct command *cmd,
		   const char *buf,
		   const jsmntok_t *result,
		   struct multifundchannel_command *mfc)
{
	const jsmntok_t *idtok;
	const jsmntok_t *featurestok;
	unsigned int i, j;
	const jsmntok_t *t;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: multiconnect done.", mfc);

	idtok = json_get_member(buf, result, "id");
	if (!idtok)
		plugin_err(cmd->plugin, "'multiconnect' missing 'id' field");
	if (idtok->type != JSMN_ARRAY)
		plugin_err(cmd->plugin,
			   "'multiconnect' 'id' field not an array");
	if (idtok->size != tal_count(mfc->destinations))
		plugin_err(cmd->plugin,
			   "'multiconnect' 'id' array wrong length");

	featurestok = json_get_member(buf, result, "features");
	if (!featurestok)
		plugin_err(cmd->plugin,
			   "'multiconnect' missing 'features' field");
	if (featurestok->type != JSMN_ARRAY)
		plugin_err(cmd->plugin,
			   "'multiconnect' 'features' field not an array");
	if (featurestok->size != tal_count(mfc->destinations))
		plugin_err(cmd->plugin,
			   "'multiconnect' 'features' array wrong length");

	/* Easier as two loops.  */
	json_for_each_arr (i, t, idtok)
		mfc->destinations[i].id = json_strdup(mfc, buf, t);
	json_for_each_arr (i, t, featurestok)
		mfc->destinations[i].their_features
			= json_tok_bin_from_hex(mfc, buf, t);

	/* Make sure every id is unique.
	We do this checking here so that the node id is already known
	to be just a node ID without any @host:port indications.
	*/
	for (i = 0; i < tal_count(mfc->destinations); ++i)
		for (j = i + 1; j < tal_count(mfc->destinations); ++j)
			if (streq(mfc->destinations[i].id,
				  mfc->destinations[j].id))
				return mfc_fail(mfc,
						JSONRPC2_INVALID_PARAMS,
						"Duplicate destination: "
						"%s",
						mfc->destinations[i].id);

	return perform_dryrun_txprepare(mfc);
}

/*---------------------------------------------------------------------------*/

static struct command_result *
after_dryrun_txprepare(struct command *cmd,
		       const char *buf,
		       const jsmntok_t *result,
		       struct multifundchannel_command *mfc);

/*~ Generate a unique placeholder address for use during the
dryrun `txprepare`.
This is later used to identify which output of the `txprepare`d
transaction belongs to which destination, in order to later
extract amounts that the user specified, as "all".
*/
static const char *
create_placeholder_addr(const tal_t *ctx, const char *destid)
{
	struct sha256 hash;
	u8 *placeholder_script = tal_arr(tmpctx, u8, 2 + sizeof(struct sha256));

	/* Generate a P2WSH address for this destination id.

	This is not actually a valid P2WSH, but note we only
	need *some* unique P2WSH address --- we are not going
	to actually broadcast this transaction.
	*/
	sha256(&hash, destid, strlen(destid));
	placeholder_script[0] = 0x00; /* SegWit version.  */
	placeholder_script[1] = 0x20; /* PUSHDATA 32.  */
	memcpy(&placeholder_script[2], &hash, sizeof(struct sha256));

	return encode_scriptpubkey_to_addr(ctx, chainparams,
					   placeholder_script);
}

/*~ Perform a dryrun `txprepare`, using placeholder addresses.
The reason for doing a dryrun `txprepare` is:

1.  It delegates handling of "all" to `txprepare`.
2.  It ensures we have the funds available before we even
    bother our peers with a channel opening proposal via
    `fundchannel_start`.
3.  It reserves the funds while we are doing (maybe lengthy)
    network operations `fundchannel_start` and
    `fundchannel_complete`.
*/
static struct command_result *
perform_dryrun_txprepare(struct multifundchannel_command *mfc)
{
	struct out_req *req;
	struct json_stream *js;
	unsigned int i;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: dryrun txprepare.", mfc);

	req = jsonrpc_request_start(mfc->cmd->plugin,
				    mfc->cmd,
				    "txprepare",
				    &after_dryrun_txprepare,
				    &mfc_forward_error,
				    mfc);
	js = req->js;

	json_array_start(js, "outputs");
	for (i = 0; i < tal_count(mfc->destinations); ++i) {
		struct multifundchannel_destination *dest;

		dest = &mfc->destinations[i];

		dest->placeholder_addr_str
			= create_placeholder_addr(mfc, dest->id);

		json_object_start(js, NULL);
		json_add_string(js, dest->placeholder_addr_str,
				dest->all ? "all" :
					    fmt_amount_sat(tmpctx,
							   &dest->amount));
		json_object_end(js);
	}
	json_array_end(js);

	if (mfc->feerate_str)
		json_add_string(js, "feerate", mfc->feerate_str);
	json_add_u32(js, "minconf", mfc->minconf);
	if (mfc->utxos_str)
		json_add_string(js, "utxos", mfc->utxos_str);

	return send_outreq(mfc->cmd->plugin, req);
}

/*---------------------------------------------------------------------------*/

/* Function to get the results of a txprepare command.
It loads the txid into the struct multifundchannel_command,
and returns the parsed bitcoin transaction.
*/
static struct bitcoin_tx *
get_txprepare_results(const tal_t *ctx,
		      struct multifundchannel_command *mfc,
		      const char *buf,
		      const jsmntok_t *result)
{
	const jsmntok_t *txid_tok;
	const jsmntok_t *tx_tok;
	struct bitcoin_tx *tx;

	/* Extract the txid.  */
	txid_tok = json_get_member(buf, result, "txid");
	if (!txid_tok)
		plugin_err(mfc->cmd->plugin,
			   "txprepare did not return 'txid': %.*s",
			   result->end - result->start, buf + result->start);
	mfc->txid = tal(mfc, struct bitcoin_txid);
	if (!bitcoin_txid_from_hex(buf + txid_tok->start,
				   txid_tok->end - txid_tok->start,
				   mfc->txid))
		plugin_err(mfc->cmd->plugin,
			   "Unable to parse 'txid' from txprepare: "
			   "%.*s",
			   txid_tok->end - txid_tok->start,
			   buf + txid_tok->start);

	/* Extract the tx.  */
	tx_tok = json_get_member(buf, result, "unsigned_tx");
	if (!tx_tok)
		plugin_err(mfc->cmd->plugin,
			   "txprepare did not return 'unsigned_tx': %.*s",
			   result->end - result->start, buf + result->start);
	tx = bitcoin_tx_from_hex(ctx,
				 buf + tx_tok->start,
				 tx_tok->end - tx_tok->start);
	if (!tx)
		plugin_err(mfc->cmd->plugin,
			   "Unable to parse 'unsigned_tx' from txprepare: "
			   "%.*s",
			   tx_tok->end - tx_tok->start,
			   buf + tx_tok->start);

	return tx;
}

static struct command_result *
perform_fundchannel_start(struct multifundchannel_command *mfc);

static struct command_result *
after_dryrun_txprepare(struct command *cmd,
		       const char *buf,
		       const jsmntok_t *result,
		       struct multifundchannel_command *mfc)
{
	unsigned int d;
	unsigned int o;
	struct bitcoin_tx *tx;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: dryrun txprepare done.", mfc);

	tx = get_txprepare_results(tmpctx, mfc,
				   buf, result);

	/* Check the outputs.  */
	for (d = 0; d < tal_count(mfc->destinations); ++d) {
		bool found = false;
		struct multifundchannel_destination *dest;
		dest = &mfc->destinations[d];

		for (o = 0; o < tx->wtx->num_outputs; ++o) {
			const u8 *output_script;
			const char *output_addr;
			struct amount_asset asset;

			asset = bitcoin_tx_output_get_amount(tx, o);
			if (!amount_asset_is_main(&asset))
				continue;
			output_script = bitcoin_tx_output_get_script(tmpctx,
								     tx, o);
			output_addr = encode_scriptpubkey_to_addr
					(tmpctx,
					 chainparams,
					 output_script);
			if (!streq(output_addr, dest->placeholder_addr_str)) {
				output_script = tal_free(output_script);
				output_addr = tal_free(output_addr);
				continue;
			}

			output_script = tal_free(output_script);
			output_addr = tal_free(output_addr);

			found = true;
			/* We now know the exact amount that funding output
			will have.
			*/
			dest->amount = amount_asset_to_sat(&asset);
			if (dest->all
			 && !feature_negotiated(plugin_feature_set(cmd->plugin),
						dest->their_features,
						OPT_LARGE_CHANNELS)
			 && amount_sat_greater(dest->amount,
					       chainparams->max_funding))
				dest->amount = chainparams->max_funding;
			dest->all = false;
			break;
		}

		if (!found)
			plugin_err(cmd->plugin,
				   "txprepare transction does not have output "
				   "address %s "
				   "for destination %s.",
				   dest->placeholder_addr_str,
				   dest->id);
	}
	mfc->has_all = false;

	return perform_fundchannel_start(mfc);
}

/*---------------------------------------------------------------------------*/

/*~
We perform all the `fundchannel_start` in parallel
by using the plugin spark system, which launches
concurrent tasks and switches between them when
they are blocked on commands.

We need to parallelize `fundchannel_start` execution
since the command has to wait for a response from
the remote peer.
The remote peer is not under our control and might
respond after a long time.

By doing them in parallel, the time it takes to
perform all the `fundchannel_start` is only the
slowest time among all peers.
This is important since faster peers might impose a
timeout on channel opening and fail subsequent
steps if we take too long before running
`fundchannel_complete`.
*/

static struct command_result *
fundchannel_start_spark(struct command *cmd,
			struct plugin_spark_completion *spark,
			struct multifundchannel_destination *dest);
static struct command_result *
after_fundchannel_start(struct command *cmd,
			struct multifundchannel_command *mfc);
static struct command_result *
perform_fundchannel_start(struct multifundchannel_command *mfc)
{
	unsigned int i;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: fundchannel_start sparks.", mfc);

	for (i = 0; i < tal_count(mfc->destinations); ++i)
		mfc->sparks[i] = plugin_start_spark(mfc->cmd,
						    &fundchannel_start_spark,
						    &mfc->destinations[i]);
	return plugin_wait_all_sparks(mfc->cmd,
				      tal_count(mfc->sparks),
				      mfc->sparks,
				      &after_fundchannel_start,
				      mfc);
}

/* Handles fundchannel_start success and failure.  */
static struct command_result *
fundchannel_start_ok(struct command *cmd,
		     const char *buf,
		     const jsmntok_t *result,
		     struct multifundchannel_destination *dest);
static struct command_result *
fundchannel_start_err(struct command *cmd,
		      const char *buf,
		      const jsmntok_t *error,
		      struct multifundchannel_destination *dest);

static struct command_result *
fundchannel_start_spark(struct command *cmd,
			struct plugin_spark_completion *spark,
			struct multifundchannel_destination *dest)
{
	struct multifundchannel_command *mfc = dest->mfc;
	struct out_req *req;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p, dest %p: fundchannel_start %s.",
		   mfc, dest, dest->id);

	dest->spark = spark;

	req = jsonrpc_request_start(cmd->plugin,
				    cmd,
				    "fundchannel_start",
				    &fundchannel_start_ok,
				    &fundchannel_start_err,
				    dest);

	json_add_string(req->js, "id", dest->id);
	assert(!dest->all);
	json_add_string(req->js, "amount",
			fmt_amount_sat(tmpctx, &dest->amount));

	if (mfc->feerate_str)
		json_add_string(req->js, "feerate", mfc->feerate_str);
	json_add_bool(req->js, "announce", dest->announce);
	json_add_string(req->js, "push_msat",
			fmt_amount_msat(tmpctx, &dest->push_msat));

	return send_outreq(cmd->plugin, req);
}

static struct command_result *
fundchannel_start_ok(struct command *cmd,
		     const char *buf,
		     const jsmntok_t *result,
		     struct multifundchannel_destination *dest)
{
	struct multifundchannel_command *mfc = dest->mfc;
	const jsmntok_t *address_tok;
	const jsmntok_t *script_tok;
	struct plugin_spark_completion *spark;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p, dest %p: fundchannel_start %s done.",
		   mfc, dest, dest->id);

	/* Extract funding_address.  */
	address_tok = json_get_member(buf, result, "funding_address");
	if (!address_tok)
		plugin_err(cmd->plugin,
			   "fundchannel_start did not "
			   "return 'funding_address': %.*s",
			   result->end - result->start,
			   buf + result->start);
	dest->funding_addr = json_strdup(dest->mfc, buf, address_tok);
	/* Extract scriptpubkey.  */
	script_tok = json_get_member(buf, result, "scriptpubkey");
	if (!script_tok)
		plugin_err(cmd->plugin,
			   "fundchannel_start did not "
			   "return 'scriptpubkey': %.*s",
			   result->end - result->start,
			   buf + result->start);
	dest->funding_script = json_tok_bin_from_hex(dest->mfc,
						     buf, script_tok);
	if (!dest->funding_script)
		plugin_err(cmd->plugin,
			   "fundchannel_start did not "
			   "return parseable 'scriptpubkey': %.*s",
			   script_tok->end - script_tok->start,
			   buf + script_tok->start);

	dest->fundchannel_start_state = multifundchannel_started;

	spark = dest->spark;
	dest->spark = NULL;
	return plugin_spark_complete(cmd, spark);
}
static struct command_result *
fundchannel_start_err(struct command *cmd,
		      const char *buf,
		      const jsmntok_t *error,
		      struct multifundchannel_destination *dest)
{
	struct multifundchannel_command *mfc = dest->mfc;
	struct plugin_spark_completion *spark;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p, dest %p: failed! fundchannel_start %s: %.*s.",
		   mfc, dest, dest->id,
		   error->end - error->start, buf + error->start);

	/*
	You might be wondering why we do not just use
	mfc_forward_error here.
	The reason is that other `fundchannel_start`
	commands are running in the meantime,
	and it is still ambiguous whether the opening
	of other destinations was started or not.

	After all sparked `fundchannel_start`s have
	completed, we can then use `mfc_err_raw`.
	*/

	dest->fundchannel_start_state = multifundchannel_start_failed;
	dest->error = json_strdup(dest->mfc, buf, error);

	spark = dest->spark;
	dest->spark = NULL;
	return plugin_spark_complete(cmd, spark);
}

static struct command_result *
perform_txmodify(struct multifundchannel_command *mfc);

/* All fundchannel_start commands have returned with either
success or failure.
*/
static struct command_result *
after_fundchannel_start(struct command *cmd,
			struct multifundchannel_command *mfc)
{
	unsigned int i;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: fundchannel_start sparks done.", mfc);

	/* Check if any fundchannel_start failed.  */
	for (i = 0; i < tal_count(mfc->destinations); ++i) {
		struct multifundchannel_destination *dest;

		dest = &mfc->destinations[i];

		assert(dest->fundchannel_start_state == multifundchannel_started
		    || dest->fundchannel_start_state == multifundchannel_start_failed);

		if (dest->fundchannel_start_state != multifundchannel_start_failed)
			continue;


		/* One of them failed, oh no.
		Forward the error.
		*/
		plugin_log(mfc->cmd->plugin, LOG_DBG,
			   "mfc %p, dest %p: "
			   "fundchannel_start failure being forwarded.",
			   mfc, dest);

		return mfc_err_raw(mfc, dest->error);
	}

	/* Next step.  */
	return perform_txmodify(mfc);
}

/*---------------------------------------------------------------------------*/

/*~
The current transaction is a dummy one that pays to
random P2WSH scripts.
We need to modify the transaction into an actual
valid funding transaction, using the addresses
we acquired from the individual `fundchannel_start`
commands.
*/

/* TODO

Ideally we would have a txmodify command that
modifies a non-broadcast transaction, in order
to ensure a continuous reservation of the
transaction input funds.

For now, we emulate this (with a race-condition
risk) by doing `txdiscard` followed by a
`txprepare`.
*/

static struct command_result *
after_txdiscard(struct command *cmd,
		const char *buf, const jsmntok_t *result,
		struct multifundchannel_command *mfc);

static struct command_result *
perform_txmodify(struct multifundchannel_command *mfc)
{
	struct bitcoin_txid *txid;
	struct out_req *req;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: txmodify - txdiscard.", mfc);

	/* Move responsibility for the txid to this function.  */
	txid = tal_steal(tmpctx, mfc->txid);
	mfc->txid = NULL;

	/* Perform txdiscard.  */
	req = jsonrpc_request_start(mfc->cmd->plugin,
				    mfc->cmd,
				    "txdiscard",
				    &after_txdiscard,
				    &mfc_forward_error,
				    mfc);
	json_add_string(req->js, "txid",
			type_to_string(tmpctx, struct bitcoin_txid, txid));

	return send_outreq(mfc->cmd->plugin, req);
}

static struct command_result *
perform_txprepare(struct multifundchannel_command *mfc);

static struct command_result *
after_txdiscard(struct command *cmd UNUSED,
		const char *buf UNUSED, const jsmntok_t *result UNUSED,
		struct multifundchannel_command *mfc)
{
	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: txmodify - txdiscard done.", mfc);
	/* Ignore result of successful discard.  */
	return perform_txprepare(mfc);
}

static struct command_result *
after_txprepare(struct command *cmd,
		const char *buf, const jsmntok_t *result,
		struct multifundchannel_command *mfc);

/* This is the actual txprepare of the actual funding tx
that we will broadcast later.
*/
static struct command_result *
perform_txprepare(struct multifundchannel_command *mfc)
{
	struct out_req *req;
	struct json_stream *js;
	unsigned int i;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: txmodify - txprepare.", mfc);

	req = jsonrpc_request_start(mfc->cmd->plugin,
				    mfc->cmd,
				    "txprepare",
				    &after_txprepare,
				    &mfc_forward_error,
				    mfc);
	js = req->js;

	json_array_start(js, "outputs");
	for (i = 0; i < tal_count(mfc->destinations); ++i) {
		struct multifundchannel_destination *dest;
		dest = &mfc->destinations[i];

		assert(!dest->all);

		json_object_start(js, NULL);
		json_add_string(js, dest->funding_addr,
				fmt_amount_sat(tmpctx, &dest->amount));
		json_object_end(js);
	}
	json_array_end(js);

	if (mfc->feerate_str)
		json_add_string(js, "feerate", mfc->feerate_str);
	json_add_u32(js, "minconf", mfc->minconf);
	if (mfc->utxos_str)
		json_add_string(js, "utxos", mfc->utxos_str);

	return send_outreq(mfc->cmd->plugin, req);
}

static struct command_result *
perform_fundchannel_complete(struct multifundchannel_command *mfc);

static struct command_result *
after_txprepare(struct command *cmd,
		const char *buf, const jsmntok_t *result,
		struct multifundchannel_command *mfc)
{
	unsigned int d;
	unsigned int o;

	struct bitcoin_tx *tx;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: txmodify - txprepare done.", mfc);

	tx = get_txprepare_results(tmpctx, mfc, buf, result);

	/* Look for the matching output number.  */
	for (d = 0; d < tal_count(mfc->destinations); ++d) {
		bool found = false;
		struct multifundchannel_destination *dest;
		dest = &mfc->destinations[d];

		for (o = 0; o < tx->wtx->num_outputs; ++o) {
			const u8 *output_script;

			output_script = bitcoin_tx_output_get_script(tmpctx,
								     tx, o);
			if (!scripteq(output_script, dest->funding_script)) {
				output_script = tal_free(output_script);
				continue;
			}

			output_script = tal_free(output_script);
			found = true;
			dest->outnum = o;
			break;
		}
		if (!found)
			plugin_err(cmd->plugin,
				   "txprepare missing output: "
				   "tx: %s, script: %s",
				   type_to_string(tmpctx, struct bitcoin_tx,
						  tx),
				   tal_hex(tmpctx, dest->funding_script));
	}

	return perform_fundchannel_complete(mfc);
}

/*---------------------------------------------------------------------------*/
/*~
We have now modified the transaction in mfc->txid
to a proper funding transaction that puts the
money into 2-of-2 channel outpoints.

However, we cannot broadcast it yet!
We need to get backout transactions --- the initial
commitment transactions --- in case any of the
peers disappear later.
Those initial commitment transactions are the
unilateral close (force-close) transactions
for each channel.
With unilateral opprtunity to close, we can then
safely broadcast the tx, so that in case the
peer disappears, we can recover our funds.

The `fundchannel_complete` command performs the
negotiation with the peer to sign the initial
commiteent transactions.
Only once the `lightningd` has the transactions
signed does the `fundchannel_complete` command
return with a success.
After that point we can `txsend` the transaction.
*/

static struct command_result *
fundchannel_complete_spark(struct command *cmd,
			   struct plugin_spark_completion *comp,
			   struct multifundchannel_destination *dest);
static struct command_result *
after_fundchannel_complete(struct command *cmd,
			   struct multifundchannel_command *mfc);

static struct command_result *
perform_fundchannel_complete(struct multifundchannel_command *mfc)
{
	unsigned int i;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: fundchannel_complete sparks.", mfc);

	for (i = 0; i < tal_count(mfc->destinations); ++i)
		mfc->sparks[i] = plugin_start_spark(mfc->cmd,
						    &fundchannel_complete_spark,
						    &mfc->destinations[i]);

	return plugin_wait_all_sparks(mfc->cmd,
				      tal_count(mfc->sparks),
				      mfc->sparks,
				      &after_fundchannel_complete,
				      mfc);
}

static struct command_result *
fundchannel_complete_ok(struct command *cmd,
			const char *buf,
			const jsmntok_t *result,
			struct multifundchannel_destination *dest);
static struct command_result *
fundchannel_complete_err(struct command *cmd,
			 const char *buf,
			 const jsmntok_t *error,
			 struct multifundchannel_destination *dest);

static struct command_result *
fundchannel_complete_spark(struct command *cmd,
			   struct plugin_spark_completion *spark,
			   struct multifundchannel_destination *dest)
{
	struct multifundchannel_command *mfc = dest->mfc;
	struct out_req *req;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p, dest %p: fundchannel_complete %s.",
		   mfc, dest, dest->id);

	dest->spark = spark;

	req = jsonrpc_request_start(cmd->plugin,
				    cmd,
				    "fundchannel_complete",
				    &fundchannel_complete_ok,
				    &fundchannel_complete_err,
				    dest);
	json_add_string(req->js, "id", dest->id);
	json_add_string(req->js, "txid",
			type_to_string(tmpctx, struct bitcoin_txid,
				       mfc->txid));
	json_add_num(req->js, "txout", dest->outnum);

	return send_outreq(cmd->plugin, req);
}
static struct command_result *
fundchannel_complete_ok(struct command *cmd,
			const char *buf,
			const jsmntok_t *result,
			struct multifundchannel_destination *dest)
{
	struct multifundchannel_command *mfc = dest->mfc;
	const jsmntok_t *channel_id_tok;
	struct plugin_spark_completion *spark;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p, dest %p: fundchannel_complete %s done.",
		   mfc, dest, dest->id);

	channel_id_tok = json_get_member(buf, result, "channel_id");
	if (!channel_id_tok)
		plugin_err(cmd->plugin,
			   "fundchannel_complete no channel_id: %.*s",
			   result->end - result->start,
			   buf + result->start);
	dest->channel_id = json_strdup(mfc, buf, channel_id_tok);

	spark = dest->spark;
	dest->spark = NULL;
	return plugin_spark_complete(cmd, spark);
}
static struct command_result *
fundchannel_complete_err(struct command *cmd,
			 const char *buf,
			 const jsmntok_t *error,
			 struct multifundchannel_destination *dest)
{
	struct multifundchannel_command *mfc = dest->mfc;
	struct plugin_spark_completion *spark;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p, dest %p: failed! fundchannel_complete %s: %.*s",
		   mfc, dest, dest->id,
		   error->end - error->start, buf + error->start);

	/*~ In theory we could fail the entire cmd right here, right now.

	Unfortunately, in order to fail the command, we need to clean up
	all the pending `fundchannel_start`ed nodes by executing
	`fundchannel_cancel`.

	And as of this writing, `fundchannel_complete` has higher priority
	than `fundchannel_cancel`.
	Meaning that if we do `fundchannel_cancel` at the same time that
	another spark does `fundchannel_complete`, then the
	`fundchannel_complete` can win and the `fundchannel_cancel` will
	fail, and the peer will still expect the channel funding to
	push through.

	Thus, we have to store the fact that the `fundchannel_complete`
	command failed, and only actually perform the cleanup later,
	when all sparks have finished `fundchannel_complete`.
	*/

	dest->fundchannel_start_state = multifundchannel_complete_failed;
	dest->error = json_strdup(mfc, buf, error);

	spark = dest->spark;
	dest->spark = NULL;
	return plugin_spark_complete(cmd, spark);
}

static struct command_result *
perform_txsend(struct multifundchannel_command *mfc);

static struct command_result *
after_fundchannel_complete(struct command *cmd,
			   struct multifundchannel_command *mfc)
{
	unsigned int i;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: fundchannel_complete sparks done.", mfc);

	/* Check if any fundchannel_complete failed.  */
	for (i = 0; i < tal_count(mfc->destinations); ++i) {
		struct multifundchannel_destination *dest;
		dest = &mfc->destinations[i];

		assert(dest->fundchannel_start_state == multifundchannel_started
		    || dest->fundchannel_start_state == multifundchannel_complete_failed);

		if (dest->fundchannel_start_state != multifundchannel_complete_failed)
			continue;

		/* One of them failed, oh no.
		Forward the error.
		*/
		plugin_log(mfc->cmd->plugin, LOG_DBG,
			   "mfc %p, dest %p: "
			   "fundchannel_complete failure being forwarded.",
			   mfc, dest);
		return mfc_err_raw(mfc, dest->error);
	}

	return perform_txsend(mfc);
}

/*---------------------------------------------------------------------------*/
/*~
Finally with everything set up correctly we `txsend` the
funding transaction.
*/

static struct command_result *
after_txsend(struct command *cmd,
	     const char *buf,
	     const jsmntok_t *result,
	     struct multifundchannel_command *mfc);

static struct command_result *
perform_txsend(struct multifundchannel_command *mfc)
{
	unsigned int i;
	struct bitcoin_txid *txid;
	struct out_req *req;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: txsend.", mfc);

	/*~ Now mark all destinations as being done.
	Why mark it now *before* doing `txsend` rather than after?
	Because `txsend` will do approximately this:

	1.  `txsend` launches `bitcoin-cli`.
	2.  `bitcoin-cli` connects to a `bitcoind` over JSON-RPC
	    over HTTP.
	3.  `bitcoind` validates the transactions and puts it int
	    its local mempool.
	4.  `bitcoind` tells `bitcoin-cli` it all went well.
	5.  `bitcoin-cli` tells `txsend` it all went well.

	If some interruption or problem occurs between steps 3
	and 4, then the transaction is already in some node
	mempool and will likely be broadcast, but `txsend` has
	failed.

	And so we have to mark the channels as being "done"
	*before* we do `txsend`.
	If not, if we error on `txsend`, that would cause us to
	`fundchannel_cancel` all the peers, but that is risky,
	as, the funding transaction could still have been
	broadcast and the channels funded.

	That is, we treat `txsend` failure as a possible
	false negative.
	*/
	for (i = 0; i < tal_count(mfc->destinations); ++i) {
		struct multifundchannel_destination *dest;
		dest = &mfc->destinations[i];
		dest->fundchannel_start_state = multifundchannel_done;
	}

	/* Move responsibility of txid to this function.
	If txsend fails, the inputs are unreserved and
	there is no need to txdiscard it later.
	*/
	txid = tal_steal(tmpctx, mfc->txid);
	mfc->txid = NULL;

	req = jsonrpc_request_start(mfc->cmd->plugin,
				    mfc->cmd,
				    "txsend",
				    &after_txsend,
				    &mfc_forward_error,
				    mfc);
	json_add_string(req->js, "txid",
			type_to_string(tmpctx, struct bitcoin_txid,
				       txid));
	return send_outreq(mfc->cmd->plugin, req);
}

static struct command_result *
multifundchannel_finished(struct multifundchannel_command *mfc);

static struct command_result *
after_txsend(struct command *cmd,
	     const char *buf,
	     const jsmntok_t *result,
	     struct multifundchannel_command *mfc)
{
	const jsmntok_t *tx_tok;
	const jsmntok_t *txid_tok;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: txsend done.", mfc);

	tx_tok = json_get_member(buf, result, "tx");
	if (!tx_tok)
		plugin_err(cmd->plugin,
			   "txsend response has no 'tx': %.*s",
			   result->end - result->start,
			   buf + result->start);
	mfc->final_tx = json_strdup(mfc, buf, tx_tok);

	txid_tok = json_get_member(buf, result, "txid");
	if (!txid_tok)
		plugin_err(cmd->plugin,
			   "txsend response has no 'txid': %.*s",
			   result->end - result->start,
			   buf + result->start);
	mfc->final_txid = json_strdup(mfc, buf, txid_tok);

	return multifundchannel_finished(mfc);
}

/*---------------------------------------------------------------------------*/
/*~
And finally we are done, after a thousand lines or so of code.
*/

static struct command_result *
multifundchannel_finished(struct multifundchannel_command *mfc)
{
	unsigned int i;
	struct json_stream *out;

	plugin_log(mfc->cmd->plugin, LOG_DBG,
		   "mfc %p: done.", mfc);

	out = jsonrpc_stream_success(mfc->cmd);
	json_add_string(out, "tx", mfc->final_tx);
	json_add_string(out, "txid", mfc->final_txid);
	json_array_start(out, "channel_id");
	for (i = 0; i < tal_count(mfc->destinations); ++i)
		json_add_string(out, NULL, mfc->destinations[i].channel_id);
	json_array_end(out);

	return mfc_finished(mfc, out);
}

/*-----------------------------------------------------------------------------
Command Entry Point
-----------------------------------------------------------------------------*/

/* Entry function.  */
static struct command_result *
json_multifundchannel(struct command *cmd,
		      const char *buf,
		      const jsmntok_t *params)
{
	const jsmntok_t *json_destinations;
	const char *feerate_str;
	u32 *minconf;
	const char *utxos_str;

	struct multifundchannel_command *mfc;

	struct command_result *result;

	if (!param(cmd, buf, params,
		   p_req("destinations", param_tok, &json_destinations),
		   p_opt("feerate", param_string, &feerate_str),
		   p_opt_def("minconf", param_number, &minconf, 1),
		   p_opt("utxos", param_string, &utxos_str),
		   NULL))
		return command_param_failed();

	mfc = tal(cmd, struct multifundchannel_command);
	mfc->cmd = cmd;
	mfc->destinations = NULL;
	mfc->sparks = tal_arr(mfc, struct plugin_spark *, 0);
	mfc->feerate_str = feerate_str;
	mfc->minconf = *minconf;
	mfc->utxos_str = utxos_str;
	mfc->has_all = false;
	mfc->txid = NULL;
	mfc->final_tx = NULL;
	mfc->final_txid = NULL;

	/* If it returns non-null, the error is reported already.  */
	result = create_destinations_array(mfc,
					   &mfc->destinations,
					   buf, json_destinations,
					   &mfc->has_all);
	if (result)
		return result;

	tal_resize(&mfc->sparks, tal_count(mfc->destinations));

	return perform_multifundchannel(mfc);
}

const struct plugin_command multifundchannel_commands[] = {
	{
		"multifundchannel",
		"channels",
		"Fund channels to {destinations}, which is an array of "
		"objects containing peer {id}, {amount}, and optional "
		"{announce} and {push_msat}.  "
		"A single transaction will be used to fund all the "
		"channels.  "
		"Use {feerate} for the transaction, select outputs that are "
		"buried {minconf} blocks deep, or specify a set of {utxos}.",
		"Fund multiple channels at once.",
		json_multifundchannel
	}
};
const unsigned int num_multifundchannel_commands =
	ARRAY_SIZE(multifundchannel_commands);

void multifundchannel_init(struct plugin *plugin,
			   const char *buf UNUSED,
			   const jsmntok_t *config UNUSED)
{
	/* Save our chainparams.  */
	const char *network_name;

	network_name = rpc_delve(tmpctx, plugin, "listconfigs",
				 take(json_out_obj(NULL, "config",
						   "network")),
				 ".network");
	chainparams = chainparams_for_network(network_name);
}

int main(int argc, char **argv)
{
	setup_locale();
	plugin_main(argv,
		    &multifundchannel_init, PLUGIN_RESTARTABLE,
		    NULL,
		    multifundchannel_commands, num_multifundchannel_commands,
		    NULL, 0, NULL, 0, NULL);
}
