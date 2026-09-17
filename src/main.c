/*
 * =============================================================================
 *  main.c — Entry Point & Input Handler
 *
 *  Responsibilities:
 *    1. Initialise all shared state (elevators, logger, QNX channel)
 *    2. Spawn five POSIX threads:
 *         - Request Manager   (receives IPC messages, dispatches)
 *         - Elevator 1        (state machine)
 *         - Elevator 2        (state machine)
 *         - Display           (terminal dashboard)
 *         - Input Handler     (reads user commands, sends via MsgSend)
 *    3. Wait for all threads to finish, then clean up
 *
 *  On non-QNX systems, a POSIX compatibility layer (at the bottom of this
 *  file) simulates QNX synchronous message passing using mutexes and
 *  condition variables.
 * =============================================================================
 */

#include "../include/elevator.h"

/* ── Global system state ───────────────────────────────────────────────────── */
static system_state_t g_sys;

/* ============================================================================
 * Input Handler Thread
 *
 * Reads single-character commands from stdin, packs them into
 * elevator_msg_t structures, and sends them to the request manager
 * via ConnectAttach / MsgSend (QNX IPC or simulated).
 *
 * Commands:
 *   f<N>  — Request floor N (1–8)
 *   e<N>  — Emergency stop on elevator N (1–2)
 *   c<N>  — Clear emergency on elevator N
 *   m<N>  — Enable maintenance on elevator N
 *   r<N>  — Disable maintenance on elevator N
 *   q     — Quit the system
 * ============================================================================*/
static void *input_thread(void *arg)
{
    system_state_t   *sys = (system_state_t *)arg;
    elevator_msg_t    msg;
    elevator_reply_t  reply;
    int               coid;
    char              line[64];

    /* Connect to the request manager's message channel */
    coid = ConnectAttach(ND_LOCAL_NODE, 0, sys->channel_id, 0, 0);
    if (coid < 0) {
        log_event(sys, "Input: FATAL — ConnectAttach failed (errno=%d)", errno);
        sys->running = 0;
        return NULL;
    }

    log_event(sys, "Input: Thread started — connection id=%d", coid);

    /*
     * Position the cursor below the dashboard for user input.
     * DASHBOARD_LINES + 2 gives a blank line below the dashboard.
     */
    printf("\033[%d;1H", DASHBOARD_LINES + 2);
    fflush(stdout);

    while (sys->running) {
        /* Move cursor to input line and show prompt */
        printf("\033[%d;1H\033[K> ", DASHBOARD_LINES + 2);
        fflush(stdout);

        /* Blocking read from stdin */
        if (fgets(line, sizeof(line), stdin) == NULL) {
            /*
             * EOF or error (e.g. Ctrl+D, redirected input ended).
             * Send MSG_QUIT to unblock the request manager and trigger
             * clean shutdown — prevents deadlock in main() on join.
             */
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_QUIT;
            MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply));
            break;
        }

        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0')
            continue;

        /* ── Parse the command ─────────────────────────────────────── */
        memset(&msg, 0, sizeof(msg));
        memset(&reply, 0, sizeof(reply));

        char cmd = line[0];
        int  num = 0;

        if (line[1] != '\0')
            num = atoi(&line[1]);

        switch (cmd) {
        case 'f': case 'F':
            if (num < 1 || num > NUM_FLOORS) {
                printf("\033[%d;1H\033[K  [!] Invalid floor (use 1-%d)\n",
                       DASHBOARD_LINES + 3, NUM_FLOORS);
                fflush(stdout);
                sleep_ms(1000);
                continue;
            }
            msg.type         = MSG_FLOOR_REQUEST;
            msg.target_floor = num;
            msg.elevator_id  = -1;   /* Auto-dispatch */
            break;

        case 'e': case 'E':
            if (num < 1 || num > NUM_ELEVATORS) {
                printf("\033[%d;1H\033[K  [!] Invalid elevator (use 1-%d)\n",
                       DASHBOARD_LINES + 3, NUM_ELEVATORS);
                fflush(stdout);
                sleep_ms(1000);
                continue;
            }
            msg.type        = MSG_EMERGENCY_STOP;
            msg.elevator_id = num - 1;   /* Convert to 0-based */
            break;

        case 'c': case 'C':
            if (num < 1 || num > NUM_ELEVATORS) {
                printf("\033[%d;1H\033[K  [!] Invalid elevator (use 1-%d)\n",
                       DASHBOARD_LINES + 3, NUM_ELEVATORS);
                fflush(stdout);
                sleep_ms(1000);
                continue;
            }
            msg.type        = MSG_EMERGENCY_CLEAR;
            msg.elevator_id = num - 1;
            break;

        case 'm': case 'M':
            if (num < 1 || num > NUM_ELEVATORS) {
                printf("\033[%d;1H\033[K  [!] Invalid elevator (use 1-%d)\n",
                       DASHBOARD_LINES + 3, NUM_ELEVATORS);
                fflush(stdout);
                sleep_ms(1000);
                continue;
            }
            msg.type        = MSG_MAINTENANCE_ON;
            msg.elevator_id = num - 1;
            break;

        case 'r': case 'R':
            if (num < 1 || num > NUM_ELEVATORS) {
                printf("\033[%d;1H\033[K  [!] Invalid elevator (use 1-%d)\n",
                       DASHBOARD_LINES + 3, NUM_ELEVATORS);
                fflush(stdout);
                sleep_ms(1000);
                continue;
            }
            msg.type        = MSG_MAINTENANCE_OFF;
            msg.elevator_id = num - 1;
            break;

        case 'q': case 'Q':
            msg.type = MSG_QUIT;
            break;

        default:
            printf("\033[%d;1H\033[K  [!] Unknown command '%c' — type q for quit\n",
                   DASHBOARD_LINES + 3, cmd);
            fflush(stdout);
            sleep_ms(1000);
            continue;
        }

        /* ── Send the message via QNX IPC (MsgSend) ───────────────── */
        if (MsgSend(coid, &msg, sizeof(msg), &reply, sizeof(reply)) < 0) {
            log_event(sys, "Input: MsgSend failed (errno=%d)", errno);
        }

        /* Clear any status message from the line below */
        printf("\033[%d;1H\033[K", DASHBOARD_LINES + 3);
        fflush(stdout);

        /* If quit was sent, stop reading input */
        if (msg.type == MSG_QUIT) {
            break;
        }
    }

    ConnectDetach(coid);
    log_event(sys, "Input: Thread exiting");
    return NULL;
}

