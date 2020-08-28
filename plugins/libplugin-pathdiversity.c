#include"libplugin-pathdiversity.h"
#include<assert.h>
#include<ccan/list/list.h>
#include<ccan/tal/link/link.h>
#include<ccan/tal/str/str.h>
#include<common/json.h>
#include<common/json_helpers.h>
#include<common/json_stream.h>
#include<common/node_id.h>
#include<common/type_to_string.h>
#include<stdarg.h>

/*-----------------------------------------------------------------------------
Path Diversity Explanation
-----------------------------------------------------------------------------*/
/*~ Logically, if we have two or more sub-payments, and we send them *all* out
 * on the exact same path, we are dumb.
 *
 * If we had them all going along the same path, we might as well just have
 * started with all of them in a single sub-payment.
 * This reduces our costs in fees, *and* reduces the load on the network,
 * since every HTLC created has to allocate some satoshis to put into fees
 * to instantiate that HTLC on commitment transactions, and forwarding nodes
 * take on slightly more risk with more HTLCs than with fewer HTLCs, thus
 * will charge higher.
 *
 * Due to limits on the number of HTLCs a single channel can host anyway,
 * sending too many sub-payments along the same paths will require more
 * funds to be allocated for instantiating those HTLCs.
 * Even in a future with anchor commitments, if a channel is dropped
 * onchain, those HTLCs have to be paid for, thus forwarding node
 * operators *will* limit the number of HTLCs per channel, still.
 *
 * Thus, if we are going to go split payments, we should try to ensure that
 * each sub-payment has a different path from other sub-payments.
 * This is called path diversity.
 *
 * We can implement path diversity by creating a *tree* of possible routes.
 *
 * Suppose the shortest route to the destination is the path A->B->C->D.
 * We use this as the root of a tree.
 *
 * Each child of a tree-node (a path) is the shortest path we find when we
 * exclude *one* of the hop channels in the parent.
 * For example, for the path A->B->C->D, the first child will be the
 * shortest path where A->B is excluded (if one exists), while the second
 * child will be the shortest path where B->C is excluded, and the third
 * child will be the shortest path when C->D is excluded.
 *
 * Further children of the path where A->B is excluded will also add that
 * channel to those it will exclude.
 *
 * For example, consider the following network:
 *
 *     A ---- B ---- C ----- D
 *       \     \   /         |
 *        \     \ /          |
 *         E --- F - G - H - I
 *
 * Then the tree would look a little like this:
 *
 *                             A->B->C->D
 *                            /     \  \ ...other children...
 *                           /       \
 *                  (ban AB)/         \ (ban BC)
 *                    A->E->F->C->D    A->B->F->C->D
 *                     /    /      \        .... other children ...
 *        (ban AB AE) /    /(AB EF) \(AB FC)
 *              <dead>  <dead>      A->E->F->G->H->I->D
 *                                   /|\
 *                                ...etc...
 *
 * And so on.
 *
 * Now, we want to traverse this tree node-by-node, parent first,
 * in a breadth-first manner.
 *
 * Why preemptively ban channels?
 * Because the point is path *diversity*.
 * Yes, maybe the channel we are banning for some tree-nodes is perfectly
 * fine, but the point is that other payments running in parallel with
 * this one might overload that channel, especially if we start splitting
 * too often, leading to even more splitting.
 *
 * Why breadth-first?
 * Because as we go down the tree, more channels become banned, which
 * makes it more likely that the found path is longer than nearer the
 * root.
 * When path lengths become longer, they become less reliable (more
 * nodes likely to fail) and more expensive.
 * So we should try paths nearer to the top of the tree first.
 *
 * So, how do we implement breadth-first search?
 * We use a queue to represent tree-nodes we have generated, but which
 * we have not processed to generate their own children.
 *
 * Each "child" tree-node then contains a (shared, ref-counting) pointer
 * to its parent.
 *
 * For example, we start with an empty queue.
 * We then perform a `getroute` and receive the shortest path A->B->C->D.
 * Before emitting that route, we push the objects AB, BC, and CD to
 * the queue.
 *
 * On the next iteration when we need to find a new route, we pop off the
 * AB object.
 * We then ban the AB path and `getroute` again.
 * If it succeeds and we receive the shortest path A->E->F->C->D, then
 * we push onto the queue an AE object that points to the AB object as a
 * parent, then EF, FC, CD objects (all pointing to the AB object as parent).
 *
 * A few iterations later, when we get the AE object, we find it points to
 * a parent AB object AE->AB, meaning we should ban both AE and AB paths.
 * `getroute` then fails, so we drop the AE object entirely.
 * We try with the next object, EF, which points again to the AB object,
 * and `getroute` with AB and EF channels disabled.
 * Again, `getroute` fails, so we drop the EF object, pop off the next
 * object FC, which again points to AB as the parent, and `getroute` with
 * AB and FC channels disabled.
 * This time it succeeds, with the route A->E->F->G->H->I->D, and we add
 * the needed objects to the queue, each with parents pointing to the FC
 * object.
 *
 * Improvement
 * -----------
 *
 * Rather than a dumb queue, we should use a priority queue, with the
 * fee and cltv delay of the path being used to evaluate queue entries
 * (lower is higher priority).
 *
 * Such an algorithm would be roughly equivalent to a Dijkstra algorithm,
 * except running over entire paths rather than individual proposal hops.
 * ("meta-Dijkstra"? "Dijkstra layer 2"?)
 * The priority queue would be the OPEN set while the set of already-
 * generated paths would be the CLOSED set.
 *
 * This would be more involved, and would require doing future `getroute`s
 * before we even return the current one (we need to measure the cost of
 * the route in order to put it in the priority queue, which means we need
 * to *know* the route).
 * This means that we would probably need to adapt a path previously
 * generated for one payment to the amount of a new payment, with concomitant
 * problems: if the path was made for a smaller payment then it might contain
 * channels that are too small for the newer, larger payment.
 *
 * Hopefully this first cut at path diversity would work well enough to be
 * useful.
 */

