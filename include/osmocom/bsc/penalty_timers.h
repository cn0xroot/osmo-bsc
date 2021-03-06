/* Manage a list of penalty timers per BTS;
 * initially used by handover algorithm 2 to keep per-BTS timers for each subscriber connection. */
#pragma once

/* Opaque struct to manage penalty timers */
struct penalty_timers;

/* Initialize a list of penalty timers.
 * param ctx: talloc context to allocate in.
 * returns an empty struct penalty_timers.  */
struct penalty_timers *penalty_timers_init(void *ctx);

/* Add a penalty timer for an arbitary object.
 * Note: the ownership of for_object remains with the caller; it is handled as a mere void* value, so
 * invalid pointers can be handled without problems, while common sense dictates that invalidated
 * pointers (freed objects) should probably be removed from this list. More importantly, the pointer must
 * match any pointers used to query penalty timers, so for_object should reference some global/singleton
 * object that tends to stay around longer than the penalty timers.
 * param pt: penalty timers list as from penalty_timers_init().
 * param for_object: arbitrary pointer reference to store a penalty timer for (passing NULL is possible,
 *         but note that penalty_timers_clear() will clear all timers if given for_object=NULL).
 * param timeout: penalty time in seconds. */
void penalty_timers_add(struct penalty_timers *pt, const void *for_object, int timeout);

/* Return the amount of penalty time remaining for an object.
 * param pt: penalty timers list as from penalty_timers_init().
 * param for_object: arbitrary pointer reference to query penalty timers for.
 * returns seconds remaining until all penalty time has expired. */
unsigned int penalty_timers_remaining(struct penalty_timers *pt, const void *for_object);

/* Clear penalty timers for one or all objects.
 * param pt: penalty timers list as from penalty_timers_init().
 * param for_object: arbitrary pointer reference to clear penalty time for,
 *                   or NULL to clear all timers. */
void penalty_timers_clear(struct penalty_timers *pt, const void *for_object);

/* Free a struct as returned from penalty_timers_init().
 * Clear all timers from the list, deallocate the list and set the pointer to NULL.
 * param pt: pointer-to-pointer which references a struct penalty_timers as returned by
 *            penalty_timers_init(); *pt_p will be set to NULL. */
void penalty_timers_free(struct penalty_timers **pt_p);
