/*
 * =============================================================================
 *  elevator.c — Elevator Car Thread & State Machine
 *
 *  Each elevator runs as an independent POSIX thread executing a state machine.
 *  The SCAN (elevator) scheduling algorithm services requests in the current
 *  direction of travel before reversing, minimising average wait time.
 *
 *  Movement is simulated using nanosleep():
 *    - FLOOR_TRAVEL_TIME_MS  per floor transition
 *    - DOOR_OPEN_TIME_MS     for passenger boarding
 *
 *  Synchronisation:
 *    - Each elevator's mutex protects its state structure
 *    - The condition variable is signalled by the request manager when
 *      a new request is assigned or a mode change occurs
 * =============================================================================
 */

#include "../include/elevator.h"

/* ---------------------------------------------------------------------------
 * elevator_init — Initialise an elevator to ground floor, idle, doors closed.
 * Called from main() before any threads are spawned.
 * ---------------------------------------------------------------------------*/
void elevator_init(elevator_t *elev, int id)
{
    elev->id            = id;
    elev->current_floor = 1;             /* Start at ground floor */
    elev->state         = STATE_IDLE;
    elev->direction     = DIR_NONE;
    elev->door_status   = DOOR_CLOSED;
    elev->request_count = 0;
    memset(elev->floor_requests, 0, sizeof(elev->floor_requests));

    pthread_mutex_init(&elev->mutex, NULL);
    pthread_cond_init(&elev->cond, NULL);
}

/* ---------------------------------------------------------------------------
 * count_requests — Count how many floors have pending requests.
 * Caller must hold elev->mutex.
 * ---------------------------------------------------------------------------*/
static int count_requests(elevator_t *elev)
{
    int count = 0;
    int i;
    for (i = 1; i <= NUM_FLOORS; i++) {
        if (elev->floor_requests[i])
            count++;
    }
    return count;
}

/* ---------------------------------------------------------------------------
 * find_next_target — SCAN scheduling algorithm.
 *
 * Strategy:
 *   1. Continue in the current direction, find the nearest request.
 *   2. If nothing ahead, reverse and find the nearest request.
 *   3. If idle (DIR_NONE), pick the nearest request in either direction.
 *
 * Returns the 1-based floor number, or -1 if no requests are pending.
 * Caller must hold elev->mutex.
 * ---------------------------------------------------------------------------*/
static int find_next_target(elevator_t *elev)
{
    int i;

    if (elev->direction == DIR_UP) {
        /* Scan upward from current floor */
        for (i = elev->current_floor; i <= NUM_FLOORS; i++) {
            if (elev->floor_requests[i]) return i;
        }
        /* Reverse: scan downward */
        for (i = elev->current_floor - 1; i >= 1; i--) {
            if (elev->floor_requests[i]) return i;
        }
    } else if (elev->direction == DIR_DOWN) {
        /* Scan downward from current floor */
        for (i = elev->current_floor; i >= 1; i--) {
            if (elev->floor_requests[i]) return i;
        }
        /* Reverse: scan upward */
        for (i = elev->current_floor + 1; i <= NUM_FLOORS; i++) {
            if (elev->floor_requests[i]) return i;
        }
    } else {
        /* DIR_NONE — find the nearest request in either direction */
        int nearest = -1;
        int nearest_dist = NUM_FLOORS + 1;
        for (i = 1; i <= NUM_FLOORS; i++) {
            if (elev->floor_requests[i]) {
                int dist = abs(i - elev->current_floor);
                if (dist < nearest_dist) {
                    nearest_dist = dist;
                    nearest = i;
                }
            }
        }
        return nearest;
    }

    return -1;
}

/* ---------------------------------------------------------------------------
 * service_floor — Open doors, wait, close doors at the current floor.
 *
 * Clears the floor request, logs events, and updates the door status.
 * Handles interruption by emergency stop during the door-open period.
 *
 * elev->mutex must be held on entry; is held on return.
 * ---------------------------------------------------------------------------*/