/*-----------------------------------------------------------------------------
Basic Data Structures
-----------------------------------------------------------------------------*/

/** tal_relink
 *
 * @brief move ownership of a link from one owner to another.
 *
 * @param newctx - the new owner of the link.
 * @param oldctx - the current owner of the link.
 * @param obj - the object whose link is being transferred.
 *
 * @return the same object.
 */
static inline tal_t *
tal_relink(const tal_t *newctx, const tal_t *oldctx, tal_t *obj)
{
	/* FIXME: This is more efficiently implemented in
	 * ccan/tal/link/link.c, by reparenting the current
	 * link to the new context.
	 */
	/* Hand-over-hand: create the new link before destroying
	 * the existing one.  */
	tal_t *tmp = tal_link(newctx, obj);
	tal_delink(oldctx, obj);
	return tmp;
}

/** struct pathdiversity_edge
 *
 * @brief an edge in the above described tree.
 *
 * @desc represents which pair of nodes should have their
 * channels banned.
 *
 * If you paid attention to the example tree in the above
 * discussion, you would have noticed that we annotated
 * `(ban AB AE)` etc. on the *edges* of the shown tree.
 * Thus, this represents such an edge on the tree.
 */
struct pathdiversity_edge {
	struct node_id source;
	struct node_id destination;
	struct pathdiversity_edge *parent;

	/* An entry in the pathdiversity_queue.  */
	struct list_node qlist;
};
static struct pathdiversity_edge *
new_pathdiversity_edge(const tal_t *ctx,
		       const struct node_id *source,
		       const struct node_id *destination,
		       struct pathdiversity_edge *parent)
{
	struct pathdiversity_edge *e;

	e = tal_linkable(tal(NULL, struct pathdiversity_edge));
	tal_link(ctx, e);
	e->source = *source;
	e->destination = *destination;
	if (parent)
		e->parent = tal_link(e, parent);
	else
		e->parent = NULL;
	list_node_init(&e->qlist);
	return e;
}

/** struct pathdiversity_queue
 *
 * @brief a dumb queue of `struct pathdiversity_edge`s.
 */
struct pathdiversity_queue {
	struct list_head list;

	/* The actual owner of the links to the edges
	 * in the above list.
	 * This simplifies the `clear` method.  */
	char *owner;
};
static struct pathdiversity_queue *
new_pathdiversity_queue(const tal_t *ctx)
{
	struct pathdiversity_queue *q;

	q = tal(ctx, struct pathdiversity_queue);
	list_head_init(&q->list);
	q->owner = tal(q, char);
	return q;
}
static void
pathdiversity_queue_push(struct pathdiversity_queue *q,
			 const tal_t *oldctx,
			 struct pathdiversity_edge *e)
{
	/* Transfer to the queue.  */
	tal_relink(q->owner, oldctx, e);
	list_add_tail(&q->list, &e->qlist);
}
static struct pathdiversity_edge *
pathdiversity_queue_pop(const tal_t *newctx,
			struct pathdiversity_queue *q)
{
	struct pathdiversity_edge *e;
	e = list_pop(&q->list, struct pathdiversity_edge, qlist);
	/* Transfer to the caller-specified owner.  */
	if (e)
		tal_relink(newctx, q->owner, e);
	return e;
}
static void
pathdiversity_queue_clear(struct pathdiversity_queue *q)
{
	/* Free all links to stored edges.  */
	tal_free(q->owner);
	q->owner = tal(q, char);
	/* Reset the list.  */
	list_head_init(&q->list);
}

