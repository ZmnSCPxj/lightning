#include "dhcache_refresher.h"
#include <common/status.h>
#include <common/timeout.h>
#include <gossipd/dhcache.h>
#include <gossipd/priority_queue.h>
#include <gossipd/routing.h>
#include <math.h>
#include <stdlib.h>

struct refresh_process;
enum refresh_process_step_result;

/* 365.25 * 24 * 60 / 10 */
#define BLOCKS_PER_YEAR 52596

struct refresh_process {
	/* From constructor of dhcache_refresher.  */
	struct routing_state *rstate;
	struct dhcache *dhcache;

	/* Writer into the dhcache.  */
	struct dhcache_writer writer;

	/* Priority queue containing malloc-allocated
	 * struct node *.
	 */
	struct priority_queue *queue;

	/* Coster to be used for this refresh process.
	 * Its fields will be copied directly from
	 * the dhcache_refresher settings.
	 */
	struct dhcache_coster coster;

	/* Step function, state pattern.  */
	enum refresh_process_step_result (*step)
		(struct refresh_process *process);
};
/* Possible results of a refresh_process
 * step function.
 */
enum refresh_process_step_result {
	/* Continue processing.  */
	refresh_process_step_result_continue,
	/* Refresh process failed; do not flip.
	 * Instead wait for defer_time and
	 * restart the process.
	 */
	refresh_process_step_result_failed,
	/* Refresh process completed successfully.  */
	refresh_process_step_result_completed,
};

struct dhcache_refresher {
	/* From constructor arguments.  */
	struct routing_state *rstate;
	struct timers *timers;
	struct dhcache *dhcache;
	void (*refresh_cb)(void*);
	void *refresh_cb_arg;

	/* Current settings.  */
	struct amount_msat sample_amount;
	double riskfactor;
	struct timerel defer_time;

	/* Most recent coster.
	 * This is copied from the most recently
	 * completed refresh_process.
	 */
	struct dhcache_coster coster;

	/* Currently scheduled deferred trigger.  */
	struct oneshot *deferred;
	/* Currently running refresher process.  */
	struct refresh_process *process;
	/* Currently scheduled process reawakening.  */
	struct oneshot *reawaken;
};

/* Step functions.  */
static enum refresh_process_step_result
refresh_process_step_init(struct refresh_process *process);
static enum refresh_process_step_result
refresh_process_step_loop(struct refresh_process *process);

/* Destructor for refresh_process.  */
static void destroy_refresh_process(struct refresh_process *process)
{
	/* The refresh process allocates struct node_id from
	 * malloc, clean it up here.
	 */
	priority_time *item;
	while ((item = priority_queue_get_min(process->queue)))
		free(item);
}

/** REFRESHER_WORKING_TIME - amount of time we are doing refresher
 * work.
 */
#define REFRESHER_WORKING_TIME time_from_msec(10)
/** REFRESHER_SLEEPING_TIME - amount of time we are allowing the
 * gossipd to do its other tasks even though refreshing is not
 * done yet.
 */
#define REFRESHER_SLEEPING_TIME time_from_msec(10)

/** refresher_reawaken - Called periodically while a refresh_process
 * is installed.
 */
static
void refresher_reawaken(struct dhcache_refresher *refresher)
{
	struct timemono start;

	assert(refresher->process != NULL);
	assert(refresher->reawaken != NULL);

	refresher->reawaken = tal_free(refresher->reawaken);

	start = time_mono();

	status_trace("dhcache_refresher: Refresh process awoken.");

	for (;;) {
		enum refresher_process_step_result res;
		struct timerel time_passed;

		/* Perform several steps.
		 * This is done so that we do not spam the OS
		 * for the current time too much.
		 */
		for (int i = 0; i < 16; ++i) {
			res = refresher->process->step(refresher->process);
			switch (res) {
			case refresh_process_step_result_continue:
				continue;
			case refresh_process_step_result_failed:
				/* Abort, reschedule.  */
				refresher->process = tal_free(refresher->process);
				dhcache_refresher_deferred_trigger(refresher);
				status_trace("dhcache_refresher: "
					     "Refresh process failed!");
				return;
			case refresh_process_step_result_completed:
				/* Flip dhcache.  */
				dhcache_flip(refresher->dhcache);
				/* Copy coster of the process to be used
				 * as reference for future pathfinding
				 * calls.
				 */
				refresher->coster = refresher->process->coster;
				/* Clear process.  */
				refresher->process = tal_free(refresher->process);
				/* Trigger refresh_cb.  */
				refresher->refresh_cb(refresher->refresh_cb_arg);
				status_trace("dhcache_refresher: "
					     "Refresh process completed!");
				return;
			}
		}

		/* If we have been working past the working time,
		 * go to sleep.
		 */
		time_passed = timemono_since(start);
		if (time_less(REFRESHER_WORKING_TIME, time_passed)) {
			/* Sleep and reinvoke this function.  */
			refresher->reawaken
				= new_reltimer(refresher->timers,
					       refresher,
					       REFRESHER_SLEEPING_TIME,
					       &refresher_reawaken,
					       refresher);
			status_trace("dhcache_refresher: "
				     "Refresh process sleeping.");
			return;
		}
	}
}

