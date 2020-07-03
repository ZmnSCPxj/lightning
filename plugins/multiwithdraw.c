#include "multiwithdraw.h"
#include <ccan/array_size/array_size.h>
#include <common/amount.h>
#include <common/json.h>
#include <common/json_helpers.h>
#include <common/json_stream.h>
#include <common/type_to_string.h>

/*-----------------------------------------------------------------------------
Commands
-----------------------------------------------------------------------------*/

static struct command_result *json_multiwithdraw(struct command *cmd,
						 const char *buf,
						 const jsmntok_t *params);

const struct plugin_command multiwithdraw_commands[] = {
	{
		"multiwithdraw",
		"bitcoin",
		"Withdraw to multiple destinations",
		"Send funds from the internal wallet to "
		"multiple {outputs}, an array of "
		"{address: amount} objects.  "
		"Send at optional {feerate}, "
		"selecting inputs with at least {minconf} confirmations, "
		"or the exact {utxos} to spend.",
		&json_multiwithdraw
	}
};
const size_t num_multiwithdraw_commands = ARRAY_SIZE(multiwithdraw_commands);

/*-----------------------------------------------------------------------------
Entry Point
-----------------------------------------------------------------------------*/

/* Represents a single address-amount pair.  */
struct multiwithdraw_output {
	/* The type struct bitcoin_address is not appropriate as it only
	 * encodes a P2PKH.
	 * We could encode it in a scriptPubKey, but to what end?
	 */
	const char *addr;
	struct amount_sat amount;
	/* If true, user specified "all".  */
	bool all;
};

/* Represents a multiwithdraw command.  */
struct multiwithdraw_command {
	struct command *cmd;

	/* The outputs to send to.  */
	struct multiwithdraw_output *outputs;
	/* The feerate specification.  */
	const char *feerate;
	/* The minimum number of confirmations.  */
	u32 *minconf;
	/* The utxos to actually use.  */
	const char *utxos;

	/* The transaction.  */
	struct bitcoin_txid *txid;

	/* Command ID.  */
	u64 id;
};

static struct command_result *
param_outputs(struct command *cmd,
	      const char *name,
	      const char *buf,
	      const jsmntok_t *tok,
	      struct multiwithdraw_output **outputs);

static struct command_result *mwc_start(struct multiwithdraw_command *mwc);

static struct command_result *json_multiwithdraw(struct command *cmd,
						 const char *buf,
						 const jsmntok_t *params)
{
	struct multiwithdraw_command *mwc;

	mwc = tal(cmd, struct multiwithdraw_command);

	if (!param(cmd, buf, params,
		   p_req("outputs", param_outputs, &mwc->outputs),
		   p_opt("feerate", param_string, &mwc->feerate),
		   p_opt("minconf", param_number, &mwc->minconf),
		   p_opt("utxos", param_string, &mwc->utxos),
		   NULL))
		return command_param_failed();

	mwc->cmd = cmd;
	mwc->txid = NULL;
	mwc->id = *cmd->id;

	return mwc_start(mwc);
}

/*-----------------------------------------------------------------------------
Outputs parameter parsing
-----------------------------------------------------------------------------*/

static struct command_result *
param_outputs(struct command *cmd,
	      const char *name,
	      const char *buf,
	      const jsmntok_t *tok,
	      struct multiwithdraw_output **outputs)
{
	size_t i;
	const jsmntok_t *output_tok;
	bool has_all = false;

	if (tok->type != JSMN_ARRAY)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "'%s' should be an array.",
				    name);

	if (tok->size == 0)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "'%s' should not be an empty array.",
				    name);

	*outputs = tal_arr(cmd, struct multiwithdraw_output, tok->size);

	json_for_each_arr(i, output_tok, tok) {
		struct multiwithdraw_output *output;
		const jsmntok_t *addr_tok;
		const jsmntok_t *amount_tok;

		output = &(*outputs)[i];

		if (output_tok->type != JSMN_OBJECT)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "'%s' should contain "
					    "objects, not '%.*s'",
					    name,
					    json_tok_full_len(output_tok),
					    json_tok_full(buf, output_tok));
		if (output_tok->size != 1)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "'%s' entry should be "
					    "objects with exactly one key, "
					    "not '%.*s'",
					    name,
					    json_tok_full_len(output_tok),
					    json_tok_full(buf, output_tok));
		/* Key should be string, that should already be checked
		by the parser.
		*/
		addr_tok = output_tok + 1;
		output->addr = json_strdup(cmd, buf, addr_tok);
		/* Value should be string that is parseable as a
		struct amount_sat.
		*/
		amount_tok = output_tok + 2;
		output->all = false;
		if (json_tok_streq(buf, amount_tok, "all")) {
			if (has_all)
				return command_fail(cmd,
						    JSONRPC2_INVALID_PARAMS,
						    "'%s' should only have "
						    "one entry with \"all\", "
						    "not '%.*s'",
						    name,
						    json_tok_full_len(tok),
						    json_tok_full(buf, tok));
			has_all = true;
			output->all = true;
		} else if (!json_to_sat(buf, amount_tok, &output->amount))
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "'%s' entry should have "
					    "amount as value, not '%.*s'",
					    name,
					    json_tok_full_len(amount_tok),
					    json_tok_full(buf, amount_tok));
	}

	/* FIXME: In theory we could have multiple outputs, exactly
	one of which is indicated as "all", meaning that output gets
	all of the remaining money.
	For now if "all" is indicated then only one output is allowed.
	*/
	if (has_all && tok->size != 1)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "'%s' indicates \"all\" but there "
				    "are multiple outputs: '%.*s'.",
				    name,
				    json_tok_full_len(tok),
				    json_tok_full(buf, tok));

	return NULL;
}