/** struct pathdiversity_routecache
 *
 * @brief a lookup table for previously-generated routes.
 */
struct pathdiversity_routecache {
	/* Array of arrays of node-ids in a route.  */
	struct node_id **routes;
};
static struct pathdiversity_routecache *
new_pathdiversity_routecache(const tal_t *ctx)
{
	struct pathdiversity_routecache *rc;
	rc = tal(ctx, struct pathdiversity_routecache);
	rc->routes = tal_arr(rc, struct node_id *, 0);
	return rc;
}
/* True if not found and inserted, false if already existing.  */
static bool
pathdiversity_routecache_lookup_or_insert(struct pathdiversity_routecache *rc,
					  const struct route_hop *route)
{
	for (size_t i = 0; i < tal_count(rc->routes); ++i) {
		struct node_id *scan;
		bool found = true;

		/* Scan in reverse order.
		 * The logic is that since we append new routes to the
		 * end of this array, and we tend to start with shorter
		 * routes and go on longer ones, later routes are more
		 * likely to match recently-added ones.
		 * In particular, no generated route will ever match
		 * the route at the root of the path-diversity tree.
		 */
		scan = rc->routes[tal_count(rc->routes) - 1 - i];

		/* Not even same size?  Skip.  */
		if (tal_count(scan) != tal_count(route))
			continue;

		for (size_t j = 0; j < tal_count(scan); ++j)
			if (!node_id_eq(&scan[j], &route[j].nodeid)) {
				found = false;
				break;
			}
		if (found)
			return false;
	}

	/* Not found, so add to end.  */
	struct node_id *to_insert = tal_arr(rc->routes, struct node_id,
					    tal_count(route));
	for (size_t i = 0; i < tal_count(route); ++i)
		to_insert[i] = route[i].nodeid;
	tal_arr_expand(&rc->routes, to_insert);

	return true;
}
static void
pathdiversity_routecache_clear(struct pathdiversity_routecache *rc)
{
	tal_free(rc->routes);
	rc->routes = tal_arr(rc, struct node_id *, 0);
}

/*-----------------------------------------------------------------------------
Excluded Channels Discovery
-----------------------------------------------------------------------------*/
/*~ We could have very easily just used `struct short_channel_id` to track
excluded channels in our `struct pathdiversity_edge`.

However, excluding one channel does not exclude any *other* channels that the
two endpoints of that channel have.

Now, from the bolt spec:

*/
/* BOLT #4:
 *
 * A node MAY forward an HTLC along an outgoing channel other than the one
 * specified by `short_channel_id`, so long as the receiver has the same node
 * public key intended by `short_channel_id`.
 */
/*~

This means that every `short_channel_id` is really an identifier for *every*
channel between two nodes.

Unfortunately our `getroute` only bans individual channels, even if the
BOLT #4 spec presumes that forwarding nodes will be smart enough to try other
channels with the same peer.

Thus, for true path diversity, we should ban *every* channel between two
nodes, not just the specific channel that our `getroute` function returned,
because it would not be truly diverse if we banned just one channel between
two forwarding nodes with multiple channels between them.

Because that information is over there in `lightningd` and not out here in
plugin-land, we need to write this in continuation-passing style (aka via
callbacks).
*/

struct pathdiversity_excluder {
	/* The paayment.  */
	struct payment *p;
	/* The caller for this get-excluded-channels request.  */
	void (*cb)(const struct short_channel_id *excludes,
		   void *cbarg);
	void *cbarg;

	/* The set of excluded channels already discovered.  */
	struct short_channel_id *excludes;
	/* The current edge being processed.  */
	const struct pathdiversity_edge *e;
};

static void pathdiversity_exclusion_step(struct pathdiversity_excluder *exc);

