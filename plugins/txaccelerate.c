#include "txaccelerate_steps.h"
#include <bitcoin/tx.h>
#include <ccan/list/list.h>
#include <ccan/tal/str/str.h>
#include <ccan/time/time.h>
#include <common/amount.h>
#include <common/json.h>
#include <common/json_helpers.h>
#include <common/json_tok.h>
#include <common/type_to_string.h>

/*-----------------------------------------------------------------------------
Command
-----------------------------------------------------------------------------*/

static
struct command_result *json_txaccelerate(struct command *cmd,
					 const char *buf,
					 const jsmntok_t *params);

struct plugin_command txaccelerate_commands[] = {
	{
		"txaccelerate",
		"bitcoin",
		"Accelerate the given {txid} until a version of it confirms, "
		"spending only up to {max_acceptable_fee}.",
		"Increase the effective fees for confirming some version of "
		"the given {txid}, blocking until it is confirmed.  "
		"The txid that actually gets confirmed may "
		"or may not be different from the original txid.  "
		"Only spend up to {max_acceptable_fee}.  "
		"Increase fees according to {aggression} percentage.",
		&json_txaccelerate
	}
};

/*-----------------------------------------------------------------------------
Objects
-----------------------------------------------------------------------------*/

struct txaccelerate_command {
	/* Core plugin command.  */
	struct command *cmd;
	/* ID of the above command, stored here for debug prints.  */
	u64 id;

	/* Original transaction ID.  */
	struct bitcoin_txid *txid;
	/* Max extra payment to make the transaction confirm.  */
	struct amount_sat *max_acceptable_fee;
	/* When computing how much to actually pay as fee, we go higher than
	 * what txaccelerate_estimate returns.
	 * Aggression is how much we overpay above the estimate from
	 * txaccelerate_estimate.
	 * For example, if txaccelerate_estimate returns E, and the max
	 * acceptable fee above is F, and this aggression is 10%, then
	 * we pay a fee that is 10% along the way from E to F, or:
	 *     actual = (F - E) * 10% + E
	 * The aggression below is in absolute.
	 * i.e. 10% means .aggression = 0.1
	 */
	double aggression;

	/* The blockheight from getinfo.  */
	u32 blockheight;
	/* The txacc_id from the txaccelerate_start.  */
	const char *txacc_id;
	/* The most recent results from txaccelerate_estimate/start.
	txacc_total_fee also doubles as the value to pass into
	txaccelerate_execute.
	*/
	struct amount_sat txacc_total_fee;
	struct amount_sat txacc_delta_fee;
	struct amount_sat txacc_max_fee;

	/* Flag set if we ever managed to accelerate at least once.  */
	bool have_accelerated;
	/* The latest total_fee we passed to txaccelerate_start.  */
	struct amount_sat final_fee;

	/* Logs.  */
	struct list_head logs;
	struct txaccelerate_log *latest_log;
};

/* A single log entry.  */
struct txaccelerate_log {
	/* Off txaccelerate_command::logs.  */
	struct list_node list;

	/* Blockheight and time.  */
	u32 blockheight;
	struct timeabs time;

	/* Data from most recent txaccelerate_estimate/start.  */
	struct amount_sat total_fee;
	struct amount_sat delta_fee;
	struct amount_sat max_fee;

	/* What we decided to do.  */
	char const *comment;
};

/*-----------------------------------------------------------------------------
Entry Point
-----------------------------------------------------------------------------*/

/* Just a wrapper around json_to_txid.
 * FIXME: Factor out into common/json_helpers.c, same code exists
 * in lightningd/json.c
 */
static
struct command_result *param_txid(struct command *cmd,
				  const char *name,
				  const char *buffer,
				  const jsmntok_t *tok,
				  struct bitcoin_txid **txid)
{
	*txid = tal(cmd, struct bitcoin_txid);
	if (json_to_txid(buffer, tok, *txid))
		return NULL;
	return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
			    "'%s' should be txid, not '%.*s'",
			    name, json_tok_full_len(tok),
			    json_tok_full(buffer, tok));
}

static struct command_result *acc_begin(struct txaccelerate_command *acc);
/* Update the blockheight field of the acc.  */
static struct command_result *
acc_getblockheight(struct txaccelerate_command *acc,
		   struct command_result *(*next)(struct txaccelerate_command *acc));

