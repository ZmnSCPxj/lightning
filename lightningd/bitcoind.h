#ifndef LIGHTNING_LIGHTNINGD_BITCOIND_H
#define LIGHTNING_LIGHTNINGD_BITCOIND_H
#include "config.h"
#include <bitcoin/chainparams.h>
#include <bitcoin/tx.h>
#include <ccan/list/list.h>
#include <ccan/short_types/short_types.h>
#include <ccan/strmap/strmap.h>
#include <ccan/take/take.h>
#include <ccan/tal/tal.h>
#include <ccan/time/time.h>
#include <ccan/typesafe_cb/typesafe_cb.h>
#include <stdbool.h>

struct bitcoin_blkid;
struct bitcoin_tx_output;
struct block;
struct lightningd;
struct ripemd160;
struct bitcoin_tx;
struct bitcoin_block;
struct short_channel_id;
struct utxo;

struct bitcoind {
	/* Where to do logging. */
	struct log *log;

	/* Main lightningd structure */
	struct lightningd *ld;

	/* Is our Bitcoin backend synced?  If not, we retry. */
	bool synced;

	/* Ignore results, we're shutting down. */
	bool shutdown;

	struct list_head pending_getfilteredblock;

	/* Map each method to a plugin, so we can have multiple plugins
	 * handling different functionalities. */
	STRMAP(struct plugin *) pluginsmap;
};

/* A single outpoint in a filtered block */
struct filteredblock_outpoint {
	struct bitcoin_txid txid;
	u32 outnum;
	u32 txindex;
	const u8 *scriptPubKey;
	struct amount_sat amount;
};

/* A struct representing a block with most of the parts filtered out. */
struct filteredblock {
	struct bitcoin_blkid id;
	u32 height;
	struct bitcoin_blkid prev_hash;
	struct filteredblock_outpoint **outpoints;
};

struct bitcoind *new_bitcoind(const tal_t *ctx,
			      struct lightningd *ld,
			      struct log *log);

void bitcoind_estimate_fees_(struct bitcoind *bitcoind,
			     size_t num_estimates,
			     void (*cb)(struct bitcoind *bitcoind,
					const u32 satoshi_per_kw[], void *),
			     void *arg);

#define bitcoind_estimate_fees(bitcoind_, num, cb, arg) \
	bitcoind_estimate_fees_((bitcoind_), (num), \
				typesafe_cb_preargs(void, void *,	\
						    (cb), (arg),	\
						    struct bitcoind *,	\
						    const u32 *),	\
				(arg))

void bitcoind_sendrawtx_(struct bitcoind *bitcoind,
			 const char *hextx,
			 void (*cb)(struct bitcoind *bitcoind,
				    bool success, const char *msg, void *),
			 void *arg);

#define bitcoind_sendrawtx(bitcoind_, hextx, cb, arg)			\
	bitcoind_sendrawtx_((bitcoind_), (hextx),			\
			    typesafe_cb_preargs(void, void *,		\
						(cb), (arg),		\
						struct bitcoind *,	\
						bool, const char *),	\
			    (arg))

/* This function **cannot** be called if `bitcoind_can_getutxobyscid`
 * returns true!
 */
void bitcoind_getfilteredblock_(struct bitcoind *bitcoind, u32 height,
				void (*cb)(struct bitcoind *bitcoind,
					   const struct filteredblock *fb,
					   void *arg),
				void *arg);
#define bitcoind_getfilteredblock(bitcoind_, height, cb, arg)		\
	bitcoind_getfilteredblock_((bitcoind_),				\
				   (height),				\
				   typesafe_cb_preargs(void, void *,	\
						       (cb), (arg),	\
						       struct bitcoind *, \
						       const struct filteredblock *), \
				   (arg))

void bitcoind_getchaininfo_(struct bitcoind *bitcoind,
			    const bool first_call,
			    void (*cb)(struct bitcoind *bitcoind,
				       const char *chain,
				       u32 headercount,
				       u32 blockcount,
				       const bool ibd,
				       const bool first_call, void *),
			    void *cb_arg);
#define bitcoind_getchaininfo(bitcoind_, first_call_, cb, arg)		   \
	bitcoind_getchaininfo_((bitcoind_), (first_call_),		   \
			      typesafe_cb_preargs(void, void *,		   \
						  (cb), (arg),		   \
						  struct bitcoind *,	   \
						  const char *, u32, u32,  \
						  const bool, const bool), \
			      (arg))