/** pathdiversity_get_exclusions
 *
 * @brief get all excluded short-channel-id-dirs of the given edge.
 *
 * @desc get all excluded short-channel-id-dirs of the given edge, and every
 * parent edge of that edge.
 * Call the callback with the array of short-channel-id-dirs.
 * The callback will be called with a set of short-channel-id-dirs that
 * will be freed on return of the callback, so if the callback needs to
 * retain that it should copy it.
 *
 * We will keep the edge alive by adding our own link to it.
 *
 * @param p - the payment we are doing this for.
 * @param e - the edge to scan.
 * @param cb - the callback to call.
 * @param cbarg - the extra argument for the callback.
 */
#define pathdiversity_get_exclusions(p, e, cb, cbarg) \
	pathdiversity_get_exclusions_(p, e, \
				      typesafe_cb_preargs(void, void *, \
							  (cb), (cbarg), \
							  const struct short_channel_id *), \
				      (cbarg))
static void
pathdiversity_get_exclusions_(struct payment *p,
			      struct pathdiversity_edge *e,
			      void (*cb)(const struct short_channel_id *,
					 void *),
			      void *cbarg)
{
	struct pathdiversity_excluder *exc;
	exc = tal(NULL, struct pathdiversity_excluder);
	exc->p = p;
	exc->cb = cb;
	exc->cbarg = cbarg;
	exc->excludes = tal_arr(exc, struct short_channel_id, 0);
	if (e)
		exc->e = tal_link(exc, e);
	else
		exc->e = NULL;

	return pathdiversity_exclusion_step(exc);
}

static struct command_result *
pathdiversity_exclusion_after_listchannels(struct command *cmd,
					   const char *buf,
					   const jsmntok_t *result,
					   struct pathdiversity_excluder *exc);

static void pathdiversity_exclusion_step(struct pathdiversity_excluder *exc)
{
	struct out_req *req;

	if (!exc->e) {
		/* Nothing more to do!  */
		tal_steal(tmpctx, exc);
		return exc->cb(exc->excludes, exc->cbarg);
	}

	req = jsonrpc_request_start(exc->p->plugin, NULL, "listchannels",
				    &pathdiversity_exclusion_after_listchannels,
				    &pathdiversity_exclusion_after_listchannels,
				    exc);
	json_add_node_id(req->js, "source", &exc->e->source);
	send_outreq(exc->p->plugin, req);
}

static struct command_result *
pathdiversity_exclusion_after_listchannels(struct command *cmd,
					   const char *buf,
					   const jsmntok_t *result,
					   struct pathdiversity_excluder *exc)
{
	const jsmntok_t *channelstok;
	size_t i;
	const jsmntok_t *chan;

	assert(exc);
	assert(exc->e);

	channelstok = json_get_member(buf, result, "channels");
	if (!channelstok || channelstok->type != JSMN_ARRAY)
		paymod_err(exc->p,
			   "Unexpected result from 'listchannels': %.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));

	json_for_each_arr (i, chan, channelstok) {
		const jsmntok_t *destinationtok;
		const jsmntok_t *scidtok;
		struct node_id destination;
		struct short_channel_id scid;

		destinationtok = json_get_member(buf, chan, "destination");
		if (!destinationtok ||
		    !json_to_node_id(buf, destinationtok, &destination))
			paymod_err(exc->p,
				   "Unexpected channel from 'listchannels': "
				   "%.*s",
				   json_tok_full_len(chan),
				   json_tok_full(buf, chan));

		/* If not a channel to destination, skip.  */
		if (!node_id_eq(&destination, &exc->e->destination))
			continue;

		scidtok = json_get_member(buf, chan, "short_channel_id");
		if (!scidtok ||
		    !json_to_short_channel_id(buf, scidtok, &scid))
			paymod_err(exc->p,
				   "Unexpected channel from 'listchannels': "
				   "%.*s",
				   json_tok_full_len(chan),
				   json_tok_full(buf, chan));
		tal_arr_expand(&exc->excludes, scid);
	}

	/* Advance.  */
	exc->e = exc->e->parent;
	pathdiversity_exclusion_step(exc);

	return command_still_pending(cmd);
}

/*-----------------------------------------------------------------------------
`getroute` replacement
-----------------------------------------------------------------------------*/
/*~ This is the core of the pathdiversity modifier.

This replaces the plain `getroute` attempt of the plain paymod system with
the path-diversity tree traversal.
*/

/*~ The `getroute` replacement should use these functions instead of
payment_continue and payment_fail.
These handle the payment queueing before actually continuing or failing
the payment.
*/
static void pathdiversity_continue(struct payment *p);
static void pathdiversity_fail(struct payment *p, const char *fmt, ...);

struct pathdiversity_getroute_attempt {
	struct payment *p;
	struct pathdiversity_queue *q;
	struct pathdiversity_routecache *rc;
	struct pathdiversity_edge *e;
};

static void
pathdiversity_getroute_step(struct pathdiversity_getroute_attempt *gr);

/** pathdiversity_getroute
 *
 * @brief gets a route for a given payment, using the
 * given queue to generate the path-diversity tree.
 *
 * @param p - the payment to get a route for.
 * @param q - the queue we should use to generate the
 * path-diversity tree.
 * @param rc - the routecache we should use to look up
 * and store routes.
 *
 * @desc Generates a route for a given payment, adding
 * exclusions to avoid having too many similarities to
 * other, parallel payments.
 */
static void pathdiversity_getroute(struct payment *p,
				   struct pathdiversity_queue *q,
				   struct pathdiversity_routecache *rc)
{
	struct pathdiversity_getroute_attempt *gr;