/** install_refresh_process - Start a new refresh_process
 * to the given refresher object.
 */
static void
install_refresh_process(struct dhcache_refresher *refresher)
{
	struct refresh_process *process;

	assert(refresher->process == NULL);
	assert(refresher->reawaken == NULL);

	process = tal(refresher, struct refresh_process);

	/* Install destructor.  */
	tal_add_destructor(process, &destroy_refresh_process);
	/* Copy constructor arguments.  */
	process->rstate = refresher->rstate;
	process->dhcache = refresher->dhcache;
	/* Initiate writer.  */
	dhcache_writer_init(&process->writer, process->dhcache);
	/* Create empty priority queue.  */
	process->queue = priority_queue_new(process);
	/* Copy settings of coster from most recent settings.  */
	process->coster.sample_amount = refresher->sample_amount;
	process->coster.riskfactor = refresher->riskfactor;
	/* Start the step function.  */
	process->step = &refresh_process_step_init;

	/* Install.  */
	refresher->process = process;

	/* Start the reawaken loop.  Defer by 0 seconds.  */
	refresher->reawaken = new_reltimer(refresher->timers,
					   refresher,
					   time_from_sec(0),
					   &refresher_reawaken,
					   refresher);
}

/** refresh_process add node - Add the node to the priority queue.
 *
 * @process - the process whose priority queue is to be updated.
 * @node - the node to add to the priority queue.
 */
static void
refresh_process_add_node(struct refresh_process *process,
			 struct node *node,
			 priority_type priority)
{
	/* Create a malloc-ed copy of the node_id.  */
	struct node_id *item = malloc(sizeof(struct node_id));
	if (!item) {
		status_unusual("dhcache_refresher: out of memory for putting node to priority queue!");
		return;
	}
	*item = node->id;
	priority_queue_add(process->queue, item, priority);
}

/** refresh_process_step_init - Perform necessary initializations
 * for the refresh process.
 *
 * @process - the process to perform this step for.
 */
static enum refresh_process_step_result
refresh_process_step_init(struct refresh_process *process)
{
	struct routing_state *rstate = process->rstate;
	struct dhcache_writer *writer = &process->writer;
	struct node *self;

	self = get_node(rstate, &rstate->local_id);
	if (!self) {
		status_trace("dhcache_refresher: Self node %s not found.",
			     type_to_string(tmpctx, struct node_id,
					    &rstate->local_id));
		return refresh_process_step_result_failed;
	}
	status_trace("dhcache_refresher: Start refresh process.");

	/* Clear everything.  */
	dhcache_writer_clear_all_nodes(writer, rstate);

	/* Set self to 0 distance and mark as visited.  */
	dhcache_writer_set_distance(writer, self, 0);
	dhcache_writer_mark_visited(writer, self);

	/* Add the self to the priority queue.  */
	refresh_process_add_node(process, node, 0);

	/* Change the next step.  */
	process->step = &refresh_process_step_loop;

	return refresh_process_step_result_continue;
}

/** refresh_process_step_init - Perform one step in the refresh
 * process loop.
 *
 * @process - the process to perform this step for.
 */
static enum refresh_process_step_result
refresh_process_step_loop(struct refresh_process *process)
{
	struct routing_state *rstate = process->rstate;
	struct dhcache_writer *writer = &process->writer;
	struct dhcache_coster *coster = &process->coster;

	struct node_id *item;
	struct node_id node_id;
	struct node *node;

	struct chan_map_iter it;
	struct chan *c;

	u32 node_total_cost;

	/* If priority queue is empty, finished.  */
	item = priority_queue_get_min(process->queue);
	if (!item)
		return refresh_process_step_result_completed;

	/* Copy to stack space and free the heap object.  */
	node_id = *item;
	free(item);

	/* Find the node in the rstate.  */
	node = get_node(rstate, &node_id);
	/* The node may have disappeared under us while we were
	 * sleeping!
	 * If node is no longer findable, just continue with
	 * next node.
	 */
	if (!node)
		return refresh_process_step_result_continue;

	node_total_cost = dhcache_writer_get_distance(writer, node);

	/* Go through other nodes.  */
	for (c = first_chan(node, &it); c; c = next_chan(node, &it)) {
		struct node *neighbor = other_node(node, c);
		struct amount_msat cost = dhcache_coster_get(coster,
							     node,
							     c,
							     neighbor);
		u64 cost_num = cost.millisatoshis; /* RAW: costing.  */
		u64 neighbor_total_cost = (u64)node_total_cost + cost_num;
		/* Keep within range.  */
		if (neighbor_total_cost > DHCACHE_MAXIMUM_DISTANCE)
			neighbor_total_cost = DHCACHE_MAXIMUM_DISTANCE;

		/* Is it a good candidate?  */
		if (!dhcache_writer_get_visited(writer, neighbor)
		 || (dhcache_writer_get_distance(writer, neighbor)
		     > neighbor_total_cost)) {
			dhcache_writer_mark_visited(writer, neighbor);
			dhcachw_writer_set_distance(writer, neighbor,
						    neighbor_total_cost);
			refresh_process_add_node(process, neighbor,
						 neighbor_total_cost);
		}
	}

	/* Keep going.  */
	return refresh_process_step_result_continue;
}

