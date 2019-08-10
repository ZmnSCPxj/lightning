#include "priority_queue.h"
#include <assert.h>
#include <common/utils.h>

/*~ This is a simple binary heap implementation.
 *
 * The paper below:
 * https://www.cs.sunysb.edu/~rezaul/papers/TR-07-54.pdf
 *
 * includes several cache-aware and cache-oblivious
 * algorithms.
 * However, they are complicated to implement and take
 * a good bit more memory.
 */

struct priority_queue_entry {
	priority_type priority;
	priority_item *item;
};

struct priority_queue {
	struct priority_queue_entry *array;
};

struct priority_queue *
priority_queue_new(const tal_t *ctx)
{
	struct priority_queue *queue = tal(ctx, struct priority_queue);
	queue->array = tal_arr(queue, struct priority_queue_entry, 0);
	return queue;
}

void
priority_queue_add(struct priority_queue *queue,
		   priority_item *item,
		   priority_type priority)
{
	struct priority_queue_entry entry;
	size_t i;

	assert(item);

	entry.priority = priority;
	entry.item = item;

	/* Insert item to end of array.  */
	tal_arr_expand(&queue->array, entry);

	/* Upsift.  */
	struct priority_queue_entry *array = queue->array;
	i = tal_count(array) - 1;
	while (i > 0) {
		size_t parent_i = (i - 1) / 2;

		/* Should we upsift?  */
		if (array[i].priority < array[parent_i].priority) {
			entry = array[i];
			array[i] = array[parent_i];
			array[parent_i] = entry;
			i = parent_i;
		} else
			break;
	}
}

priority_item *
priority_queue_get_min(struct priority_queue *queue)
{
	priority_item *item;
	struct priority_queue_entry entry;
	size_t s = tal_count(queue->array);
	size_t i;

	if (s == 0)
		return NULL;

	/* Get minimum.  */
	item = queue->array[0].item;
	/* Handle degenerate case.  */
	if (s == 1) {
		tal_resize(&queue->array, 0);
		return item;
	}

	/* Overwrite minimum with last.  */
	queue->array[0] = queue->array[s - 1];
	/* Delete last.  */
	--s;
	tal_resize(&queue->array, s);

	/* Downsink.  */
	struct priority_queue_entry *array = queue->array;
	i = 0;
	while (i < s) {
		size_t child1_i = i * 2 + 1;
		size_t child2_i = i * 2 + 2;
		size_t child_i;

		/* No more children? */
		if (child1_i >= s)
			break;

		/* Select smaller child.  */
		if (child2_i >= s)
			/* Only one child.  */
			child_i = child1_i;
		else if (array[child1_i].priority < array[child2_i].priority)
			child_i = child1_i;
		else
			child_i = child2_i;

		/* Check if we should downsink.  */
		if (array[child_i].priority < array[i].priority) {
			entry = array[i];
			array[i] = array[child_i];
			array[child_i] = entry;
			i = child_i;
		} else
			break;
	}

	return item;
}