void bitcoind_getrawblockbyheight_(struct bitcoind *bitcoind,
				   u32 height,
				   void (*cb)(struct bitcoind *bitcoind,
					      struct bitcoin_blkid *blkid,
					      struct bitcoin_block *blk,
					      void *arg),
				   void *arg);
#define bitcoind_getrawblockbyheight(bitcoind_, height_, cb, arg)		\
	bitcoind_getrawblockbyheight_((bitcoind_), (height_),			\
				      typesafe_cb_preargs(void, void *,		\
							  (cb), (arg),		\
							  struct bitcoind *,	\
							  struct bitcoin_blkid *, \
							  struct bitcoin_block *),\
				      (arg))

/* This function **cannot** be called if `bitcoind_can_getutxobyscid`
 * returns true!
 */
void bitcoind_getutxout_(struct bitcoind *bitcoind,
			 const struct bitcoin_txid *txid, const u32 outnum,
			 void (*cb)(struct bitcoind *bitcoind,
				    const struct bitcoin_tx_output *txout,
				    void *arg),
			 void *arg);
#define bitcoind_getutxout(bitcoind_, txid_, vout_, cb, arg)		\
	bitcoind_getutxout_((bitcoind_), (txid_), (vout_),		\
			    typesafe_cb_preargs(void, void *,		\
					        (cb), (arg),		\
					        struct bitcoind *,	\
					        struct bitcoin_tx_output *),\
			    (arg))

void bitcoind_check_commands(struct bitcoind *bitcoind);

/** bitcoind_can_getutxobyscid
 *
 * @brief determine if we can call `bitcoind_getutxobyscid`.
 *
 * @param bitcoind - the bitcoind object to query.
 *
 * @return true if we can call `bitcoind_getutxobyscid` and
 * `bitcoind_checkspent`, but **not** `bitcoind_getfilteredblock`
 * or `bitcoind_getutxout`.
 * false if we can call `bitcoind_getfilteredblock` and
 * `bitcoind_getutxout`, but **not** `bitcoind_getutxobyscid` or
 * `bitcoind_checkspent`.
 */
bool bitcoind_can_getutxobyscid(const struct bitcoind *bitcoind);
/** bitcoind_can_checkspent
 *
 * @brief alias of bitcoind_can_getutxobyscid, for didactic
 * purposes.
 */
static inline
bool bitcoind_can_checkspent(const struct bitcoind *bitcoind)
{
	return bitcoind_can_getutxobyscid(bitcoind);
}
/** bitcoind_can_getfilteredblock
 *
 * @brief alias of !bitcoind_can_getutxobyscid, for didactic
 * purposes.
 */
static inline
bool bitcoind_can_getfilteredblock(const struct bitcoind *bitcoind)
{
	return !bitcoind_can_getutxobyscid(bitcoind);
}

/** bitcoind_getutxobyscid
 *
 * @brief query the UTXO at the given SCID.
 *
 * @desc query the amount and `scriptPubKey` of a utxo according
 * to its confirmed position in the blockchain, represented by a
 * short channel ID or SCID.
 * Call the callback with NULL if output specified by SCID is not
 * a P2WSH or Taproot output, is not a valid position on the
 * blockchain, or has been spent.
 *
 * Precondition: This function ***cannot*** be called if
 * `bitcoind_can_getutxobyscid` returns false!
 *
 * @param bitcoind - the bitcoind object to query.
 * @param scid - the position of the transaction output to find.
 * @param script - the `scriptPubKey` that we expect the output
 * to have.
 * If the output does not match the given `scriptPubKey`, this
 * will be considered a failure and the callback will be called
 * with `NULL` arguments.
 * @param cb - the callback function to call.
 * @param arg - the extra argument to pass to the callback.
 *
 * @return nothing, but the callback is called with the `struct
 * bitcoin_txid` at the position, plus the `struct
 * bitcoin_tx_output` at the position.
 * If the UTXO at the SCID is not a P2WSH or Taproot output, or
 * there is no transaction or output at the position indicated by
 * the SCID, or the output has been spent, then both arguments
 * to the callback are `NULL`.
 * The callback is called within a database transaction.
 */
void bitcoind_getutxobyscid_(struct bitcoind *bitcoind,
			     struct short_channel_id scid,
			     u8 *script TAKES,
			     void (*cb)(struct bitcoind *,
					const struct bitcoin_txid *,
					const struct bitcoin_tx_output *,
					void *),
			     void *arg);