static
struct command_result *json_txaccelerate(struct command *cmd,
					 const char *buf,
					 const jsmntok_t *params)
{
	struct txaccelerate_command *acc;
	u64 *aggression_millionths;

	acc = tal(cmd, struct txaccelerate_command);
	if (!param(cmd, buf, params,
		   p_req("txid", param_txid, &acc->txid),
		   p_req("max_acceptable_fee", param_sat,
			 &acc->max_acceptable_fee),
		   p_opt_def("aggression", param_millionths,
			     &aggression_millionths,
			     /* Default is 10%.  */
			     (10) * 1000000),
		   NULL))
		return command_param_failed();

	acc->cmd = cmd;
	acc->id = *cmd->id;
	acc->aggression = ((double) *aggression_millionths) / (1000000.0 / 100.0);
	acc->have_accelerated = false;

	return acc_getblockheight(acc, &acc_begin);
}

/*-----------------------------------------------------------------------------
Get block height
-----------------------------------------------------------------------------*/

struct txaccelerate_getblockheight {
	struct txaccelerate_command *acc;
	struct command_result *(*next)(struct txaccelerate_command *acc);
};

static struct command_result *
acc_getblockheight_getinfo_ok(struct command *cmd,
			      const char *buf,
			      const jsmntok_t *result,
			      struct txaccelerate_getblockheight *);
static struct command_result *
acc_getblockheight_getinfo_err(struct command *cmd,
			       const char *buf,
			       const jsmntok_t *result,
			       struct txaccelerate_getblockheight *);

static struct command_result *
acc_getblockheight(struct txaccelerate_command *acc,
		   struct command_result *(*next)(struct txaccelerate_command *acc))
{
	struct txaccelerate_getblockheight *acc_gbh;
	struct out_req *req;

	acc_gbh = tal(acc, struct txaccelerate_getblockheight);
	acc_gbh->acc = acc;
	acc_gbh->next = next;

	req = jsonrpc_request_start(acc->cmd->plugin, acc->cmd,
				    "getinfo",
				    &acc_getblockheight_getinfo_ok,
				    &acc_getblockheight_getinfo_err,
				    acc_gbh);
	return send_outreq(acc->cmd->plugin, req);
}

static struct command_result *
acc_getblockheight_getinfo_ok(struct command *cmd,
			      const char *buf,
			      const jsmntok_t *result,
			      struct txaccelerate_getblockheight *acc_gbh)
{
	const jsmntok_t *blockheight_tok;

	blockheight_tok = json_get_member(buf, result, "blockheight");
	if (!blockheight_tok)
		plugin_err(cmd->plugin,
			   "getinfo gave no 'blockheight'? %.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));

	if (!json_to_u32(buf, blockheight_tok, &acc_gbh->acc->blockheight))
		plugin_err(cmd->plugin,
			   "getinfo gave non-unsigned-32-bit 'blockheight'? %.*s",
			   json_tok_full_len(blockheight_tok),
			   json_tok_full(buf, blockheight_tok));

	/* Schedule for freeing.  */
	tal_steal(tmpctx, acc_gbh);
	/* Continue.  */
	return acc_gbh->next(acc_gbh->acc);
}
static struct command_result *
acc_getblockheight_getinfo_err(struct command *cmd,
			       const char *buf,
			       const jsmntok_t *result,
			       struct txaccelerate_getblockheight *acc_gbh)
{
	/* getinfo failing should never happen.  */
	plugin_err(cmd->plugin, "getinfo failed??? %.*s",
		   json_tok_full_len(result),
		   json_tok_full(buf, result));
}

/*-----------------------------------------------------------------------------
Begin Acceleration
-----------------------------------------------------------------------------*/

static struct command_result *
txaccelerate_start_ok(struct command *cmd,
		      const char *buf,
		      const jsmntok_t *result,
		      struct txaccelerate_command *acc);

static struct command_result *acc_begin(struct txaccelerate_command *acc)
{
	struct out_req *req;

	plugin_log(acc->cmd->plugin, LOG_DBG,
		   "acc %"PRIu64": txaccelerate_start %s.",
		   acc->id, type_to_string(tmpctx, struct bitcoin_txid,
					   acc->txid));