	gr = tal(p, struct pathdiversity_getroute_attempt);
	gr->p = p;
	gr->q = q;
	gr->rc = rc;
	gr->e = NULL;

	pathdiversity_getroute_step(gr);
}

static void
pathdiversity_getroute_got_exclusions(const struct short_channel_id *excs,
				      struct pathdiversity_getroute_attempt *gr);

static void
pathdiversity_getroute_step(struct pathdiversity_getroute_attempt *gr)
{
	/* Clean up.  */
	if (gr->e)
		tal_delink(gr, gr->e);

	gr->e = pathdiversity_queue_pop(gr, gr->q);
	/* If queue is empty, we are (re)starting at the path-diversity tree
	 * root node, so clear the routecache since we are going to
	 * re-generate all the routes again.  */
	if (!gr->e)
		pathdiversity_routecache_clear(gr->rc);

	pathdiversity_get_exclusions(gr->p, gr->e,
				     &pathdiversity_getroute_got_exclusions,
				     gr);
}

static struct command_result *
pathdiversity_getroute_ok(struct command *cmd,
			  const char *buf,
			  const jsmntok_t *result,
			  struct pathdiversity_getroute_attempt *gr);
static struct command_result *
pathdiversity_getroute_fail(struct command *cmd,
			    const char *buf,
			    const jsmntok_t *error,
			    struct pathdiversity_getroute_attempt *gr);

static void
pathdiversity_getroute_got_exclusions(const struct short_channel_id *excs,
				      struct pathdiversity_getroute_attempt *gr)
{
	struct out_req *req;
	struct payment *p = gr->p;
	char *added = tal_strdup(tmpctx, "");

	req = jsonrpc_request_start(p->plugin, NULL, "getroute",
				    &pathdiversity_getroute_ok,
				    &pathdiversity_getroute_fail,
				    gr);
	json_add_node_id(req->js, "id", p->getroute->destination);
	json_add_amount_msat_only(req->js, "msatoshi", p->getroute->amount);
	json_add_num(req->js, "cltv", p->getroute->cltv);
	json_add_num(req->js, "maxhops", p->getroute->max_hops);
	json_add_member(req->js, "riskfactor", false, "%lf",
			p->getroute->riskfactorppm / 1000000.0);

	json_array_start(req->js, "exclude");
	payment_getroute_splice_excludes(p, req->js);
	/* Add our own excludes.  */
	for (size_t i = 0; i < tal_count(excs); ++i) {
		const char *scid;
		scid = type_to_string(tmpctx, struct short_channel_id,
				      &excs[i]);
		json_add_string(req->js, NULL, tal_fmt(tmpctx, "%s/0", scid));
		json_add_string(req->js, NULL, tal_fmt(tmpctx, "%s/1", scid));
		if (i == 0)
			tal_append_fmt(&added, "%s", scid);
		else
			tal_append_fmt(&added, ", %s", scid);
	}
	json_array_end(req->js);

	if (tal_count(excs) != 0)
		paymod_log(p, LOG_DBG,
			   "Path-diversity getroute with "
			   "additional excludes: %s",
			   added);

	send_outreq(p->plugin, req);
}

static struct command_result *
pathdiversity_getroute_fail(struct command *cmd,
			    const char *buf,
			    const jsmntok_t *error,
			    struct pathdiversity_getroute_attempt *gr)
{
	const jsmntok_t *codetok, *msgtok;

	codetok = json_get_member(buf, error, "code");
	msgtok = json_get_member(buf, error, "message");