/* ============================================================================
 * main — System initialisation, thread creation, and shutdown.
 * ============================================================================*/
int main(void)
{
    pthread_t               tid_reqmgr;
    pthread_t               tid_elev[NUM_ELEVATORS];
    pthread_t               tid_display;
    pthread_t               tid_input;
    elevator_thread_arg_t   elev_args[NUM_ELEVATORS];
    int                     i;

    /* ── Initialise system state ───────────────────────────────────── */
    memset(&g_sys, 0, sizeof(g_sys));
    g_sys.running = 1;

    /* Open the event log */
    logger_init(&g_sys, "elevator_log.txt");
    log_event(&g_sys, "System: Initialising — %d elevators, %d floors",
              NUM_ELEVATORS, NUM_FLOORS);

    /* Initialise each elevator to floor 1, idle */
    for (i = 0; i < NUM_ELEVATORS; i++) {
        elevator_init(&g_sys.elevators[i], i);
    }

    /* ── Create the QNX message-passing channel ────────────────────── */
    g_sys.channel_id = ChannelCreate(0);
    if (g_sys.channel_id < 0) {
        log_event(&g_sys, "FATAL: ChannelCreate failed (errno=%d)", errno);
        fprintf(stderr, "FATAL: Could not create message channel.\n");
        logger_close(&g_sys);
        return 1;
    }
    log_event(&g_sys, "System: Message channel created (id=%d)",
              g_sys.channel_id);

    /* ── Spawn threads ─────────────────────────────────────────────── */

    /* Request manager (must start first — it calls MsgReceive) */
    if (pthread_create(&tid_reqmgr, NULL, request_manager_thread, &g_sys) != 0) {
        perror("pthread_create: request_manager");
        goto cleanup_channel;
    }

    /* Elevator car threads */
    for (i = 0; i < NUM_ELEVATORS; i++) {
        elev_args[i].sys         = &g_sys;
        elev_args[i].elevator_id = i;
        if (pthread_create(&tid_elev[i], NULL, elevator_thread, &elev_args[i]) != 0) {
            perror("pthread_create: elevator");
            g_sys.running = 0;
            goto cleanup_channel;
        }
    }

    /* Display (dashboard renderer) */
    if (pthread_create(&tid_display, NULL, display_thread, &g_sys) != 0) {
        perror("pthread_create: display");
        g_sys.running = 0;
        goto cleanup_channel;
    }

    /* Input handler (user command reader) */
    if (pthread_create(&tid_input, NULL, input_thread, &g_sys) != 0) {
        perror("pthread_create: input");
        g_sys.running = 0;
        goto cleanup_channel;
    }

    log_event(&g_sys, "System: All threads started — ready for commands");

    /* ── Wait for input thread to finish (user entered 'q') ────────── */
    pthread_join(tid_input, NULL);

    /* ── Wait for request manager to finish ────────────────────────── */
    pthread_join(tid_reqmgr, NULL);

    /* ── Ensure all elevator threads can exit ──────────────────────── */
    g_sys.running = 0;
    for (i = 0; i < NUM_ELEVATORS; i++) {
        pthread_mutex_lock(&g_sys.elevators[i].mutex);
        pthread_cond_broadcast(&g_sys.elevators[i].cond);
        pthread_mutex_unlock(&g_sys.elevators[i].mutex);
    }

    for (i = 0; i < NUM_ELEVATORS; i++) {
        pthread_join(tid_elev[i], NULL);
    }

    /* Wait for display to finish its last render */
    pthread_join(tid_display, NULL);

    /* ── Cleanup ───────────────────────────────────────────────────── */
cleanup_channel:
    ChannelDestroy(g_sys.channel_id);
    log_event(&g_sys, "System: All threads joined — shutting down");
    logger_close(&g_sys);

    for (i = 0; i < NUM_ELEVATORS; i++) {
        pthread_mutex_destroy(&g_sys.elevators[i].mutex);
        pthread_cond_destroy(&g_sys.elevators[i].cond);
    }

    printf("\nElevator Control System terminated cleanly.\n");
    return 0;
}

