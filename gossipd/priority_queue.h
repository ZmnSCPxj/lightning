#ifndef LIGHTNING_GOSSIPD_PRIORITY_QUEUE_H
#define LIGHTNING_GOSSIPD_PRIORITY_QUEUE_H
#include "config.h"
#include <ccan/short_types/short_types.h>
#include <ccan/tal/tal.h>

/*~ A*, Dijkstra, and Greedy Best First search
 * all require a priority queue.
 * Nodes are added to the priority queue in
 * arbitrary order.
 * Each node has a priority attached to it,
 * often the cost (or estimated cost) of
 * paths going through that node.
 * Then the lowest-cost node is removed from
 * the priority queue in order to expand
 * its neighbors.
 *
 * Traditionally, priority queues used in
 * these three related pathfinding algorithms
 * define three operations:
 *
 * 1.  Add node.
 * 2.  Get-minimum node.
 * 3.  Decrease priority of node.
 *
 * When a node is expanded, its neighbors are
 * adde to the priority queue via the add-node
 * operation.
 * After we have expanded a node, we drop it
 * from consideration and get the next node
 * to expand via get-minimum.
 * Finally, if we expand a neighbor, and that
 * neighbor is already in the priority queue,
 * we might find that the cost would get
 * reduced and so we reduce the priority
 * of the node.
 *
 * However, according to:
 * https://www.cs.sunysb.edu/~rezaul/papers/TR-07-54.pdf
 *
 * Priority queues that do not implement the
 * decrease-priority operation run faster.
 * The only thing needed to do is to be able
 * to mark nodes already evaluated somehow.
 *
 * Thus, this priority queue implementation
 * does not include a decrease-priority
 * operation.
 */

typedef void priority_item;
typedef u64 priority_type;

/* Opaque type.  */
struct priority_queue;

/** priorirty_queue_new - Construct a priority queue.
 * The object returned is tal-allocated and should be
 * freed by tal_free or by freeing the owner.
 *
 * @ctx - Owner of the priority queue.
 */
struct priority_queue *priority_queue_new(const tal_t* ctx);

/** priority_queue_add - Add an item to the priority
 * queue.
 *
 * @queue - queue to add to.
 * @item - item to add, must be non-NULL.
 * @priority - priority of the item.
 */
void priority_queue_add(struct priority_queue *queue,
			priority_item *item,
			priority_type priority);

/** priority_queue_get_min - Get the item with the
 * lowest priority.
 * Return null, if queue is empty.
 *
 * @queue - queue to get minimum-priority item from.
 */
priority_item *priority_queue_get_min(struct priority_queue *queue);

#endif /* LIGHTNING_GOSSIPD_PRIORITY_QUEUE_H */