static void service_floor(system_state_t *sys, elevator_t *elev)
{
    /* Mark request as served */
    elev->floor_requests[elev->current_floor] = 0;
    elev->request_count = count_requests(elev);

    /* Open doors */
    elev->state       = STATE_DOOR_OPEN;
    elev->door_status = DOOR_OPEN;
    log_event(sys, "Elevator %d: Doors OPENING at floor %d",
              elev->id + 1, elev->current_floor);

    /* Release lock while doors are open (simulated boarding time) */
    pthread_mutex_unlock(&elev->mutex);
    sleep_ms(DOOR_OPEN_TIME_MS);
    pthread_mutex_lock(&elev->mutex);

    /*
     * Close doors only if not interrupted by emergency stop or maintenance.
     * If the state was changed while doors were open, preserve the new state.
     */
    if (elev->state == STATE_DOOR_OPEN) {
        elev->door_status = DOOR_CLOSED;
        /* Do NOT set state to IDLE here — let the caller decide */
        log_event(sys, "Elevator %d: Doors CLOSED at floor %d",
                  elev->id + 1, elev->current_floor);
    }
}

/* ---------------------------------------------------------------------------
 * elevator_thread — Main loop for one elevator car.
 *
 * Flow per iteration:
 *   1. Wait on condition variable if IDLE with no requests
 *   2. Handle special states (EMERGENCY_STOP, MAINTENANCE, DOOR_OPEN)
 *   3. Pick next target floor via SCAN algorithm
 *   4. If already at target → service (open / close doors)
 *   5. Otherwise → move one floor toward target (nanosleep)
 *   6. If the new floor has a pending request → service it
 *   7. Loop
 * ---------------------------------------------------------------------------*/
