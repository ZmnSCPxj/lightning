#ifndef LIGHTNING_GOSSIPD_DHCACHE_H
#define LIGHTNING_GOSSIPD_DHCACHE_H
#include "config.h"
#include <assert.h>
#include <ccan/tal/tal.h>
#include <gossipd/routing.h>

/*~ Differential Heuristic
 *
 * The differential heuristic is a heuristic for A-star and
 * other guided pathfinding algorithms.
 *
 * Under this scheme, the map is first preprocessed, by
 * selecting one or more landmarks.
 * These landmarks are ideally as far apart from each
 * other as possible, often corners of a 2d square map.
 *
 * During preprocessing, for each landmark, we execute a
 * "full" Dijkstra starting at that landmark.
 * This creates a distance from each node of the map to
 * that particular landmark.
 * We store this distance-to-landmark on each node; each
 * node contains one distance-to-landmark for each
 * landmark we selected.
 *
 * This preprocessing is generally done only once, then
 * several thousands of pathfinding A-star runs are
 * executed using the differential heuristic.
 *
 * During actual pathfinding, A-star requires a heuristic
 * `h(n, g)`, which gives an estimate of the distance from
 * node `n` to the goal node `g`.
 * With differential heuristic:
 *
 *   h(n, g) = max for all landmarks l: abs(d(n, l) - d(g, l))
 *
 * That is, we iterate over landmarks and get the distance
 * between `n` to that landmark `l`, the distance between the
 * goal `g` to that landmark `l`, get the absolute difference,
 * then use the largest absolute difference among all
 * landmarks as the result of the heuristic.
 *
 * Of note is that if we use a single landmark, and the
 * landmark happens to be the goal node, then the
 * differential heuristic is an exact heuristic and
 * A-star completes very accurately and very quickly.
 */

/*~ Differential Heuristic Cache (dhcache)
 *
 * The actual cached data is stored in struct node, in
 * the dhcache_distance[2] field.
 * The differential heuristic cache object handles
 * access to the cached data.
 *
 * We have a single landmark: our own node.
 * Note that this implies that if we route backwards
 * from payee to payer, and most routefinding attempts
 * are going to have our own node as payer, then we
 * are an exact heuristic, at least ignoring the fact
 * that the map changes dynamically over time.
 *
 * Preprocessing of the map involves writing to one
 * of the dhcache_distance entries using a Dijkstra
 * algorithm that just measures the cost of reaching
 * every node from the landmark (our own node).
 * The other dhcache_distance entry remains in use
 * for pathfinding algorithms.
 * Then, when preprocessing completes, the two entries
 * are swapped and the next preprocessing cycle
 * uses the other entry.
 * This is just standard double-bufferring, common
 * in video games.
 *
 * Because we "pre"process the map regularly, this
 * is actually nearer to a refresh of the cached
 * differential heuristic data.
 * A separate module handles this refresh; this
 * module only provides interfaces to the data.
 */

/** struct dhcache
 *
 * Represents the cache of differential heuristics
 * stored in every node.
 */
struct dhcache;

/** dhcache_new - Construct a new tal-allocated
 * struct dhcache that currently contains no
 * cached distances.
 *
 * @ctx - the parent to tal-allocate from.
 */
struct dhcache *dhcache_new(const tal_t *ctx);

/** dhcache_is_available - Determine if the dhcache
 * has cached distance data available.
 *
 * @dhcache - the dhcache to query.
 */
bool dhcache_available(const struct dhcache *dhcache);

/** dhcache_flip - Flips the double-buffering of
 * the dhcache.
 * The current dhcache_distance field being read
 * by pathfinding algorithms (dhcache_reader)
 * is swapped with the current dhcache_distance
 * field being written by future preprocessing
 * cycles (dhcache_writer).
 *
 * @dhcache - the dhcache to flip.
 *
 * Postconditions: Any dhcache_reader or dhcache_writer
 * in existence becomes invalid.
 * The dhcache becomes available if it was not already
 * available.
 */
void dhcache_flip(struct dhcache *dhcache);

/*~ The dhcache_distance fields are two u32 entries
 * in a small [2] array.
 *
 * Each u32 has 1 bit for the visited/unvisited
 * flag.
 * This visited/unvisited flag also doubles as
 * reachable/unreachable flag during routefinding;
 * if the node was not visited after the
 */

/** DHCACHE_MAXIMUM_DISTANCE - the maximum storable
 * distance.
 */
#define DHCACHE_MAXIMUM_DISTANCE (0x7FFFFFFF)
/** DHCACHE_DISTANCE_MASK - the bits where the
 * distance is stored in the dhcache_distance
 * fields.
 */
#define DHCACHE_DISTANCE_MASK (0x7FFFFFFF)
/** DHCACHE_VISITED_MASK - the bit where the
 * visited/unvisted flag (reachable/unreachable
 * flag during routefinding) is stored in the
 * dhcache_distance fields.
 * The meaning is 0 = unvisited/unreachable,
 * 1 = visited/reachable.
 */
#define DHCACHE_VISITED_MASK (0x80000000)
/** DHCACHE_NEWNODE_VALUE - the value to put
 * when a new node is allocated.
 * We mark the new node as visited already
 * so that the routefinding algorithms will
 * not reject it as unreachable, and
 * give it the maximum distance value as
 * we do not know its distance (this will
 * tend to make routefinding avoid it).
 */
#define DHCACHE_NEWNODE_VALUE (0xFFFFFFFF)
/** DHCACHE_START_PREPROCESSING_VALUE - the
 * value to put when we start a new
 * preprocessing cycle.
 * We set it to unvisited (because the
 * preprocessing algorithm will be
 * responsible for visiting the node)
 * and the maximum distance (because
 * every practical distance will be
 * smaller than the maximum distance).
 */
