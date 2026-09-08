#include "timer.h"

/**
 * MAX_ATTEMPTS_ARRAY_CAPACITTY ensures that at any given time
 * the maximum amount of memory our in-memory attempts history
 * will be MAX_ATTEMPTS_ARRAY_CAPACITTY * sizeof(ls_attempt)
 * this would be a truly ridiculous number of runs all in one
 * session without ever saving or closing LibreSplit.
 *
 * Using some conservative assumptions about how many splits are
 * in the run, this would be roughly 1GB of ram usage.
 *
 * This memory is not allocated up front, this would truly require
 * the user to sit there doing *completed* runs for years without ever
 * saving or closing to reach that theoretical 1GB of ram usage.
 */
#define INITIAL_ATTEMPTS_ARRAY_SIZE 16 // 2^4
#define MAX_ATTEMPTS_ARRAY_CAPACITTY 524288 // 2^19

/**
 * @brief This holds information about an attempt after it is complete.
 * By complete, I mean the attempt is no longer in `ls_timer` meaning the run
 * finished, was reset etc. This should only hold information from `ls_timer`
 * that actually goes into the user's saved attempt history.
 */
typedef struct ls_attempt {
    ls_time* split_times;
} ls_attempt;

typedef struct ls_attempts {
    ls_attempt** attempts; /**< The attempts array */
    size_t count; /**< The number of attempts in the array i.e. used slots */
    size_t size; /**< The current actual allocation size of the array i.e. total slots */
} ls_attempts;

int ls_attempts_create(ls_attempts** attempts);
void ls_attempts_release(ls_attempts* attempts);
bool ls_attempts_append(ls_attempts* self, ls_attempt* attempt);
bool ls_attempts_clear(ls_attempts* self);