void
dhcache_refresher_immediate_trigger(struct dhcache_refresher *refresher)
{
	/* If a deferred trigger is waiting, cancel it.  */
	if (refresher->deferred)
		refresher->deferred = tal_free(refresher->deferred);
	/* If the process is ongoing, this call does nothing.  */
	if (refresher->process)
		return;
	/* Else install the process.  */
	install_refresh_process(refresher);
}

void
dhcache_refresher_deferred_trigger(struct dhcache_refresher *refresher)
{
	/* If a deferred trigger or process is ongoing, do nothing.  */
	if (refresher->deferred || refresher->process)
		return;
	/* Else schedule the deferred trigger.  */
	refresher->deferred = new_reltimer(refresher->timers,
					   refresher,
					   refresher->defer_time,
					   &dhcache_refresher_immediate_trigger,
					   refresher);
}

struct dhcache_refresher *
dhcache_refresher_new(const tal_t *ctx,
		      struct routing_state *rstate,
		      struct timers *timers,
		      struct dhcache *dhcache,
		      void (*refresh_cb)(void*),
		      void *refresh_cb_arg)
{
	struct dhcache_refresher *refresher;

	refresher = tal(ctx, struct dhcache_refresher);
	refresher->rstate = rstate;
	refresher->timers = timers;
	refresher->dhcache = dhcache;
	refresher->refresh_cb = refresh_cb;
	refresher->refresh_cb_arg = refresh_cb_arg;

	/* Default settings.  */
	refresher->sample_amount = DHCACHE_REFRESHER_DEFAULT_SAMPLE_AMOUNT;
	refresher->riskfactor = DHCACHE_REFRESHER_DEFAULT_SAMPLE_RISKFACTOR;
	refresher->defer_time = DHCACHE_REFRESHER_DEFAULT_DEFER_TIME;

	/* Set invalid values for coster.  */
	refresher->coster.sample_amount = AMOUNT_MSAT(UINT64_MAX);
	refresher->coster.riskfactor = nan("");

	/* Clear process-related fields.  */
	refresher->deferred = NULL;
	refresher->process = NULL;
	refresher->reawaken = NULL;

	return refresher;
}

/* Accessors.  */
struct amount_msat
dhcache_refresher_get_sample_amount(
		const struct dhcache_refresher *refresher)
{
	return refresher->sample_amount;
}
void
dhcache_refresher_set_sample_amount(
		struct dhcache_refresher *refresher,
		struct amount_msat amount)
{
	refresher->sample_amount = amount;
}
double
dhcache_refresher_get_sample_riskfactor(
		const struct dhcache_refresher *refresher)
{
	return refresher->riskfactor;
}
void
dhcache_refresher_set_sample_riskfactor(
		struct dhcache_refresher *refresher,
		double riskfactor)
{
	refresher->riskfactor = riskfactor;
}
struct timerel
dhcache_refresher_get_defer_time(
		const struct dhcache_refresher *refresher)
{
	return refresher->defer_time;
}
void
dhcache_refresher_set_defer_time(
		struct dhcache_refresher *refresher,
		struct timerel time)
{
	refresher->defer_time = time;
}

/* dhcache_coster operations.  */
void dhcache_coster_init(struct dhcache_coster *coster,
			 const struct dhcache_refresher *refresher)
{
	/* dhcache should have been flipped at least once.  */
	assert(dhcache_available(refresher->dhcache));
	/* refresher->coster must now contain valid data, not
	 * the initial values.
	 */
	assert(!amount_msat_eq(refresher->coster.sample_amount,
			       AMOUNT_MSAT(UINT64_MAX)));
	assert(!isnan(refresher->coster.riskfactor));

	*coster = dhcache->coster;
}
struct amount_msat dhcache_coster_get(const struct dhcache_coster *coster,
				      struct node *from,
				      struct chan *channel,
				      struct node *to)
{
	double riskfactor_per_block;
	int idx;
	u64 cost;

	riskfactor_per_block = coster->riskfactor / BLOCKS_PER_YEAR / 100;
	idx = channel->nodes[1] == from;
	assert(channel->nodes[idx] == from);
	assert(channel->nodes[!idx] == to);

}