/* ============================================================================
 * QNX COMPATIBILITY LAYER (non-QNX systems only)
 *
 * Simulates QNX-style synchronous message passing using POSIX primitives.
 * A global sim_channel_t provides one channel with:
 *   - A message buffer   (sender → receiver)
 *   - A reply buffer     (receiver → sender)
 *   - A send_mutex       to serialise concurrent senders
 *   - A ch_mutex + condvars for handshake between send and receive sides
 *
 * The semantics match QNX:
 *   - MsgSend blocks until MsgReply
 *   - MsgReceive blocks until MsgSend
 *   - MsgReply unblocks the corresponding MsgSend
 * ============================================================================*/

#ifndef __QNX__

sim_channel_t g_sim_channel;   /* Global simulated channel */

int sim_ChannelCreate(int flags)
{
    (void)flags;
    memset(&g_sim_channel, 0, sizeof(g_sim_channel));
    pthread_mutex_init(&g_sim_channel.ch_mutex, NULL);
    pthread_mutex_init(&g_sim_channel.send_mutex, NULL);
    pthread_cond_init(&g_sim_channel.msg_cond, NULL);
    pthread_cond_init(&g_sim_channel.reply_cond, NULL);
    return 1;  /* Simulated channel ID */
}

int sim_ChannelDestroy(int chid)
{
    (void)chid;

    /* Wake any thread blocked in MsgReceive or MsgSend before destroying */
    pthread_mutex_lock(&g_sim_channel.ch_mutex);
    g_sim_channel.msg_ready   = 1;  /* Unblock MsgReceive */
    g_sim_channel.reply_ready = 1;  /* Unblock MsgSend    */
    pthread_cond_broadcast(&g_sim_channel.msg_cond);
    pthread_cond_broadcast(&g_sim_channel.reply_cond);
    pthread_mutex_unlock(&g_sim_channel.ch_mutex);

    pthread_mutex_destroy(&g_sim_channel.ch_mutex);
    pthread_mutex_destroy(&g_sim_channel.send_mutex);
    pthread_cond_destroy(&g_sim_channel.msg_cond);
    pthread_cond_destroy(&g_sim_channel.reply_cond);
    return 0;
}

