#ifndef LIGHTNING_PLUGINS_LIBPLUGIN_SPARK_H
#define LIGHTNING_PLUGINS_LIBPLUGIN_SPARK_H
#include "config.h"

#include "libplugin.h"
#include <ccan/short_types/short_types.h>
#include <ccan/typesafe_cb/typesafe_cb.h>

/** struct plugin_spark
 *
 * @brief Represents a task being done concurrently
 * while processing a plugin command.
 *
 * @desc Constructed via `plugin_start_spark`,
 * triggering a new task to run concurrently with
 * the invoker.
 * The spark will start execution once the invoker
 * gets blocked, for example on a `send_outreq`,
 * or on a `plugin_wait_spark`.
 * Objects of this type can only be destroyed by
 * `plugin_wait_spark` or one of its variants.
 *
 * Do NOT depend on this being tal-allocated, and
 * do not free it by any means other than waiting
 * on it.
 * In particular do not `tal` using an object of
 * this type as parent.
 *
 * Sparks are given access to the `struct command *`
 * that started them, if any, and if they succeed or
 * fail the command, other sparks of the same
 * command will silently be dropped (and freed).
 * Other sparks that are currently blocked on
 * `send_outreq` will be dropped but the command
 * will continue; the command output will be
 * logged at debug level.
 * If you need to clean up memory, you should
 * tal-allocate them from the command.
 * If you need to clean up something more
 * complicated, you should install a
 * tal-destructor on the command or an object
 * tal-allocated from the command.
 */
struct plugin_spark;

/** struct plugin_spark_completion
 *
 * @brief Represents the "self" of the spark.
 *
 * @desc A token provided to a spark.
 * This is freed by a `plugin_spark_complete`,
 * which signals as well that the spark has
 * finished processing and any waiters on it
 * can resume processing.
 */
struct plugin_spark_completion;

/** plugin_start_spark
 *
 * @brief Starts a new spark.
 *
 * @desc Initiates a "spark", i.e. allows to
 * issue commands via `send_outreq` concurrently
 * with other invocations of `send_outreq`.
 * The returned spark is then cleaned up by
 * `plugin_wait_spark` or one of its variants;
 * those calls will only call the callback once
 * the required spark(s) have signalled their
 * completion via `plugin_spark_complete`.
 *
 * The spark can invoke `send_outreq`, providing
 * the same `cmd` argument it receives.
 *
 * @param cmd - The command for which this spark
 * is executing; cannot be NULL.
 * If the command is completed or failed, execution
 * of the spark will get cancelled.
 * @param cb - The function that executes the
 * processing to be done within the spark.
 * @param arg - The argument that is passed to
 * the callback.
 *
 * @return The spark that was started.
 * Clean this up with `plugin_wait_spark`.
 */
#define plugin_start_spark(cmd, cb, arg) \
	plugin_start_spark_((cmd), \
			    typesafe_cb_preargs(struct command_result *, \
						void *, \
						(cb), (arg), \
						struct command *, \
						struct plugin_spark_completion *), \
			    (void*)(arg))
struct plugin_spark *
plugin_start_spark_(struct command *cmd,
		    struct command_result *(*cb)(struct command *,
						 struct plugin_spark_completion *,
						 void *arg),
		    void *arg);

/** plugin_wait_spark
 *
 * @brief wait for one spark to complete.
 *
 * @desc Schedule the callback to be called
 * once the specified spark has completed.
 * If the spark has already completed, the
 * callback will be scheduled on the next
 * mainloop iteration.
 * If the command gets failed or succeeded
 * while waiting, the callback will never
 * get called and the command (and anything
 * tal-allocated from the command) is freed.
 *
 * Also clears the given spark variable,
 * since this also doubles as freeing the
 * spark.
 * Clearing is done before this function
 * returns, so can safely pass in a local
 * auto variable.
 *
 * There can only be one waiter pending on
 * each spark, as resuming after the spark
 * has completed will cleanup the spark
 * resources.
 *
 * @param cmd - The command being processed,
 * cannot be NULL.
 * @param pspark - Pointer to a variable
 * containing the spark.
 * Cannot be NULL, but can point to a
 * variable that is already NULL, in which
 * case the callback is scheduled
 * immediately.
 * Cleared on entry to this function.
 * @param cb - The callback to invoke when
 * the spark has completed.
 * @param arg - The argument that is passed to
 * the callback.
 *
 * @return Tail-call this function in your
 * processing.
 */
#define plugin_wait_spark(cmd, pspark, cb, arg) \
	plugin_wait_spark_((cmd), (pspark), \
			   typesafe_cb_preargs(struct command_result *, \
					       void *, \
					       (cb), (arg), \
					       struct command *), \
			   (arg))
struct command_result *
plugin_wait_spark_(struct command *cmd,
		   struct plugin_spark **pspark,
		   struct command_result *(*cb)(struct command *cmd,
						void *arg),
		   void *arg);

/** plugin_wait_all_sparks
 *
 * @brief Like `plugin_wait_spark` except
 * resumes on all given sparks.
 *
 * @param cmd - The command being processed,
 * cannot be NULL.
 * @param num_sparks - Number of elements
 * in the pspark array.
 * Can be 0, in which case the callback is
 * called immediately.
 * @param pspark - Pointer to an array
 * containing the sparks.
 * Cannot be NULL, but the pointed array
 * can have entries that are already NULL.
 * If all entries are NULL then the callback
 * is scheduled immediately.
 * This array must persist beyond the return
 * of this function and thus cannot be a
 * local variable!
 * The array need not be tal-allocated, but
 * it should be tal-allocated off or part of
 * a structure tal-allocated off the command,
 * or otherwise freed when the command object
 * gets freed.
 * @param cb - The callback to invoke when
 * all of the sparks have completed.
 * @param arg - The argument that is passed to
 * the callback.
 *
 * @return Tail-call this function in your
 * processing.
 */
#define plugin_wait_all_sparks(cmd, num_sparks, pspark, cb, arg) \
	plugin_wait_all_sparks_((cmd), (num_sparks), (pspark), \
				typesafe_cb_preargs(struct command_result *, \
						    void *, \
						    (cb), (arg), \
						    struct command *), \
				(arg))
struct command_result *
plugin_wait_all_sparks_(struct command *cmd,
			size_t num_sparks,
			struct plugin_spark **pspark,
 			struct command_result *(*cb)(struct command *cmd,
						     void *arg),
			void *arg);

/** plugin_spark_complete
 *
 * @brief Called by the spark to signal that
 * it has completed its processing.
 *
 * @desc Signals that this spark has completed.
 * This ends processing of the spark.
 * If the spark completes the command (i.e.
 * fails or succeeds it) then spark completion
 * for all the sparks of the command is
 * automatically implied, and the spark that
 * completes the command does not need to
 * call plugin_spark_complete either.
 *
 * @param cmd - The command passed in to the
 * spark.
 * @param completion - The completion token
 * that was passed to this spark.
 *
 * @return Tail-call this function in your
 * processing.
 */
struct command_result *
plugin_spark_complete(struct command *cmd,
		      struct plugin_spark_completion *completion);

#endif /* LIGHTNING_PLUGINS_LIBPLUGIN_SPARK_H */


