/*
 * =============================================================================
 *  request_manager.c — QNX IPC Request Dispatcher
 *
 *  Runs as a dedicated POSIX thread, blocking on MsgReceive() to accept
 *  requests from the input thread.  For each message:
 *
 *    FLOOR_REQUEST   → dispatch to the optimal elevator (nearest / idle)
 *    EMERGENCY_STOP  → halt the specified elevator immediately
 *    EMERGENCY_CLEAR → resume normal operation
 *    MAINTENANCE_ON  → take an elevator out of service
 *    MAINTENANCE_OFF → return an elevator to service
 *    QUIT            → initiate clean shutdown
 *
 *  All dispatches are synchronous (MsgSend blocks until MsgReply).
 *  This is the core demonstration of QNX Neutrino message-passing IPC.
 * =============================================================================
 */

#include "../include/elevator.h"

/* ---------------------------------------------------------------------------
 * select_best_elevator — Nearest-elevator dispatch algorithm.
 *
 *   1. If a specific elevator is requested (preferred_id ≥ 0) and it is
 *      available, use it.
 *   2. Otherwise, auto-dispatch: skip elevators in EMERGENCY or MAINTENANCE,
 *      prefer IDLE over moving, pick the one closest to the target floor.
 *   3. On tie, prefer the lower-numbered elevator.
 *
 * Returns elevator index (0 or 1), or -1 if none available.
 * ---------------------------------------------------------------------------*/
static int select_best_elevator(system_state_t *sys,
                                int target_floor,
                                int preferred_id)
{
    int best          = -1;
    int best_score    = NUM_FLOORS + 10;   /* lower is better */
    int i;

    /* If a specific elevator was requested, try it first */
    if (preferred_id >= 0 && preferred_id < NUM_ELEVATORS) {
        elevator_t *elev = &sys->elevators[preferred_id];
        pthread_mutex_lock(&elev->mutex);
        int available = (elev->state != STATE_EMERGENCY_STOP
                         && elev->state != STATE_MAINTENANCE);
        pthread_mutex_unlock(&elev->mutex);
        if (available)
            return preferred_id;
    }

    /* Auto-dispatch: evaluate all elevators */
    for (i = 0; i < NUM_ELEVATORS; i++) {
        elevator_t *elev = &sys->elevators[i];
        int         score;

        pthread_mutex_lock(&elev->mutex);

        /* Skip unavailable elevators */
        if (elev->state == STATE_EMERGENCY_STOP
            || elev->state == STATE_MAINTENANCE) {
            pthread_mutex_unlock(&elev->mutex);
            continue;
        }

        /* Base score = distance to target floor */
        score = abs(elev->current_floor - target_floor);

        /* Bias: prefer idle elevators (reduce their score) */
        if (elev->state == STATE_IDLE)
            score = (score > 0) ? score - 1 : 0;

        /* Penalty: moving away from target is less desirable */
        if ((elev->direction == DIR_UP   && target_floor < elev->current_floor)
         || (elev->direction == DIR_DOWN && target_floor > elev->current_floor)) {
            score += 2;
        }

        pthread_mutex_unlock(&elev->mutex);

        if (score < best_score) {
            best_score = score;
            best       = i;
        }
    }

    return best;
}

/* ---------------------------------------------------------------------------
 * request_manager_thread — Main loop: receive messages, dispatch, reply.
 * ---------------------------------------------------------------------------*/
