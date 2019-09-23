#ifndef LIGHTNING_GOSSIPD_DHCACHE_REFRESHER
#define LIGHTNING_GOSSIPD_DHCACHE_REFRESHER
#include "config.h"
#include <ccan/tal/tal.h>
#include <ccan/time/time.h>
#include <common/amount.h>

/*~ Differential Heuristic Cache Refresher
 *
 * As described in dhcache.h, the differential
 * heuristic is a fast heuristic for use by
 * guided pathfinding algorithms like A-star
 * and greedy best first search.
 * This heuristics requires the map to be
 * preprocessed;
 * the distance of every node from a fixed
 * set of distant landmarks is measured and
 * stored at each node.
 *
 * The dhcache object is responsible for
 * managing the storage of the heuristic
 * data.
 * The dhcache_refresher object is
 * responsible for the actual preprocessing.
 */

struct chan;
struct dhcache;
struct dhcache_refresher;
struct node;
struct routing_state;
struct timers;

/** dhcache_refresher_new - construct a new
 * refresher object.
 *
 * ctx - the owner of this object.
 * rstate - the routing_state to be traversed.
 * timers - the timers object to place our timers in.
 * dhcache - the cache to refresh periodically.
 * refresh_cb - function to call when a refresh
 * has just ended
 * refresh_cb_arg - argument of refresh_cb.
 */
struct dhcache_refresher *
dhcache_refresher_new(const tal_t *ctx,
		      struct routing_state *rstate,
		      struct timers *timers,
		      struct dhcache *dhcache,
		      void (*refresh_cb)(void*),
		      void *refresh_cb_arg);

/** dhcache_refresher_get_sample_amount,
 * dhcache_refresher_set_sample_amount,
 * DHCACHE_REFRESHER_DEFAULT_SAMPLE_AMOUNT.
 *
 * Gets and sets the sample amount, in
 * millisatoshis, to use when measuring the
 * distance of a node from the landmark.
 *
 * The nearer the sample amount is to the
 * actual payments made by the user, the
 * more accurate our fee estimation and
 * the nearer pathfinding will be to
 * optimum.
 * Default is 1 millibitcoin.
 *
 * @refresher - the refresher whose
 * sample amount to get/set.
 * @amount - the amount to set.
 */
/* 1 millibitcoin is the default.  */
#define DHCACHE_REFRESHER_DEFAULT_SAMPLE_AMOUNT \
	AMOUNT_MSAT( 1000 /*millisatoshi per satoshi*/ \
		   * 100 /*satoshi per microbitcoin*/ \
		   * 1000 /*microbitcoin per millibitcoin*/)
struct amount_msat
dhcache_refresher_get_sample_amount(
		const struct dhcache_refresher *refresher);
void
dhcache_refresher_set_sample_amount(
		struct dhcache_refresher *refresher,
		struct amount_msat amount);

/** dhcache_refresher_get_sample_riskfactor,
 * dhcache_refresher_set_sample_riskfactor,
 * DHCACHE_REFRESHER_DEFAULT_SAMPLE_RISKFACTOR.
 *
 * Gets and sets the sample riskfactor, in percent
 * interest per annum.
 * I have no idea how to translate this intuitively,
 * so just refer to doc/lightning-getroute.7.txt
 * for an explanation.
 *
 * The nearer this sample riskfactor to what the
 * user actually desires, the more accurate the
 * fee estimate and the nearer pathfinding will
 * be to optimum.
 * Default is 10% per annum.
 *
 * @refresher - the refresher whose
 * sample riskfactor to get/set.
 * @riskfactor - the riskfactor to set.
 */
/* 10% per annum is the default.  */
#define DHCACHE_REFRESHER_DEFAULT_SAMPLE_RISKFACTOR ((double)10.0)
double
dhcache_refresher_get_sample_riskfactor(
		const struct dhcache_refresher *refresher);
void
dhcache_refresher_set_sample_riskfactor(
		struct dhcache_refresher *refresher,
		double riskfactor);

/** dhcache_refresher_get_defer_time,
 * dhcache_refresher_set_defer_time,
 * DHCACHE_REFRESHER_DEFAULT_DEFER_TIME.
 *
 * Gets and sets the default defer time, in
 * seconds.
 *
 * Periodically, the dhcache_refresher
 * should be triggered via the
 * dhcache_refresher_deferred_trigger.
 * The actual start of a new refresh
 * cycle will be defer_time seconds
 * after the trigger function is called.
 *
 * The expectation is that the deferred
 * trigger function will be called at
 * each block.
 * Channel closures will be known
 * as soon as a block is received and
 * processed, and new channels that have
 * now been deeply confirmed will
 * also be gossiped and eventually reach
 * our node.
 * Thus, the defer time is a grace
 * period for the gossip system to
 * update the routemap.
 * Default is 10 seconds.
 *
 * @refresher - the refresher whose
 * defer time is to be get/set.
 * @time - the defer time to set.
 */
#define DHCACHE_REFRESHER_DEFAULT_DEFER_TIME \
	time_from_sec(10)
struct timerel
dhcache_refresher_get_defer_time(
		const struct dhcache_refresher *refresher);
void
dhcache_refresher_set_defer_time(
		struct dhcache_refresher *refresher,
		struct timerel time);

/** dhcache_refresher_deferred_trigger
 *
 * Schedules a refresh in defer_time seconds
 * from now.
 *
 * If a refresh is already scheduled or
 * running, this call does nothing.
 *
 * @refresher - the refresher which will
 * be triggered later.
 */
void dhcache_refresher_deferred_trigger(
		struct dhcache_refresher *refresher);

/** dhcache_refresher_immediate_trigger
 *
 * Immediately triggers a refresh right now.
 *
 * If a deferred refresh is already scheduled,
 * the deferred refresh is cancelled and the
 * refresh is started immediately.
 *
 * If a refresh is currently ongoing, this
 * call does nothing.
 *
 * @refresher - the refresher which will
 * be triggered now.
 */
void dhcache_refresher_immediate_trigger(
		struct dhcache_refresher *refresher);

/** struct dhcache_coster - object that measures the
 * cost of traversing a channel, using the
 * sample_amount and riskfactor that was used in the
 * most recent dhcache view.
 */
struct dhcache_coster {
	struct amount_msat sample_amount;
	double riskfactor;
};

/** dhcache_coster_init - Initialize the dhcache_coster
 * from a dhcache_refresher, taking the most recently
 * refreshed sample_amount and riskfactor.
 *
 * Precondition: The dhcache used by the dhcache_refresher
 * must return true for dhcache_available.
 *
 * @coster - the dhcache_coster to initialize.
 * @refresher - the dhcache_refresher to use.
 */
void dhcache_coster_init(struct dhcache_coster *coster,
			 const struct dhcache_refresher *refresher);

/** dhcache_coster_get - Get the cost of traversing
 * the given channel across the nodes.
 *
 * @coster - the dhcache_coster to use as reference.
 * @from - the node which will pay.
 * @channel - the channel to traverse.
 * @to - the node which will be paid.
 */
struct amount_msat dhcache_coster_get(const struct dhcache_coster *coster,
				      struct node *from,
				      struct chan *channel,
				      struct node *to);

#endif /* LIGHTNING_GOSSIPD_DHCACHE_REFRESHER */
