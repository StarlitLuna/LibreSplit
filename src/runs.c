#include "runs.h"
#include "logging.h"
#include <string.h>

/**
 * @brief Creates a new attempts array and assigns it to
 * the pointers in runs.
 *
 * @param runs Pointer to the memory location to store the new array
 * @return int 0 on success otherwise failure
 */
int ls_runs_create(ls_runs** runs)
{
    int error = 0;
    ls_runs* self = calloc(1, sizeof(ls_runs));
    if (self == NULL) {
        error = 1;
        goto ls_runs_create_error;
    }

    self->attempts = calloc(INITIAL_ATTEMPTS_ARRAY_SIZE, sizeof(ls_attempt*));
    if (self->attempts == NULL) {
        error = 1;
        goto ls_runs_create_error;
    }

    self->size = INITIAL_ATTEMPTS_ARRAY_SIZE;
    self->count = 0;

ls_runs_create_error:
    if (error) {
        if (self) {
            ls_runs_release(self);
        }

        return error;
    }

    // free old runs before replacing.
    if (*runs) {
        ls_runs_release(*runs);
        *runs = 0;
    }

    *runs = self;
    return 0;
}

/**
 * @brief Frees all attempt data for an individual attempt
 *
 * @param attempt the attempt instance
 */
static void ls_attempt_release(ls_attempt* attempt)
{
    free(attempt->split_times);
    free(attempt->segment_times);
    free(attempt->reason);

    for (unsigned int i = 0; i < attempt->split_count; ++i) {
        free(attempt->split_titles[i]);
    }

    free(attempt->split_titles);
    free(attempt);
}

/**
 * @brief Frees all runs data
 *
 * @param self the runs instance
 */
void ls_runs_release(ls_runs* self)
{
    for (size_t i = 0; i < self->count; i++) {
        ls_attempt_release(self->attempts[i]);
    }

    free(self->attempts);
    self->count = 0;
    self->size = 0;

    free(self);
}

/**
 * @brief Resizes the dynamic array at a rate of 1.5x its current size
 *
 * @param self The runs instance
 * @return bool Whether or not the reallocation succeeded
 */
static bool grow(ls_runs* self)
{
    if (self->size >= MAX_ATTEMPTS_ARRAY_CAPACITTY) {
        // TODO: Dump this to disk first.
        return ls_runs_clear(self);
    }

    size_t new_size = self->size + ((size_t)(self->size / 2));
    if (new_size > MAX_ATTEMPTS_ARRAY_CAPACITTY) {
        new_size = MAX_ATTEMPTS_ARRAY_CAPACITTY;
    }

    size_t old_size = self->size;
    ls_attempt** new_attempts = realloc(self->attempts, new_size * sizeof(ls_attempt*));
    if (new_attempts == NULL) {
        LOG_WARNF("unable to reallocate runs to new size of: %zu", new_size);
        return false;
    }

    self->attempts = new_attempts;
    self->size = new_size;

    // NULL new memory block
    memset(self->attempts + old_size, 0, (new_size - old_size) * sizeof(ls_attempt*));
    return true;
}

/**
 * @brief Appends the attempt to the next empty slot in the array
 * and increments the count. Automatically grows the array
 * when nearing capacity.
 *
 * Always appends the entry to the array even if growth fails.
 * When growth fails we should prevent new runs since storing the
 * attempt after that point becomes impossible.
 *
 * @param self The runs instance.
 * @param attempt The attempt instance to append to runs.
 * @return Whether or not the array grew successfully. When no growth is needed, always true.
 */
bool ls_runs_append(ls_runs* self, ls_attempt* attempt)
{
    self->attempts[self->count++] = attempt;
    if (self->count == self->size) {
        return grow(self);
    }

    return true;
}

/**
 * @brief Clears the array and reduces memory usage.
 *
 * @param self The current runs instance.
 * @return bool Whether or not the clear succeeded.
 */
bool ls_runs_clear(ls_runs* self)
{
    for (size_t i = 0; i < self->count; i++) {
        ls_attempt_release(self->attempts[i]);
    }

    free(self->attempts);
    self->attempts = calloc(INITIAL_ATTEMPTS_ARRAY_SIZE, sizeof(ls_attempt*));
    if (self->attempts == NULL) {
        LOG_WARN("unable to allocate runs after clear");
        return false;
    }

    self->size = INITIAL_ATTEMPTS_ARRAY_SIZE;
    self->count = 0;
    return true;
}

/**
 * @brief Creates a new persistent attempt based on the current timer state
 * and timer completion reason.
 *
 * @param timer The current timer instance.
 * @param reason The reason for the run termination.
 * @return ls_attempt*
 */
ls_attempt* ls_runs_new_attempt(ls_timer* timer, const char* reason)
{
    if (reason == NULL) {
        LOG_WARN("invalid NULL reason provided");
        return NULL;
    }

    ls_attempt* attempt = calloc(1, sizeof(ls_attempt));
    if (attempt == NULL) {
        LOG_WARN("unable to allocate a new attempt");
        goto ls_runs_new_attempt_failed;
    }

    const size_t split_count = timer->game->split_count;
    size_t time_size = split_count * sizeof(ls_time);
    attempt->split_count = split_count;
    attempt->split_times = calloc(1, time_size);
    if (attempt->split_times == NULL) {
        LOG_WARN("unable to allocate `split_times` for the attempt");
        goto ls_runs_new_attempt_failed;
    }

    attempt->segment_times = calloc(1, time_size);
    if (attempt->segment_times == NULL) {
        LOG_WARN("unable to allocate `segment_times` for the attempt");
        goto ls_runs_new_attempt_failed;
    }

    attempt->reason = strdup(reason);
    if (attempt->segment_times == NULL) {
        LOG_WARN("unable to duplicate `reason` for the attempt");
        goto ls_runs_new_attempt_failed;
    }

    attempt->split_titles = calloc(1, split_count * sizeof(char*));
    if (attempt->segment_times == NULL) {
        LOG_WARN("unable to allocate `segment_times` for the attempt");
        goto ls_runs_new_attempt_failed;
    }

    for (unsigned int i = 0; i < split_count; ++i) {
        attempt->split_titles[i] = strdup(timer->game->split_titles[i]);
        if (attempt->segment_times == NULL) {
            LOG_WARNF("unable to duplicate `split_titles[%u]` for the attempt", i);
            goto ls_runs_new_attempt_failed;
        }
    }

    memcpy(attempt->split_times, timer->split_times, time_size);
    memcpy(attempt->segment_times, timer->segment_times, time_size);
    attempt->final_time = ls_timer_get_time(timer, true);
    return attempt;

ls_runs_new_attempt_failed:
    ls_attempt_release(attempt);
    return NULL;
}