void *request_manager_thread(void *arg)
{
    system_state_t   *sys = (system_state_t *)arg;
    elevator_msg_t    msg;
    elevator_reply_t  reply;
    int               rcvid;
    int               eid;   /* elevator id extracted from message */

    log_event(sys, "Request Manager: Thread started — channel %d",
              sys->channel_id);

    while (sys->running) {
        /* ── Block until a message arrives ─────────────────────────── */
        rcvid = MsgReceive(sys->channel_id, &msg, sizeof(msg), NULL);
        if (rcvid < 0) {
            /* Channel destroyed or error during shutdown */
            if (!sys->running) break;
            log_event(sys, "Request Manager: MsgReceive error (%d)", errno);
            continue;
        }

        /* Prepare default reply */
        reply.status            = 0;
        reply.assigned_elevator = -1;

        /* ── Dispatch based on message type ────────────────────────── */
        switch (msg.type) {

        /* ---- Floor request ---------------------------------------- */
        case MSG_FLOOR_REQUEST: {
            /* Validate floor number */
            if (msg.target_floor < 1 || msg.target_floor > NUM_FLOORS) {
                log_event(sys, "Request Manager: Invalid floor %d — ignored",
                          msg.target_floor);
                reply.status = -1;
                break;
            }

            /* Select the best elevator */
            int best = select_best_elevator(sys, msg.target_floor,
                                            msg.elevator_id);
            if (best < 0) {
                log_event(sys,
                    "Request Manager: No elevator available for floor %d",
                    msg.target_floor);
                reply.status = -1;
            } else {
                /* Assign the request and wake the elevator thread */
                elevator_t *elev = &sys->elevators[best];
                pthread_mutex_lock(&elev->mutex);
                if (!elev->floor_requests[msg.target_floor]) {
                    elev->floor_requests[msg.target_floor] = 1;
                    elev->request_count++;
                }
                pthread_cond_signal(&elev->cond);
                pthread_mutex_unlock(&elev->mutex);

                reply.assigned_elevator = best;
                log_event(sys,
                    "Request Manager: Floor %d → assigned to Elevator %d",
                    msg.target_floor, best + 1);
            }
            break;
        }

        /* ---- Emergency stop --------------------------------------- */
        case MSG_EMERGENCY_STOP: {
            eid = msg.elevator_id;
            if (eid < 0 || eid >= NUM_ELEVATORS) {
                reply.status = -1;
                break;
            }

            elevator_t *elev = &sys->elevators[eid];
            pthread_mutex_lock(&elev->mutex);
            elev->state       = STATE_EMERGENCY_STOP;
            elev->door_status = DOOR_CLOSED;
            elev->direction   = DIR_NONE;
            pthread_cond_signal(&elev->cond);
            pthread_mutex_unlock(&elev->mutex);

            log_event(sys,
                "Request Manager: EMERGENCY STOP activated — Elevator %d",
                eid + 1);
            break;
        }

        /* ---- Clear emergency -------------------------------------- */
        case MSG_EMERGENCY_CLEAR: {
            eid = msg.elevator_id;
            if (eid < 0 || eid >= NUM_ELEVATORS) {
                reply.status = -1;
                break;
            }

            elevator_t *elev = &sys->elevators[eid];
            pthread_mutex_lock(&elev->mutex);
            if (elev->state == STATE_EMERGENCY_STOP) {
                elev->state     = STATE_IDLE;
                elev->direction = DIR_NONE;
                pthread_cond_signal(&elev->cond);
                log_event(sys,
                    "Request Manager: Emergency cleared — Elevator %d",
                    eid + 1);
            }
            pthread_mutex_unlock(&elev->mutex);
            break;
        }

        /* ---- Maintenance on --------------------------------------- */
        case MSG_MAINTENANCE_ON: {
            eid = msg.elevator_id;
            if (eid < 0 || eid >= NUM_ELEVATORS) {
                reply.status = -1;
                break;
            }

            elevator_t *elev = &sys->elevators[eid];
            pthread_mutex_lock(&elev->mutex);
            elev->state       = STATE_MAINTENANCE;
            elev->door_status = DOOR_CLOSED;
            elev->direction   = DIR_NONE;
            pthread_cond_signal(&elev->cond);
            pthread_mutex_unlock(&elev->mutex);

            log_event(sys,
                "Request Manager: Elevator %d → MAINTENANCE mode ON",
                eid + 1);
            break;
        }

        /* ---- Maintenance off -------------------------------------- */
        case MSG_MAINTENANCE_OFF: {
            eid = msg.elevator_id;
            if (eid < 0 || eid >= NUM_ELEVATORS) {
                reply.status = -1;
                break;
            }

            elevator_t *elev = &sys->elevators[eid];
            pthread_mutex_lock(&elev->mutex);
            if (elev->state == STATE_MAINTENANCE) {
                elev->state     = STATE_IDLE;
                elev->direction = DIR_NONE;
                pthread_cond_signal(&elev->cond);
                log_event(sys,
                    "Request Manager: Elevator %d → MAINTENANCE mode OFF",
                    eid + 1);
            }
            pthread_mutex_unlock(&elev->mutex);
            break;
        }

        /* ---- Quit / shutdown -------------------------------------- */
        case MSG_QUIT: {
            log_event(sys,
                "Request Manager: QUIT received — initiating shutdown");
            sys->running = 0;

            /* Wake all elevator threads so they can exit */
            for (eid = 0; eid < NUM_ELEVATORS; eid++) {
                pthread_mutex_lock(&sys->elevators[eid].mutex);
                pthread_cond_broadcast(&sys->elevators[eid].cond);
                pthread_mutex_unlock(&sys->elevators[eid].mutex);
            }

            /* Reply immediately, then exit this thread */
            MsgReply(rcvid, 0, &reply, sizeof(reply));
            goto thread_exit;
        }

        default:
            log_event(sys, "Request Manager: Unknown message type %d",
                      msg.type);
            reply.status = -1;
            break;
        }

        /* ── Reply to unblock the sender ───────────────────────────── */
        MsgReply(rcvid, 0, &reply, sizeof(reply));
    }

thread_exit:
    log_event(sys, "Request Manager: Thread exiting");
    return NULL;
}
