#include "dhcache.h"

struct dhcache {
	u8 writer_selector;
	u8 available;
};

struct dhcache *dhcache_new(const tal_t *ctx)
{
	struct dhcache *dhcache = tal(ctx, struct dhcache);

	dhcache->writer_selector = 0;
	dhcache->available = 0;

	return dhcache;
}

bool dhcache_available(const struct dhcache *dhcache)
{
	return !!dhcache->available;
}

void dhcache_flip(struct dhcache *dhcache)
{
	dhcache->writer_selector = !dhcache->writer_selector;
	dhcache->available = 1;
}

void dhcache_reader_init(struct dhcache_reader *reader,
			 const struct dhcache *dhcache,
			 const struct node *goal)
{
	int selector = !dhcache->writer_selector;
	reader->selector = selector;
	reader->distance_goal = goal->dhcache_distance[selector]
				& DHCACHE_DISTANCE_MASK;
}

void dhcache_writer_init(struct dhcache_writer *writer,
			 const struct dhcache *dhcache)
{
	writer->selector = !!dhcache->writer_selector;
}

void dhcache_writer_clear_all_nodes(const struct dhcache_writer *writer,
				    struct routing_state *rstate)
{
	int selector = writer->selector;
	struct node_map_iter(it);
	struct node *n;

	for (n = node_map_first(rstate->nodes, &it);
	     n;
	     n = node_map_next(rstate->nodes, &it))
		n->dhcache_distance[selector] = DHCACHE_START_PREPROCESSING_VALUE;
}