	/* If we did this without any additional exclusions, then
	 * there is no path at all with the payment-specific exclusions.
	 * Fail it directly, imitating what the default paymod flow would
	 * do.
	 */
	if (!gr->e) {
		tal_steal(tmpctx, gr);
		pathdiversity_fail(gr->p,
				   "Error computing a route to %s: "
				   "%.*s (%.*s)",
				   type_to_string(tmpctx, struct node_id,
						  gr->p->getroute->destination),
				   json_tok_full_len(msgtok),
				   json_tok_full(buf, msgtok),
				   json_tok_full_len(codetok),
				   json_tok_full(buf, codetok));
		return command_still_pending(cmd);
	}

	paymod_log(gr->p, LOG_DBG,
		   "Error computing a route to %s with extra exclusions: "
		   "%.*s (%.*s)",
		    type_to_string(tmpctx, struct node_id,
				   gr->p->getroute->destination),
		    json_tok_full_len(msgtok),
		    json_tok_full(buf, msgtok),
		    json_tok_full_len(codetok),
		    json_tok_full(buf, codetok));

	/* Try next one.  */
	pathdiversity_getroute_step(gr);

	return command_still_pending(cmd);
}

/* Determine if the given route is within the constraints.  */
enum pathdiversity_constraint_violation {
	PATHDIVERSITY_OK,
	PATHDIVERSITY_OUT_OF_FEES,
	PATHDIVERSITY_OUT_OF_TIME
};
static enum pathdiversity_constraint_violation
pathdiversity_check_constraints(struct payment *p,
				const struct route_hop *route,
				struct amount_msat *fee)
{
	if (!amount_msat_sub(fee, route[0].amount, p->amount))
		paymod_err(p,
			   "gossipd returned a route with negative fee: "
			   "sending %s to deliver %s",
			   type_to_string(tmpctx, struct amount_msat,
					  &route[0].amount),
			   type_to_string(tmpctx, struct amount_msat,
					  &p->amount));

	if (amount_msat_greater(*fee, p->constraints.fee_budget))
		return PATHDIVERSITY_OUT_OF_FEES;
	else if (route[0].delay > p->constraints.cltv_budget)
		return PATHDIVERSITY_OUT_OF_TIME;
	else
		return PATHDIVERSITY_OK;
}

static struct command_result *
pathdiversity_getroute_ok(struct command *cmd,
			  const char *buf,
			  const jsmntok_t *result,
			  struct pathdiversity_getroute_attempt *gr)
{
	struct payment *p = gr->p;
	const jsmntok_t *routetok;
	struct route_hop *route;
	struct amount_msat fee;
	struct pathdiversity_edge *e;

	routetok = json_get_member(buf, result, "route");
	if (!routetok ||
	    !(route = json_to_route(tmpctx, buf, routetok)))
		paymod_err(p,
			   "Error parsing result from getroute: %.*s",
			   json_tok_full_len(result),
			   json_tok_full(buf, result));

	/* Is the route already in the cache?  */
	if (!pathdiversity_routecache_lookup_or_insert(gr->rc, route)) {
		/* Try next one.  */
		pathdiversity_getroute_step(gr);
		return command_still_pending(cmd);
	}

	/* Add route hops to the queue.  */
	e = new_pathdiversity_edge(tmpctx,
				   p->local_id, &route[0].nodeid,
				   gr->e);
	pathdiversity_queue_push(gr->q, tmpctx, e);
	for (size_t i = 1; i < tal_count(route); ++i) {
		e = new_pathdiversity_edge(tmpctx,
					   &route[i - 1].nodeid,
					   &route[i].nodeid,
					   gr->e);
		pathdiversity_queue_push(gr->q, tmpctx, e);
	}

	/* Check constraints.  */
	enum pathdiversity_constraint_violation res;
	res = pathdiversity_check_constraints(p, route, &fee);

