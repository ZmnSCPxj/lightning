#include "withdraw.h"
#include <common/amount.h>
#include <common/json_tok.h>
#include <common/json_stream.h>
#include <common/type_to_string.h>

/*-----------------------------------------------------------------------------
Commands
-----------------------------------------------------------------------------*/

static struct command_result *json_withdraw(struct command *cmd,
					    const char *buf,
					    const jsmntok_t *params);

const struct plugin_command withdraw_commands[] = {
	{
		"withdraw",
		"bitcoin",
		"Send to {destination} address {satoshi} (or 'all') amount "
		"via Bitcoin transaction, at optional {feerate}",
		"Send funds from the internal wallet "
		"to the specified address.  "
		"Either specify a number of satoshis to send or 'all' "
		"to sweep all funds in the internal wallet to the address.  "
		"Only use outputs that have at least "
		"{minconf} confirmations.  "
		"Use the {utxos} indicated if provided."
	}
};
const size_t num_withdraw_commands = ARRAY_SIZE(withdraw_commands);

/*-----------------------------------------------------------------------------
Entry point
-----------------------------------------------------------------------------*/
/*~ The `withdraw` command just delegates completely to `multiwithdraw`.  */

/** json_withdraw
 *
 * @brief Implement withdrawing to a single address.
 *
 * @desc The user requests a withdrawal.
 * Parse the request, then delegate to `multiwithdraw`.
 */
static struct command_result *json_withdraw(struct command *cmd,
					    const char *buf,
					    const jsmntok_t *params)
{
	const char *destination;
	struct amount_sat *satoshi;
	const char *feerate;
	u32 *minconf;
	const char *utxos;

	struct out_req *req;
	struct json_stream *js;

	if (!param(cmd, buffer, params,
		   p_req("destination", param_string, &destination),
		   p_req("satoshi", param_sat_or_all, &satoshi),
		   p_opt("feerate", param_string, &feerate),
		   p_opt("minconf", param_number, &miconf),
		   p_opt("utxos", param_string, &utxos),
		   NULL))
		return command_param_failed();

	req = jsonrpc_request_start(cmd->plugin, cmd,
				    "multiwithdraw",
				    &forward_result,
				    &forward_error,
				    cmd);
	js = req->js;

	json_array_start(js, "outputs");
	json_object_start(js, NULL);
	if (amount_sat_eq(*satoshi, AMOUNT_SAT(-1ULL)))
		json_add_string(js, destination, "all");
	else
		json_add_string(js, destination,
				type_to_string(tmpctx, struct amount_sat,
					       satoshi));
	json_object_end(js);
	json_array_end(js);

	if (feerate)
		json_add_string(js, "feerate", feerate);
	if (minconf)
		json_add_u32(js, "minconf", *minconf);
	if (utxos)
		json_add_jsonstr(js, "utxos", utxos);

	return send_outreq(cmd->plugin, req);
}