#define DHCACHE_START_PREPROCESSING_VALUE (0x7FFFFFFF)

/** dhcache_node_init - initializes the
 * dhcache_distance field of a new struct
 * node.
 *
 * @node - the node to initialize.
 */
static inline
void dhcache_node_init(struct node *node)
{
	node->dhcache_distance[0] = DHCACHE_NEWNODE_VALUE;
	node->dhcache_distance[1] = DHCACHE_NEWNODE_VALUE;
}

/** struct dhcache_reader - Represents a
 * reader/pathfinder view into the distance
 * cache.
 *
 * The fields are private and should not be
 * used directly by client code.
 * They are only exposed here to allow
 * compiler optimization.
 *
 * @selector - Whether we should read from
 * dhcache_distance[1] or [0].
 * @distance_goal - The distance from the
 * goal node to the landmark node.
 */
struct dhcache_reader {
	int selector;
	u32 distance_goal;
};

/** dhcache_reader_init - Initializes a dhcache_reader
 * from a dhcache.
 *
 * Preconditions: dhcache_is_available should have
 * returned true on the given dhcache.
 *
 * @reader - the dhcache_reader to initialize.
 * @dhcache - the dhcache to read from.
 * @goal - the goal node.
 */
void dhcache_reader_init(struct dhcache_reader *reader,
			 const struct dhcache *dhcache,
			 const struct node *goal);

/** dhcache_reader_is_reachable - Determine if the
 * given node is known to be reachable or not.
 *
 * @reader - the dchache_reader to reference.
 * @node - the node whose reachability is to be
 * queried.
 */
static inline
bool dhcache_reader_is_reachable(const struct dhcache_reader *reader,
				 const struct node *node)
{
	return !!(node->dhcache_distance[reader->selector]
		  & DHCACHE_VISITED_MASK);
}
/** dhcache_reader_distance - Determine the heuristic
 * distance to the goal node.
 *
 * Preconditions: the dhcache_reader_is_reachable
 * should have returned true for the node.
 *
 * @reader - the dchache_reader to reference.
 * @node - the node whose distance from the goal is
 * to be queried.
 */
static inline
u32 dhcache_reader_distance(const struct dhcache_reader *reader,
			    const struct node *node)
{
	u32 distance_goal = reader->distance_goal;
	u32 distance_node = node->dhcache_distance[reader->selector]
			  & DHCACHE_DISTANCE_MASK;
	assert(dhcache_reader_is_reachable(reader, node));
	if (distance_node > distance_goal)
		return distance_node - distance_goal;
	else
		return distance_goal - distance_node;
}

/** dhcache_writer - Represents a
 * preprocessor/refresher view of the distance
 * cache.
 *
 * The fields are private and should not be
 * used directly by client code.
 * They are only exposed here to allow
 * compiler optimization.
 *
 * @selector - Whether we should read and update
 * dhcache_distance[1] or [0].
 */
struct dhcache_writer {
	int selector;
};


/** dhcache_writer_init - Initializes the given
 * writer from a dhcache.
 *
 * @writer - the dhcache_writer to initialize.
 * @dhcache - the dhcache to initialize from.
 */
void dhcache_writer_init(struct dhcache_writer *writer,
			 const struct dhcache *dhcache);

/** dhcache_writer_clear_all_nodes - Set all
 * dhcache_distance fields in all nodes of
 * the specified routing_state to the starting
 * state.
 * Unvisited and at maximum distance.
 *
 * @writer - the dhcache_writer to query.
 * @rstate - the routing state to clear.
 */
void dhcache_writer_clear_all_nodes(const struct dhcache_writer *writer,
				    struct routing_state *rstate);

/** dhcache_writer_get_visited - Determine if
 * we have already visited the node.
 *
 * @writer - the dhcache_writer to query.
 * @node -the node to determine if already visited.
 */
static inline
bool dhcache_writer_get_visited(const struct dhcache_writer *writer,
				const struct node *node)
{
	return !!(node->dhcache_distance[writer->selector]
		  & DHCACHE_VISITED_MASK);
}

/** dhcache_writer_mark_visited - Set the
 * node to already visited.
 *
 * @writer - the dhcache_writer to query.
 * @node - the node to mark as visited.
 *
 * Postconditions: the node is marked
 * visited.
 */
static inline
void dhcache_writer_mark_visited(const struct dhcache_writer *writer,
				 struct node *node)
{
	node->dhcache_distance[writer->selector] |= DHCACHE_VISITED_MASK;
}

/** dhcache_writer_get_distance - Determine
 * the current distance of the specified node.
 *
 * @writer - the dhcache_writer to query.
 * @node - the node whose distance from landmark is to be queried.
 */
static inline
u32 dhcache_writer_get_distance(const struct dhcache_writer *writer,
				const struct node *node)
{
	return node->dhcache_distance[writer->selector]
	     & DHCACHE_DISTANCE_MASK;
}

/** dhcache_writer_set_distance - Set the
 * current distance of the specified node.
 *
 * @writer - the dhcache_writer to query.
 * @node - the node whose distance from landmark will be changed.
 * @distance - the value to set the distance to.
 *
 * Postconditions: the distance of the node
 * is set to the specified distance.
 */
static inline
void dhcache_writer_set_distance(const struct dhcache_writer *writer,
				 struct node *node,
				 u32 distance)
{
	int selector = writer->selector;
	u32 visited = node->dhcache_distance[selector]
		    & DHCACHE_VISITED_MASK;
	assert(distance <= DHCACHE_MAXIMUM_DISTANCE);
	node->dhcache_distance[selector] = visited
					   | (distance
					      & DHCACHE_DISTANCE_MASK);
}

#endif /* LIGHTNING_GOSSIPD_DHCACHE_H */