	if (res != PATHDIVERSITY_OK) {
		if (!gr->e) {
			/* This was the shortest path!  Fail it.  */
			tal_steal(tmpctx, gr);
			p->route = NULL;
			if (res == PATHDIVERSITY_OUT_OF_FEES) {
				payment_exclude_most_expensive(p, route);
				pathdiversity_fail(p,
						   "Fee exceeds our fee "
						   "budget: %s > %s, "
						   "discarding route",
						   type_to_string(
							tmpctx,
							struct amount_msat,
							&fee),
						   type_to_string(
							tmpctx,
							struct amount_msat,
							&p->constraints.fee_budget));
			} else {
				payment_exclude_longest_delay(p, route);
				pathdiversity_fail(p,
						   "CLTV delay exceeds our "
						   "CLTV budget: %d > %d",
						   route[0].delay,
						   p->constraints.cltv_budget);
			}
		} else {
			/* This particular node of the path-diversity tree
			 * has resulted in a route that exceeds the budget!
			 * Traversing further will tend to result in routes
			 * with similar, or longer, length/cost, so it would
			 * be better at this point to go back to the tree
			 * root.
			 * We start traversing at the tree root when the
			 * traversal queue is empty, so clear it and retry.
			 * If the result afterwards still exceeds the budget,
			 * we enter into the other branch of this `if`.
			 */
			pathdiversity_queue_clear(gr->q);
			pathdiversity_getroute_step(gr);
		}
		return command_still_pending(cmd);
	}

	/* Success!  We can now free our object and continue processing.  */
	tal_steal(tmpctx, gr);
	p->route = tal_steal(p, route);
	payment_set_step(p, PAYMENT_STEP_GOT_ROUTE);
	pathdiversity_continue(p);

	return command_still_pending(cmd);
}

/*-----------------------------------------------------------------------------
Path Diversity Payment Queues
-----------------------------------------------------------------------------*/
/*~ We need to synchronize path diversity attempts since we want ongoing
`getroute` attempts to finish before starting a new one for the current
payment.
This is because each `getroute` seeds the subsequent attempts into the
`struct pathdiversity_queue`, so we should defer subsequent payments
until after the current payment has gotten its route and actually put
objects into the `struct pathdiversity_queue`.

For example, consider if we enter this paymod on two sub-payments.
The first sub-payment enters this paymod, and since it came first, it starts
at the root of the path-diversity tree (equivalently, it will see the
path-diversity traversal queue as empty, meaning no additional excludes,
meaning it gets the "true shortest path"), and then it enters `getroute`.
Since `getroute` runs over there in `gossipd`-land, this plugin keeps on
keeping on, and the second sub-payment will also enter this paymod, see the
path-diversity traversal queue as empty, meaning it *also* gets the root
of the path-diversity tree and *also* gets the same path as the first,
utterly failing our desire to have *different* paths.

Thus, payments are also queued for going through the above `getroute`
replacement.
This ensures that, while the first sub-payment is over in `gossipd` running
`getroute`, subsequent sub-payments are blocked waiting until it completes
and has loaded the queue correctly.

Note that `gossipd` is single-threaded anyway and parallel `getroute`
requests are no faster than serial requests, there will be a synchronization
between `lightningd` and `gossipd` to ensure this serialization, so we might
as well do the serializing here, where we can take advantage of previous
attempts to inform subsequent attempts.

In addition, we have separate queues (and therefore different path-diversity
trees) for each destination.
Different sub-payments of the same overall payment might have different
destinations if they go to different routehints, for example.
*/

/* Per-payment data structure.  */
struct pathdiversity_data {
	/* The common pathdiversity data structures.  */
	struct pathdiversity_common *common;
	/* The payment this is for.  */
	struct payment *p;
	/* Which destination we are going to.  */
	struct pathdiversity_destination *destination;
	/* The list of payments we are currently in.  */
	struct list_node list;
	/* Whether we should start.  */
	bool should_start;
};

struct pathdiversity_common {
	/* The individual destinations.
	 * Contains `struct pathdiversity_destination` objects.  */
	struct list_head destinations;
	/* The list of payments that are still to be distributed.
	 * Actually contains `struct pathdiversity_data`.
	 */
	struct list_head payments;
};

struct pathdiversity_destination {
	/* The actual destination node this queue is for.  */
	struct node_id node;
	/* The queue of tree edges.  */
	struct pathdiversity_queue *q;
	/* The routes already generated.  */
	struct pathdiversity_routecache *rc;
	/* The list of payments that are going to this destination.
	 * Actually contains `struct pathdiversity_data`.
	 */
	struct list_head payments;
	/* The list of destinations in `struct pathdiversity_common`.  */
	struct list_node list;
};

REGISTER_PAYMENT_MODIFIER_HEADER(pathdiversity, struct pathdiversity_data);

static struct pathdiversity_data *
pathdiversity_data_init(struct payment *p)
{
	struct pathdiversity_common *common;
	struct pathdiversity_data *d;

	if (p->parent)
		common = payment_mod_pathdiversity_get_data(p->parent)->common;
	else {
		common = tal(p, struct pathdiversity_common);
		list_head_init(&common->destinations);
		list_head_init(&common->payments);
	}

	d = tal(p, struct pathdiversity_data);
	d->common = common;
	d->p = p;
	d->destination = NULL;
	list_add_tail(&common->payments, &d->list);
	d->should_start = false;

	return d;
}

static void pathdiversity_start_payment_of(struct pathdiversity_data *d);

static void pathdiversity_step_cb(struct pathdiversity_data *d,
				  struct payment *p)
{
	struct pathdiversity_common *common = d->common;