int sim_ConnectAttach(int nd, pid_t pid, int chid, int index, int flags)
{
    (void)nd; (void)pid; (void)chid; (void)index; (void)flags;
    return 1;  /* Simulated connection ID */
}

int sim_ConnectDetach(int coid)
{
    (void)coid;
    return 0;
}

/*
 * sim_MsgSend — Synchronous send: post a message and block until the
 * receiver calls MsgReply.
 *
 * Flow:
 *   1. Acquire send_mutex (serialise senders)
 *   2. Lock ch_mutex
 *   3. Copy message into buffer, set msg_ready, signal receiver
 *   4. Wait on reply_cond until reply_ready
 *   5. Copy reply, clear reply_ready
 *   6. Unlock ch_mutex, release send_mutex
 */
int sim_MsgSend(int coid, const void *smsg, int sbytes,
                void *rmsg, int rbytes)
{
    (void)coid;

    /* Only one sender at a time */
    pthread_mutex_lock(&g_sim_channel.send_mutex);
    pthread_mutex_lock(&g_sim_channel.ch_mutex);

    /* Post the message */
    memcpy(&g_sim_channel.msg_buffer, smsg,
           (size_t)sbytes < sizeof(elevator_msg_t)
               ? (size_t)sbytes : sizeof(elevator_msg_t));
    g_sim_channel.msg_ready   = 1;
    g_sim_channel.reply_ready = 0;
    pthread_cond_signal(&g_sim_channel.msg_cond);

    /* Wait for the receiver to reply */
    while (!g_sim_channel.reply_ready) {
        pthread_cond_wait(&g_sim_channel.reply_cond, &g_sim_channel.ch_mutex);
    }

    /* Collect reply */
    if (rmsg && rbytes > 0) {
        memcpy(rmsg, &g_sim_channel.reply_buffer,
               (size_t)rbytes < sizeof(elevator_reply_t)
                   ? (size_t)rbytes : sizeof(elevator_reply_t));
    }
    g_sim_channel.reply_ready = 0;

    pthread_mutex_unlock(&g_sim_channel.ch_mutex);
    pthread_mutex_unlock(&g_sim_channel.send_mutex);

    return 0;
}

/*
 * sim_MsgReceive — Block until a message is available, then copy it
 * and return a pseudo receive-ID.
 *
 * Flow:
 *   1. Lock ch_mutex
 *   2. Wait on msg_cond until msg_ready
 *   3. Copy message, clear msg_ready
 *   4. Unlock ch_mutex
 *   5. Return rcvid = 1
 */
int sim_MsgReceive(int chid, void *msg, int bytes, void *info)
{
    (void)chid; (void)info;

    pthread_mutex_lock(&g_sim_channel.ch_mutex);

    while (!g_sim_channel.msg_ready) {
        pthread_cond_wait(&g_sim_channel.msg_cond, &g_sim_channel.ch_mutex);
    }

    memcpy(msg, &g_sim_channel.msg_buffer,
           (size_t)bytes < sizeof(elevator_msg_t)
               ? (size_t)bytes : sizeof(elevator_msg_t));
    g_sim_channel.msg_ready = 0;

    pthread_mutex_unlock(&g_sim_channel.ch_mutex);

    return 1;  /* Pseudo rcvid */
}

/*
 * sim_MsgReply — Post a reply to unblock the waiting sender.
 *
 * Flow:
 *   1. Lock ch_mutex
 *   2. Copy reply into buffer, set reply_ready
 *   3. Signal reply_cond
 *   4. Unlock ch_mutex
 */
int sim_MsgReply(int rcvid, int status, const void *msg, int bytes)
{
    (void)rcvid; (void)status;

    pthread_mutex_lock(&g_sim_channel.ch_mutex);

    if (msg && bytes > 0) {
        memcpy(&g_sim_channel.reply_buffer, msg,
               (size_t)bytes < sizeof(elevator_reply_t)
                   ? (size_t)bytes : sizeof(elevator_reply_t));
    }
    g_sim_channel.reply_ready = 1;
    pthread_cond_signal(&g_sim_channel.reply_cond);

    pthread_mutex_unlock(&g_sim_channel.ch_mutex);

    return 0;
}

#endif /* !__QNX__ */