	req = jsonrpc_request_start(acc->cmd->plugin, acc->cmd,
				    "txaccelerate_start",
				    &txaccelerate_start_ok,
				    &forward_error,
				    acc);
	json_add_txid(req->js, "txid", acc->txid);
	return send_outreq(acc->cmd->plugin, req);
}

/** update_txaccelerate_result
 *
 * @brief Update the txacc_total_fee and
 * txacc_max_fee from the result of
 * txaccelerate_start or txaccelerate_estimate.
 */
static void update_txaccelerate_result(struct txaccelerate_command *acc,
				       const char *buf,
				       const jsmntok_t *result)
{
	const jsmntok_t *total_fee_tok;
	const jsmntok_t *delta_fee_tok;
	const jsmntok_t *max_fee_tok;

	total_fee_tok = json_get_member(buf, result, "total_fee");
	if (!total_fee_tok)
		plugin_err(acc->cmd->plugin,
			   "'txaccelerate_*' did not return 'total_fee'? "
			   "%.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));
	if (!json_to_sat(buf, total_fee_tok, &acc->txacc_total_fee))
		plugin_err(acc->cmd->plugin,
			   "'txaccelerate_*' returned unparseable 'total_fee'? "
			   "%.*s",
			   json_tok_full_len(total_fee_tok),
			   json_tok_full(buf, total_fee_tok));

	delta_fee_tok = json_get_member(buf, result, "delta_fee");
	if (!delta_fee_tok)
		plugin_err(acc->cmd->plugin,
			   "'txaccelerate_*' did not return 'delta_fee'? "
			   "%.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));
	if (!json_to_sat(buf, delta_fee_tok, &acc->txacc_delta_fee))
		plugin_err(acc->cmd->plugin,
			   "'txaccelerate_*' returned unparseable 'delta_fee'? "
			   "%.*s",
			   json_tok_full_len(delta_fee_tok),
			   json_tok_full(buf, delta_fee_tok));

	max_fee_tok = json_get_member(buf, result, "max_fee");
	if (!max_fee_tok)
		plugin_err(acc->cmd->plugin,
			   "'txaccelerate_*' did not return 'max_fee'? "
			   "%.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));
	if (!json_to_sat(buf, max_fee_tok, &acc->txacc_max_fee))
		plugin_err(acc->cmd->plugin,
			   "'txaccelerate_*' returned unparseable 'max_fee'? "
			   "%.*s",
			   json_tok_full_len(max_fee_tok),
			   json_tok_full(buf, max_fee_tok));
}

static struct command_result *acc_loop(struct txaccelerate_command *acc);

static struct command_result *
txaccelerate_start_ok(struct command *cmd,
		      const char *buf,
		      const jsmntok_t *result,
		      struct txaccelerate_command *acc)
{
	const jsmntok_t *txacc_id_tok;

	txacc_id_tok = json_get_member(buf, result, "txacc_id");
	if (!txacc_id_tok)
		plugin_err(cmd->plugin,
			   "'txaccelerate_start' did not return 'txacc_id'? "
			   "%.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));

	acc->txacc_id = json_strdup(acc, buf, txacc_id_tok);
	update_txaccelerate_result(acc, buf, result);

	plugin_log(acc->cmd->plugin, LOG_DBG,
		   "acc %"PRIu64": txaccelerate_start txacc_id: %s.",
		   acc->id, acc->txacc_id);
	return acc_loop(acc);
}

/*-----------------------------------------------------------------------------
Acceleration Loop
-----------------------------------------------------------------------------*/

static void acc_new_log(struct txaccelerate_command *acc);

static struct command_result *
acc_wait_and_retry(struct txaccelerate_command *acc);
static struct command_result *
acc_fail_never_accelerated(struct txaccelerate_command *acc);
static struct command_result *
acc_execute(struct txaccelerate_command *acc);

/* txaccelerate_estimate / txaccelerate_start has just been called,
 * and the total_fee/delta_fee/max_fee entries are very fresh.
 */
static struct command_result *acc_loop(struct txaccelerate_command *acc)
{
	u64 total_fee_satoshi, max_acceptable_fee_satoshi;

	/* Logs!  */
	plugin_log(acc->cmd->plugin, LOG_DBG,
		   "acc %"PRIu64": loop top. "
		   "estimates: total = %s, delta = %s, max = %s. "
		   "limit = %s.",
		   acc->id,
		   type_to_string(tmpctx, struct amount_sat,
				  &acc->txacc_total_fee),
		   type_to_string(tmpctx, struct amount_sat,
				  &acc->txacc_delta_fee),
		   type_to_string(tmpctx, struct amount_sat,
				  &acc->txacc_max_fee),
		   type_to_string(tmpctx, struct amount_sat,
				  acc->max_acceptable_fee));
	acc_new_log(acc);

	/* If delta_fee is 0 and total_fee == max_fee, then we can no
	 * longer do anything!
	 * Sleep for a while --- *maybe* something will happen that will
	 * let us accelerate the transaction later.
	 */
	if (amount_sat_eq(acc->txacc_delta_fee, AMOUNT_SAT(0)) &&
	    amount_sat_eq(acc->txacc_total_fee, acc->txacc_max_fee)) {
		acc->latest_log->comment = "Cannot accelerate now, "
					   "will sleep.";
		return acc_wait_and_retry(acc);
	}

	/* If total_fee from the estimation is greater than our max
	 * acceptable, we cannot execute the acceleration.
	 */
	if (amount_sat_greater(acc->txacc_total_fee,
			       *acc->max_acceptable_fee)) {
		char const *max;
		max = type_to_string(tmpctx,
				     struct amount_sat,
				     acc->max_acceptable_fee);

		/* If we have never accelerated, then fail it now.  */
		if (!acc->have_accelerated) {
			acc->latest_log->comment = tal_fmt(acc,
							   "Max acceptable %s "
							   "too low for *any* "
							   "acceleration, "
							   "failing.",
							   max);
			return acc_fail_never_accelerated(acc);
		}

		/* Otherwise, sleep for a while.  */
		acc->latest_log->comment = tal_fmt(acc,
						   "Max acceptable %s "
						   "reached, wil sleep.",
						   max);
		return acc_wait_and_retry(acc);
	}

	/* Extract total_fee.  */
	total_fee_satoshi = acc->txacc_total_fee.satoshis; /* Raw: multiply by double later.  */
	max_acceptable_fee_satoshi = acc->max_acceptable_fee->satoshis; /* Raw: multiply by double later.  */

	/* And tweak by aggression.  */
	total_fee_satoshi = (max_acceptable_fee_satoshi - total_fee_satoshi) * acc->aggression + total_fee_satoshi;

	/* Now store it in the object again.  */
	acc->txacc_total_fee = AMOUNT_SAT(total_fee_satoshi);
	/* Cap it.  */
	if (amount_sat_greater(acc->txacc_total_fee, acc->txacc_max_fee))
		acc->txacc_total_fee = acc->txacc_max_fee;

	/* Now execute.  */
	return acc_execute(acc);
}

/*-----------------------------------------------------------------------------
Acceleration Execution
-----------------------------------------------------------------------------*/

static struct command_result *
acc_success(struct txaccelerate_command *acc);
static struct command_result *
acc_reestimate(struct txaccelerate_command *acc);

static struct command_result *
txaccelerate_execute_ok(struct command *cmd,
			const char *buf,
			const jsmntok_t *result,
			struct txaccelerate_command *acc);
static struct command_result *
txaccelerate_execute_err(struct command *cmd,
			 const char *buf,
			 const jsmntok_t *error,
			 struct txaccelerate_command *acc);

static struct command_result *
acc_execute(struct txaccelerate_command *acc)
{
	struct out_req *req;

	plugin_log(acc->cmd->plugin, LOG_DBG,
		   "acc %"PRIu64": txaccelerate_execute %s.",
		   acc->id,
		   type_to_string(tmpctx, struct amount_sat,
				  &acc->txacc_total_fee));

	req = jsonrpc_request_start(acc->cmd->plugin, acc->cmd,
				    "txaccelerate_execute",
				    &txaccelerate_execute_ok,
				    &txaccelerate_execute_err,
				    acc);
	json_add_string(req->js, "txacc_id", acc->txacc_id);
	json_add_string(req->js, "total_fee",
			type_to_string(tmpctx, struct amount_sat,
				       &acc->txacc_total_fee));
	return send_outreq(acc->cmd->plugin, req);
}

static errcode_t get_code(const char *buf, const jsmntok_t *error)
{
	const jsmntok_t *code_tok;
	errcode_t code;

	/* Extract code.  */
	code_tok = json_get_member(buf, error, "code");
	if (!code_tok)
		plugin_err(cmd->plugin,
			   "error did not return 'code'? %.*s",
			   json_tok_full_len(error),
			   json_tok_full(buf, error));
	if (!json_to_errcode(buf, code_tok, &code))
		plugin_err(cmd->plugin,
			   "error did not return parseable 'code'? %.*s",
			   json_tok_full_len(code_tok),
			   json_tok_full(buf, code_tok));
}

static struct command_result *
txaccelerate_execute_err(struct command *cmd,
			 const char *buf,
			 const jsmntok_t *error,
			 struct txaccelerate_command *acc)
{
	errcode_t code;

	plugin_log(acc->cmd->plugin, LOG_DBG,
		   "acc %"PRIu64": txaccelerate_execute failed: %.*s.",
		   acc->id,
		   json_tok_full_len(error),
		   json_tok_full(buf, error));

	code = get_code(buf, error);

	/* If not found, that actually means we *succeeded*, yey!
	txacc_id is automatically managed and if the transaction it
	is accelerating is confirmed at least once, the txacc_id is
	deleted and will no longer be found.
	*/
	if (code == TXACCELERATE_ID_NOT_FOUND)
		return acc_success(acc);
	/* We can get spurious FUND_CANNOT_AFFORD, which means basically
	that some other client spent coins we were considering to be
	useable in the previous estimation.
	We should instead re-request an estimate.
	*/
	if (code == FUND_CANNOT_AFFORD)
		return acc_reestimate(acc);

	/* Other errors should not happen; forward if so.  */
	return forward_error(cmd, buf, error, acc);
}

static struct command_result *
txaccelerate_execute_ok(struct command *cmd,
			const char *buf,
			const jsmntok_t *result,
			struct txaccelerate_command *acc)
{
	plugin_log(acc->cmd->plugin, LOG_DBG,
		   "acc %"PRIu64": txaccelerate_execute ok.",
		   acc->id);

	/* We successfully did an acceleration attempt.  */
	acc->have_accelerated = true;
	acc->final_fee = acc->txacc_total_fee;

	return acc_wait_and_retry(acc);
}

/*-----------------------------------------------------------------------------
Wait for blockchain events
-----------------------------------------------------------------------------*/

static struct command_result *
acc_waitblockheight_done(struct command *cmd,
			 const char *buf,
			 const jsmntok_t *result,
			 struct txaccelerate_command *acc);

static struct command_result *
acc_wait_and_retry(struct txaccelerate_command *acc)
{
	struct out_req *req;

	plugin_log(acc->cmd->plugin, LOG_DBG,
		   "acc %"PRIu64": sleep and wait for new blocks.",
		   acc->id);

	req = jsonrpc_request_start(acc->cmd->plugin, acc->cmd,
				    "waitblockheight",
				    &acc_waitblockheight_done,
				    &acc_waitblockheight_done,
				    acc);
	json_add_u32(req->js, "blockheight", acc->blockheight + 1);
	json_add_u32(req->js, "timeout", 60);
	return send_outreq(acc->cmd->plugin, req);
}

static struct command_result *
acc_waitblockheight_done(struct command *cmd UNUSED,
			 const char *buf UNUSED,
			 const jsmntok_t *result UNUSED,
			 struct txaccelerate_command *acc)
{
	/* Does not matter if waitblockheight succeeds or fails,
	we just re-query getinfo for the blockheight.
	*/

	plugin_log(acc->cmd->plugin, LOG_DBG,
		   "acc %"PRIu64": wake up!",
		   acc->id);

	/* FIXME: We should probably expose a txaccelerate_wait
	method as well, and call that in parallel with the
	txaccelerate.
	When that method returns, we then close this command
	regardless of what this command is doing.
	This requires fixes inside libplugin to allow a
	pending out_req to have the calling command freed
	without crashing when the out_req returns.
	*/

	return acc_getblockheight(acc, &acc_reestimate);
}

/*-----------------------------------------------------------------------------
Reestimate
-----------------------------------------------------------------------------*/

static struct command_result *
acc_reestimate(struct txaccelerate_command *acc)
{
}