	/* We only operate at the end of the initialized step.  */
	if (p->step != PAYMENT_STEP_INITIALIZED) {
		/* If the payment enters into any step other than
		 * INITIALIZED while it is in a payment list, we
		 * should remove it from the whatever payment list it
		 * is in.
		 *
		 * Otherwise, if a payment transitions from INITIALIZED
		 * to any other state, its `should_start` flag will not
		 * be set, and it would keep blocking subsequent payments.
		 */
		if (d->list.next != &d->list) {
			list_del_init(&d->list);
			payment_continue(p);
		} else
			return payment_continue(p);
	} else
		/* *This* particular payment should start.
		 * However, we should wait for earlier-created payments to
		 * begin, on the assumption that some other paymods are smart
		 * and will first construct payments that should go to shorter
		 * and cheaper paths.  */
		d->should_start = true;

	/* Now check for payments in the payments list that are ready for
	 * processing through this system.  */
	while ((d = list_top(&common->payments,
			     struct pathdiversity_data, list)) &&
	       d->should_start) {
		list_del_init(&d->list);
		pathdiversity_start_payment_of(d);
	}
}

static void pathdiversity_start_payment_of(struct pathdiversity_data *d)
{
	struct pathdiversity_common *common = d->common;
	struct node_id *target_node;
	struct pathdiversity_destination *dest, *scan;

	/* Find the correct destination.  */
	target_node = d->p->getroute->destination;
	dest = NULL;
	list_for_each (&common->destinations, scan, list)
		if (node_id_eq(target_node, &scan->node)) {
			/* Found.  */
			dest = scan;
			break;
		}

	/* If no destination object yet, construct one.  */
	if (!dest) {
		dest = tal(common, struct pathdiversity_destination);
		dest->node = *target_node;
		dest->q = new_pathdiversity_queue(dest);
		dest->rc = new_pathdiversity_routecache(dest);
		list_head_init(&dest->payments);
		list_add_tail(&common->destinations, &dest->list);
	}

	/* Add to the destination payments list.  */
	d->destination = dest;
	list_add_tail(&dest->payments, &d->list);

	/* If this newly-enqueued item is the first in the destination
	 * payments queue (meaning, the list was empty before we added
	 * this payment, meaning no ongoing getroute), we should initiate
	 * doing getroute.  */
	if (list_top(&dest->payments, struct pathdiversity_data, list) == d)
		pathdiversity_getroute(d->p, dest->q, dest->rc);
}

/* Used by pathdiversity_continue and pathdiversity_fail, to indicate that
 * we are done processing this payment.  */
static void pathdiversity_processing_done(struct pathdiversity_data *d)
{
	struct pathdiversity_destination *dest;

	dest = d->destination;

	/* This should be at the front of the payments list for this
	 * destination.  */
	assert(list_top(&dest->payments,
			struct pathdiversity_data, list) == d);

	list_del_init(&d->list);

	/* Are there more to process for this destination?  */
	if (!list_empty(&dest->payments)) {
		d = list_top(&dest->payments,
			     struct pathdiversity_data, list);
		pathdiversity_getroute(d->p, dest->q, dest->rc);
	}
}

static void pathdiversity_continue(struct payment *p)
{
	pathdiversity_processing_done(payment_mod_pathdiversity_get_data(p));

	payment_continue(p);
}

static void pathdiversity_fail(struct payment *p, const char *fmt, ...)
{
	va_list ap;
	char *txt;

	pathdiversity_processing_done(payment_mod_pathdiversity_get_data(p));

	va_start(ap, fmt);
	txt = tal_vfmt(tmpctx, fmt, ap);
	va_end(ap);

	payment_fail(p, "%s", txt);
}

/* Finally, the actual payment modifier.  */
REGISTER_PAYMENT_MODIFIER(pathdiversity, struct pathdiversity_data *,
			  &pathdiversity_data_init,
			  &pathdiversity_step_cb);