void *elevator_thread(void *arg)
{
    elevator_thread_arg_t *targ = (elevator_thread_arg_t *)arg;
    system_state_t        *sys  = targ->sys;
    elevator_t            *elev = &sys->elevators[targ->elevator_id];
    int                    target_floor;

    log_event(sys, "Elevator %d: Thread started — standing at floor %d",
              elev->id + 1, elev->current_floor);

    while (sys->running) {
        pthread_mutex_lock(&elev->mutex);

        /* ── 1. Wait while idle and no work ────────────────────────── */
        while (sys->running
               && elev->state == STATE_IDLE
               && count_requests(elev) == 0)
        {
            pthread_cond_wait(&elev->cond, &elev->mutex);
        }

        if (!sys->running) {
            pthread_mutex_unlock(&elev->mutex);
            break;
        }

        /* ── 2. Handle EMERGENCY STOP ──────────────────────────────── */
        if (elev->state == STATE_EMERGENCY_STOP) {
            log_event(sys, "Elevator %d: EMERGENCY STOP — halted at floor %d",
                      elev->id + 1, elev->current_floor);

            while (sys->running && elev->state == STATE_EMERGENCY_STOP) {
                pthread_cond_wait(&elev->cond, &elev->mutex);
            }
            pthread_mutex_unlock(&elev->mutex);

            if (sys->running)
                log_event(sys, "Elevator %d: Emergency cleared — resuming operations",
                          elev->id + 1);
            continue;
        }

        /* ── 3. Handle MAINTENANCE mode ────────────────────────────── */
        if (elev->state == STATE_MAINTENANCE) {
            log_event(sys, "Elevator %d: Entering MAINTENANCE mode",
                      elev->id + 1);

            while (sys->running && elev->state == STATE_MAINTENANCE) {
                pthread_cond_wait(&elev->cond, &elev->mutex);
            }
            pthread_mutex_unlock(&elev->mutex);

            if (sys->running)
                log_event(sys, "Elevator %d: Maintenance complete — resuming",
                          elev->id + 1);
            continue;
        }

        /* ── 3b. Handle DOOR_OPEN state (interrupted door cycle) ───── */
        if (elev->state == STATE_DOOR_OPEN) {
            /* Door was left open (e.g. emergency during service).
             * Close it before proceeding. */
            elev->door_status = DOOR_CLOSED;
            elev->state       = STATE_IDLE;
            log_event(sys, "Elevator %d: Closing residual open doors at floor %d",
                      elev->id + 1, elev->current_floor);
            pthread_mutex_unlock(&elev->mutex);
            continue;
        }

        /* ── 4. Pick next target (SCAN algorithm) ──────────────────── */
        target_floor = find_next_target(elev);

        if (target_floor < 0) {
            /* No pending requests — go idle */
            elev->state     = STATE_IDLE;
            elev->direction = DIR_NONE;
            pthread_mutex_unlock(&elev->mutex);
            continue;
        }

        /* ── 5. Already at target floor → service immediately ──────── */
        if (target_floor == elev->current_floor) {
            service_floor(sys, elev);
            /*
             * After servicing, check if state was changed by an external
             * event (emergency/maintenance) during the door-open period.
             * Only go idle if state is still DOOR_OPEN (normal completion).
             */
            if (elev->state == STATE_DOOR_OPEN) {
                /* Normal completion: close doors, check for more work */
                elev->door_status = DOOR_CLOSED;
                if (count_requests(elev) == 0) {
                    elev->state     = STATE_IDLE;
                    elev->direction = DIR_NONE;
                } else {
                    elev->state = STATE_IDLE; /* Will pick next target on next iteration */
                }
            }
            /* If state is EMERGENCY/MAINTENANCE, leave it — handled next iteration */
            pthread_mutex_unlock(&elev->mutex);
            continue;
        }

        /* ── 6. Set direction and begin moving ─────────────────────── */
        if (target_floor > elev->current_floor) {
            elev->state     = STATE_MOVING_UP;
            elev->direction = DIR_UP;
        } else {
            elev->state     = STATE_MOVING_DOWN;
            elev->direction = DIR_DOWN;
        }

        log_event(sys, "Elevator %d: Moving %s from floor %d toward floor %d",
                  elev->id + 1,
                  (elev->direction == DIR_UP) ? "UP" : "DOWN",
                  elev->current_floor, target_floor);

        pthread_mutex_unlock(&elev->mutex);

        /* ── 7. Simulate travel time for one floor ─────────────────── */
        sleep_ms(FLOOR_TRAVEL_TIME_MS);

        /* ── 8. Advance one floor ──────────────────────────────────── */
        pthread_mutex_lock(&elev->mutex);

        /* Check for interruption during travel */
        if (elev->state == STATE_EMERGENCY_STOP
            || elev->state == STATE_MAINTENANCE) {
            pthread_mutex_unlock(&elev->mutex);
            continue;   /* Re-enter the main loop to handle the special state */
        }

        /* Update floor position with bounds checking */
        if (elev->direction == DIR_UP) {
            if (elev->current_floor < NUM_FLOORS)
                elev->current_floor++;
        } else if (elev->direction == DIR_DOWN) {
            if (elev->current_floor > 1)
                elev->current_floor--;
        }

        log_event(sys, "Elevator %d: Arrived at floor %d",
                  elev->id + 1, elev->current_floor);

        /* ── 9. Service this floor if it has a request ─────────────── */
        if (elev->floor_requests[elev->current_floor]) {
            service_floor(sys, elev);
        }

        /* ── 10. Re-evaluate after servicing ───────────────────────── */
        /*
         * Only transition to idle if the state hasn't been changed to
         * EMERGENCY_STOP or MAINTENANCE during service_floor().
         */
        if (elev->state == STATE_EMERGENCY_STOP
            || elev->state == STATE_MAINTENANCE) {
            /* Preserve the externally-set state; handled next iteration */
            pthread_mutex_unlock(&elev->mutex);
            continue;
        }

        if (elev->state == STATE_DOOR_OPEN) {
            /* service_floor completed normally but state is still DOOR_OPEN */
            elev->door_status = DOOR_CLOSED;
        }

        if (count_requests(elev) == 0) {
            elev->state     = STATE_IDLE;
            elev->direction = DIR_NONE;
            log_event(sys, "Elevator %d: All requests served — now IDLE at floor %d",
                      elev->id + 1, elev->current_floor);
        } else {
            /* More requests pending — continue in IDLE, next iteration picks target */
            elev->state = STATE_IDLE;
        }

        pthread_mutex_unlock(&elev->mutex);
        /* Loop back: the next iteration will pick the next target */
    }

    log_event(sys, "Elevator %d: Thread exiting", elev->id + 1);
    return NULL;
}
