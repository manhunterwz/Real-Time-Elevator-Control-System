/*
 * =============================================================================
 *  elevator.h — Real-Time Elevator Control System
 *
 *  Central header for the QNX Neutrino RTOS elevator simulator.
 *  Defines all shared types, constants, and function declarations.
 *  Includes a POSIX compatibility layer so the project can be compiled
 *  and tested on Linux/macOS when QNX SDP is not available.
 * =============================================================================
 */

#ifndef ELEVATOR_H
#define ELEVATOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>

#ifdef __QNX__
#include <sys/neutrino.h>   /* ChannelCreate, MsgSend, MsgReceive, etc. */
#include <sys/netmgr.h>     /* ND_LOCAL_NODE                            */
#endif

/* ============================================================================
 * Constants
 * ============================================================================*/

#define NUM_ELEVATORS        2       /* Number of elevator cars               */
#define NUM_FLOORS           8       /* Floors in the building (1..8)         */
#define FLOOR_TRAVEL_TIME_MS 2000    /* Milliseconds to travel one floor      */
#define DOOR_OPEN_TIME_MS    3000    /* Milliseconds doors stay open          */
#define DISPLAY_REFRESH_MS   500     /* Dashboard refresh interval            */
#define MAX_LOG_MSG          512     /* Maximum log message length            */
#define DASHBOARD_LINES      23     /* Total lines in the dashboard          */

/* ============================================================================
 * Enumerations
 * ============================================================================*/

/* Elevator operational states */
typedef enum {
    STATE_IDLE,              /* Stationary, no pending requests    */
    STATE_MOVING_UP,         /* Travelling upward                  */
    STATE_MOVING_DOWN,       /* Travelling downward                */
    STATE_DOOR_OPEN,         /* Stopped, doors are open            */
    STATE_EMERGENCY_STOP,    /* Emergency halt — all ops suspended */
    STATE_MAINTENANCE        /* Maintenance mode — no new requests */
} elevator_state_t;

/* Door position */
typedef enum {
    DOOR_CLOSED,
    DOOR_OPEN
} door_status_t;

/* Travel direction */
typedef enum {
    DIR_NONE,
    DIR_UP,
    DIR_DOWN
} direction_t;

/* QNX IPC message types */
typedef enum {
    MSG_FLOOR_REQUEST,       /* Request an elevator to visit a floor  */
    MSG_EMERGENCY_STOP,      /* Trigger emergency stop                */
    MSG_EMERGENCY_CLEAR,     /* Clear emergency stop                  */
    MSG_MAINTENANCE_ON,      /* Enter maintenance mode                */
    MSG_MAINTENANCE_OFF,     /* Exit maintenance mode                 */
    MSG_QUIT                 /* Shut down the entire system           */
} msg_type_t;

/* ============================================================================
 * Message Structures — used with MsgSend / MsgReceive
 * ============================================================================*/

/* Outgoing request from input thread → request manager */
typedef struct {
    msg_type_t type;           /* What kind of request                   */
    int        target_floor;   /* 1-based floor (used by FLOOR_REQUEST)  */
    int        elevator_id;    /* 0 or 1; -1 means auto-dispatch        */
} elevator_msg_t;

/* Reply from request manager → input thread */
typedef struct {
    int status;                /* 0 = success, -1 = error               */
    int assigned_elevator;     /* Which elevator was assigned (0 or 1)  */
} elevator_reply_t;

/* ============================================================================
 * Per-Elevator State
 * ============================================================================*/

typedef struct {
    int              id;                               /* Elevator index 0..1     */
    int              current_floor;                    /* Current floor 1..8      */
    elevator_state_t state;                            /* Operational state       */
    direction_t      direction;                        /* Travel direction        */
    door_status_t    door_status;                      /* Door open / closed      */
    int              floor_requests[NUM_FLOORS + 1];   /* Pending reqs [1..8]     */
    int              request_count;                    /* Cached count of reqs    */
    pthread_mutex_t  mutex;                            /* Protects this struct    */
    pthread_cond_t   cond;                             /* Signals new work        */
} elevator_t;

/* ============================================================================
 * System-Wide Shared State
 * ============================================================================*/

typedef struct {
    elevator_t      elevators[NUM_ELEVATORS];
    int             channel_id;      /* QNX message-passing channel ID */
    volatile int    running;         /* 1 while system is live         */
    FILE           *log_file;        /* Event log output               */
    pthread_mutex_t log_mutex;       /* Serializes log writes          */
} system_state_t;

/* ============================================================================
 * Thread Argument — passes system pointer + elevator index
 * ============================================================================*/

