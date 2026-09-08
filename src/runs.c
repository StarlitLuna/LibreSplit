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
    size_t new_size = self->size + ((size_t)(self->size / 2));
    if (new_size > MAX_ATTEMPTS_ARRAY_CAPACITTY) {
        // TODO: Dump this to disk and clear capacity.
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