/*-----------------------------------------------------------------------------
multiwithdraw error forwarding
-----------------------------------------------------------------------------*/

/*~ Nothing particularly special, just making it clear in the logs that
the command failed.
*/
static struct command_result *
mwc_forward_error(struct command *cmd,
		  const char *buf,
		  const jsmntok_t *error,
		  struct multiwithdraw_command *mwc)
{
	plugin_log(mwc->cmd->plugin, LOG_DBG,
		   "mwc %"PRIu64": failed, forwarding error: %.*s",
		   mwc->id,
		   json_tok_full_len(error),
		   json_tok_full(buf, error));

	return forward_error(cmd, buf, error, mwc);
}

/*-----------------------------------------------------------------------------
txprepare
-----------------------------------------------------------------------------*/

static struct command_result *
mwc_txprepare_ok(struct command *cmd,
		 const char *buf,
		 const jsmntok_t *result,
		 struct multiwithdraw_command *mwc);

static struct command_result *mwc_start(struct multiwithdraw_command *mwc)
{
	struct out_req *req;
	size_t i;

	plugin_log(mwc->cmd->plugin, LOG_DBG,
		   "mwc %"PRIu64": txprepare.", mwc->id);

	req = jsonrpc_request_start(mwc->cmd->plugin, mwc->cmd,
				    "txprepare",
				    &mwc_txprepare_ok,
				    &mwc_forward_error,
				    mwc);

	json_array_start(req->js, "outputs");
	for (i = 0; i < tal_count(mwc->outputs); ++i) {
		json_object_start(req->js, NULL);
		if (mwc->outputs[i].all)
			json_add_string(req->js, mwc->outputs[i].addr,
					"all");
		else
			json_add_string(req->js, mwc->outputs[i].addr,
					type_to_string(tmpctx,
						       struct amount_sat,
						       &mwc->outputs[i].amount));
		json_object_end(req->js);
	}
	json_array_end(req->js);

	if (mwc->feerate)
		json_add_string(req->js, "feerate", mwc->feerate);
	if (mwc->minconf)
		json_add_u32(req->js, "minconf", *mwc->minconf);
	if (mwc->utxos)
		json_add_jsonstr(req->js, "utxos", mwc->utxos);

	return send_outreq(mwc->cmd->plugin, req);
}

static struct command_result *mwc_finish(struct multiwithdraw_command *mwc);

static struct command_result *
mwc_txprepare_ok(struct command *cmd,
		 const char *buf,
		 const jsmntok_t *result,
		 struct multiwithdraw_command *mwc)
{
	const jsmntok_t *txid_tok;

	txid_tok = json_get_member(buf, result, "txid");
	if (!txid_tok)
		plugin_err(mwc->cmd->plugin,
			   "No 'txid' from 'txprepare'? %.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));
	mwc->txid = tal(mwc, struct bitcoin_txid);
	if (!bitcoin_txid_from_hex(buf + txid_tok->start,
				   txid_tok->end - txid_tok->start,
				   mwc->txid))
		plugin_err(mwc->cmd->plugin,
			   "'txid' from 'txprepare' not parseable? %.*s",
			   json_tok_full_len(txid_tok),
			   json_tok_full(buf, txid_tok));

	plugin_log(mwc->cmd->plugin, LOG_DBG,
		   "mwc %"PRIu64": txprepare ok: txid=%.*s",
		   mwc->id,
		   json_tok_full_len(txid_tok), json_tok_full(buf, txid_tok));

	return mwc_finish(mwc);
}

/*-----------------------------------------------------------------------------
txsend
-----------------------------------------------------------------------------*/

static struct command_result *
mwc_txsend_ok(struct command *cmd,
	      const char *buf,
	      const jsmntok_t *result,
	      struct multiwithdraw_command *mwc);

static struct command_result *mwc_finish(struct multiwithdraw_command *mwc)
{
	struct out_req *req;

	plugin_log(mwc->cmd->plugin, LOG_DBG,
		   "mwc %"PRIu64": txsend.", mwc->id);

	req = jsonrpc_request_start(mwc->cmd->plugin, mwc->cmd,
				    "txsend",
				    &mwc_txsend_ok,
				    &mwc_forward_error,
				    mwc);
	json_add_txid(req->js, "txid", mwc->txid);
	json_add_string(req->js, "annotate", "withdraw");
	return send_outreq(mwc->cmd->plugin, req);
}

static struct command_result *
mwc_txsend_ok(struct command *cmd,
	      const char *buf,
	      const jsmntok_t *result,
	      struct multiwithdraw_command *mwc)
{
	plugin_log(mwc->cmd->plugin, LOG_DBG,
		   "mwc %"PRIu64": succeeded.", mwc->id);

	return forward_result(cmd, buf, result, mwc);
}
