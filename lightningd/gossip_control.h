#ifndef LIGHTNING_LIGHTNINGD_GOSSIP_CONTROL_H
#define LIGHTNING_LIGHTNINGD_GOSSIP_CONTROL_H
#include "config.h"
#include <bitcoin/short_channel_id.h>
#include <ccan/short_types/short_types.h>
#include <stdbool.h>

struct bitcoin_outpoint;
struct lightningd;

void gossip_init(struct lightningd *ld, int connectd_fd);

void gossipd_notify_spend(struct lightningd *ld,
			  const struct short_channel_id *scid);

void gossip_notify_new_block(struct lightningd *ld, u32 blockheight);

/** gossip_gather_filters
 *
 * @brief Gather UTXOs that gossip wants to know if they get spent.
 *
 * @desc gossip has to check that published channels get closed.
 * Each channel is really just a UTXO, and the TXO getting spent is
 * the event that closes the channel.
 *
 * @param ld - the main lightningd object.
 * @param receive_scriptpubkeys - in/out; a `tal_arr` array of
 * scriptpubkeys which is extended to add more scriptpubkeys gossip
 * would be interested in; not modified by this function.
 * @param spend_utxos - in/out; a `tal_arr` array of UTXOs which is
 * extended to add UTXOs that gossip thinks are published channels.
 */
void gossip_gather_filters(struct lightningd *ld,
			   u8 ***receive_scriptpubkeys,
			   struct bitcoin_outpoint **spend_utxos);

#endif /* LIGHTNING_LIGHTNINGD_GOSSIP_CONTROL_H */