typedef struct {
    system_state_t *sys;
    int             elevator_id;
} elevator_thread_arg_t;

/* ============================================================================
 * Function Declarations
 * ============================================================================*/

/* logger.c */
void logger_init(system_state_t *sys, const char *filename);
void logger_close(system_state_t *sys);
void log_event(system_state_t *sys, const char *fmt, ...);

/* elevator.c */
void  elevator_init(elevator_t *elev, int id);
void *elevator_thread(void *arg);

/* request_manager.c */
void *request_manager_thread(void *arg);

/* display.c */
void *display_thread(void *arg);

/* ============================================================================
 * Inline Utilities
 * ============================================================================*/

/* Sleep for the given number of milliseconds (POSIX nanosleep wrapper). */
static inline void sleep_ms(int ms)
{
    struct timespec ts;
    if (ms <= 0) return;   /* Guard against negative or zero values */
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Human-readable elevator state name. */
static inline const char *state_name(elevator_state_t s)
{
    switch (s) {
        case STATE_IDLE:           return "IDLE";
        case STATE_MOVING_UP:      return "MOVING UP";
        case STATE_MOVING_DOWN:    return "MOVING DOWN";
        case STATE_DOOR_OPEN:      return "DOOR OPEN";
        case STATE_EMERGENCY_STOP: return "EMERGENCY";
        case STATE_MAINTENANCE:    return "MAINTENANCE";
        default:                   return "UNKNOWN";
    }
}

/* Direction indicator arrow (ASCII). */
static inline const char *direction_arrow(direction_t d)
{
    switch (d) {
        case DIR_UP:   return "^";
        case DIR_DOWN: return "v";
        default:       return "-";
    }
}

/* Door status string. */
static inline const char *door_name(door_status_t d)
{
    switch (d) {
        case DOOR_OPEN:   return "OPEN";
        case DOOR_CLOSED: return "CLOSED";
        default:          return "UNKNOWN";
    }
}

/* ============================================================================
 * QNX Compatibility Layer (non-QNX builds only)
 *
 * Simulates QNX synchronous message passing using POSIX mutexes and
 * condition variables.  A single global channel is provided, with
 * a send-side mutex to serialize concurrent senders.
 *
 * On QNX, the native kernel APIs are used directly.
 * ============================================================================*/

#ifndef __QNX__

/* Simulated channel structure */
typedef struct {
    elevator_msg_t   msg_buffer;     /* Holds the in-flight message      */
    elevator_reply_t reply_buffer;   /* Holds the reply                  */
    int              msg_ready;      /* 1 when a message is waiting      */
    int              reply_ready;    /* 1 when a reply is waiting        */
    pthread_mutex_t  ch_mutex;       /* Protects buffer + flags          */
    pthread_mutex_t  send_mutex;     /* Serializes concurrent senders    */
    pthread_cond_t   msg_cond;       /* Wakes receiver on new message    */
    pthread_cond_t   reply_cond;     /* Wakes sender on reply            */
} sim_channel_t;

/* Global simulated channel — defined in main.c */
extern sim_channel_t g_sim_channel;

/* Simulated API functions — implemented in main.c */
int sim_ChannelCreate(int flags);
int sim_ChannelDestroy(int chid);
int sim_ConnectAttach(int nd, pid_t pid, int chid, int index, int flags);
int sim_ConnectDetach(int coid);
int sim_MsgSend(int coid, const void *smsg, int sbytes, void *rmsg, int rbytes);
int sim_MsgReceive(int chid, void *msg, int bytes, void *info);
int sim_MsgReply(int rcvid, int status, const void *msg, int bytes);

/* Macro aliases so application code uses QNX-style names everywhere */
#define ChannelCreate(flags)              sim_ChannelCreate(flags)
#define ChannelDestroy(chid)              sim_ChannelDestroy(chid)
#define ConnectAttach(nd, pid, chid, i, f) sim_ConnectAttach(nd, pid, chid, i, f)
#define ConnectDetach(coid)               sim_ConnectDetach(coid)
#define MsgSend(coid, sm, sb, rm, rb)     sim_MsgSend(coid, sm, sb, rm, rb)
#define MsgReceive(chid, m, b, info)      sim_MsgReceive(chid, m, b, info)
#define MsgReply(rcvid, st, m, b)         sim_MsgReply(rcvid, st, m, b)

/* ND_LOCAL_NODE is QNX-specific; provide a no-op value */
#ifndef ND_LOCAL_NODE
#define ND_LOCAL_NODE 0
#endif

#endif /* !__QNX__ */

#endif /* ELEVATOR_H */