#define bitcoind_getutxobyscid(bitcoind, scid, script, cb, arg)		\
	bitcoind_getutxobyscid_((bitcoind), (scid), (script),		\
				typesafe_cb_preargs(void, void *,	\
						    (cb), (arg),	\
						    struct bitcoind *,	\
						    const struct bitcoin_txid *, \
						    const struct bitcoin_tx_output *), \
				(arg))

/** bitcoind_checkspent
 *
 * @brief check if the given utxos have been spent.
 *
 * @desc check if the given UTXOs have been spent, and return an array
 * of UTXOs now known to be spent.
 * The returned array will have `struct utxo` objects whose `status` is
 * set to `output_state_spent`.
 * If the spending transaction is confirmed, it will set `spendheight` to
 * non-NULL and provide the depth of the spending transaction.
 *
 * Precondition: This function ***cannot*** be called if
 * `bitcoind_can_getutxobyscid` (or its alias `bitcoind_can_checkspent`)
 * returns false!
 * 
 * @param bitcoind - the bitcoind object to query.
 * @param utxos - an array of pointers to `struct utxo` objects.
 * This function will steal responsibility of this array.
 * It assumes that the pointed-to objects are `tal`-allocated from the
 * array itself.
 * This function only really checks `txid`, `outnum`, `blockheight` (which
 * can be `NULL`), and `status` fields, and will pass-through UTXOs with
 * `status` of `spent`.
 * @param cb - the callback function to call.
 * @param arg - the extra argument to pass to the callback.
 *
 * @return nothing, but the callback is called with the same input
 * `utxos` array, with the individual pointed-to objects updated to
 * state `output_state_spent` if spent.
 */
void bitcoind_checkspent_(struct bitcoind *bitcoind,
			  struct utxo **utxos,
			  void (*cb)(struct bitcoind *bitcoind,
				     struct utxo **utxos,
				     void *arg),
			  void *arg);
#define bitcoind_checkspent(bitcoind, utxos, cb, arg)			\
	bitcoind_checkspent_((bitcoind), (utxos),			\
			     typesafe_cb_preargs(void, void *,		\
						 (cb), (arg),		\
						 struct bitcoind *,	\
						 struct utxo **),	\
			     (arg))

/** bitcoind_gettxesbyheight
 *
 * @brief get block header and ID, and matching transactions of the block
 * at the given height.
 *
 * @desc Check the block at the given height if it exists, and return
 * the block ID, and the block, but with only matching transactions
 * being returned.
 *
 * @param bitcoind - the bitcoind object to query.
 * @param height - the height at which we want to check the block.
 * @param receive_scriptpubkeys - an array of `scriptPubKey`s; if a
 * transaction creates a new output matching the `scriptPubKey` it
 * should be included in the returned block data.
 * The individual scripts should have the array itself as their `tal`
 * parent if the array is specified as take().
 * @param spend_utxos - an array of UTXOs; if a transaction takes as
 * input any of the specified UTXOs it should be included in the
 * returned block data.
 * @param cb - the callback function to call.
 * @param arg - the extra argument to pass to the callback.
 *
 * @return nothing, but the callback is called with the block ID
 * and the block that was queried if it was found.
 * If the block at that height is not yet reached, both arguments
 * are `NULL`.
 * The given `struct block` may have an incomplete set of transactions
 * (i.e. some transactions in the block may not be returned).
 * The caller should only assume that if a transaction matches either
 * the `receive_scriptpubkeys` or the `spend_utxos`, or both, it *will*
 * be included, but other transactions may or may not be included.
 */
void bitcoind_gettxesbyheight_(struct bitcoind *bitcoind,
			       u32 height,
			       u8 **receive_scriptpubkeys TAKES,
			       struct bitcoin_outpoint *spend_utxos TAKES,
			       void (*cb)(struct bitcoind *bitcoind,
					  struct bitcoin_blkid *blkid,
					  struct bitcoin_block *blk,
					  void *arg),
			       void *arg);
#define bitcoind_gettxesbyheight(bitcoind, height, receive_scriptpubkeys, spend_utxos, cb, arg) \
	bitcoind_gettxesbyheight_((bitcoind), (height),			\
				  (receive_scriptpubkeys),		\
				  (spend_utxos),			\
				  typesafe_cb_preargs(void, void *,	\
						      (cb), (arg),	\
						      struct bitcoind *,\
						      struct bitcoin_blkid *, \
						      struct bitcoin_block *), \
				  (arg))

#endif /* LIGHTNING_LIGHTNINGD_BITCOIND_H */
