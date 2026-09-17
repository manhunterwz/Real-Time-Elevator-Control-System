/*
 * =============================================================================
 *  display.c — Live Terminal Dashboard
 *
 *  Renders a real-time ANSI terminal dashboard showing:
 *    - Per-elevator state, floor position, direction, door status
 *    - Visual floor-by-floor indicator with request markers
 *    - Command help reference
 *
 *  The dashboard refreshes every DISPLAY_REFRESH_MS (500 ms).
 *  Uses ANSI escape codes for cursor positioning, color, and clearing.
 *
 *  Thread safety:
 *    - Snapshots each elevator's state under its mutex
 *    - All stdout output is done in a single burst per refresh
 *    - Cursor save/restore avoids disturbing the input prompt line
 * =============================================================================
 */

#include "../include/elevator.h"

/* ── ANSI Escape Code Definitions ──────────────────────────────────────────── */

#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_DIM        "\033[2m"

#define ANSI_RED        "\033[31m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BLUE       "\033[34m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_WHITE      "\033[37m"

#define ANSI_BG_RED     "\033[41m"
#define ANSI_BG_GREEN   "\033[42m"
#define ANSI_BG_BLUE    "\033[44m"

/* Cursor control */
#define CURSOR_HOME     "\033[H"       /* Move to row 1, col 1          */
#define CURSOR_SAVE     "\033[s"       /* Save cursor position          */
#define CURSOR_RESTORE  "\033[u"       /* Restore saved cursor position */
#define CURSOR_HIDE     "\033[?25l"    /* Hide cursor (reduces flicker) */
#define CURSOR_SHOW     "\033[?25h"    /* Show cursor                   */
#define CLEAR_LINE      "\033[K"       /* Erase from cursor to end      */
#define CLEAR_SCREEN    "\033[2J"      /* Clear entire screen           */

/* ── Dashboard dimensions ──────────────────────────────────────────────────── */
/* Total visible width of the dashboard box = 64 characters (including borders) */
/* Each elevator column = 30 chars visible (col1) + 1 border + 31 chars (col2) + 2 outer borders = 64 */
#define BOX_WIDTH       64
#define COL1_INNER      29    /* Visible chars inside column 1 (between | and |) */
#define COL2_INNER      30    /* Visible chars inside column 2 (between | and |) */

/* ---------------------------------------------------------------------------
 * state_color — Return the ANSI color code for a given elevator state.
 * ---------------------------------------------------------------------------*/
static const char *state_color(elevator_state_t s)
{
    switch (s) {
        case STATE_IDLE:           return ANSI_GREEN;
        case STATE_MOVING_UP:      return ANSI_YELLOW;
        case STATE_MOVING_DOWN:    return ANSI_YELLOW;
        case STATE_DOOR_OPEN:      return ANSI_CYAN;
        case STATE_EMERGENCY_STOP: return ANSI_RED;
        case STATE_MAINTENANCE:    return ANSI_BLUE;
        default:                   return ANSI_RESET;
    }
}

/* ---------------------------------------------------------------------------
 * print_padded — Print text and pad with spaces to reach target visible width.
 *
 * 'visible_so_far' is how many visible characters have been printed in the
 * current segment. This function prints (target_width - visible_so_far) spaces.
 * ---------------------------------------------------------------------------*/
static void print_padded(int visible_so_far, int target_width)
{
    int i;
    for (i = visible_so_far; i < target_width; i++)
        putchar(' ');
}

/* ---------------------------------------------------------------------------
 * draw_dashboard — Render the complete dashboard to stdout.
 *
 * Layout (23 lines total):
 *   Line  1     : top border
 *   Line  2     : title
 *   Line  3     : timestamp
 *   Line  4     : separator
 *   Line  5     : elevator headers
 *   Line  6     : states
 *   Line  7     : floor numbers
 *   Line  8     : door status
 *   Line  9     : blank divider
 *   Lines 10-17 : floor display (8 -> 1)
 *   Line  18    : separator
 *   Line  19-21 : command help
 *   Line  22    : bottom border
 *   Line  23    : pending request counts
 * ---------------------------------------------------------------------------*/
static void draw_dashboard(system_state_t *sys)
{
    int e, f, vis;

    /* ── Snapshot elevator states (lock, copy, unlock) ─────────────── */
    int              cur[NUM_ELEVATORS];
    elevator_state_t states[NUM_ELEVATORS];
    direction_t      dirs[NUM_ELEVATORS];
    door_status_t    doors[NUM_ELEVATORS];
    int              reqs[NUM_ELEVATORS][NUM_FLOORS + 1];
    int              rcnt[NUM_ELEVATORS];

    for (e = 0; e < NUM_ELEVATORS; e++) {
        pthread_mutex_lock(&sys->elevators[e].mutex);
        cur[e]    = sys->elevators[e].current_floor;
        states[e] = sys->elevators[e].state;
        dirs[e]   = sys->elevators[e].direction;
        doors[e]  = sys->elevators[e].door_status;
        rcnt[e]   = sys->elevators[e].request_count;
        memcpy(reqs[e], sys->elevators[e].floor_requests, sizeof(reqs[e]));
        pthread_mutex_unlock(&sys->elevators[e].mutex);
    }

    /* ── Timestamp ─────────────────────────────────────────────────── */
    struct timespec ts;
    struct tm       tm_info;
    clock_gettime(CLOCK_REALTIME, &ts);
#ifdef _WIN32
    localtime_s(&tm_info, &ts.tv_sec);
#else
    localtime_r(&ts.tv_sec, &tm_info);
#endif

    /* ── Begin rendering ───────────────────────────────────────────── */
    /* Each line uses CLEAR_LINE at the end to erase leftover characters */

    /* Line 1: top border (62 dashes + 2 corners = 64 chars) */
    printf("+--------------------------------------------------------------+" CLEAR_LINE "\n");

    /* Line 2: title — fixed string, manually measured to 62 visible chars */
    printf("|  %sREAL-TIME ELEVATOR CONTROL SYSTEM%s - QNX Neutrino RTOS     |" CLEAR_LINE "\n",
           ANSI_BOLD, ANSI_RESET);

    /* Line 3: timestamp */
    /* "|  Time: HH:MM:SS" = 2+7+8 = 17 visible chars */
    printf("|  Time: %02d:%02d:%02d",
           tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    print_padded(17, BOX_WIDTH - 2);  /* pad to 62 visible inner chars */
    printf("|" CLEAR_LINE "\n");

    /* Line 4: separator with column split */
    printf("+-----------------------------+--------------------------------+" CLEAR_LINE "\n");

    /* Line 5: elevator headers */
    /* Column 1: "|  ELEVATOR 1" = 1 border + "  ELEVATOR 1" = 12 visible */
    printf("|  %sELEVATOR 1%s", ANSI_BOLD, ANSI_RESET);
    print_padded(12, COL1_INNER);
    /* Column 2: "|  ELEVATOR 2" = 12 visible */
    printf("|  %sELEVATOR 2%s", ANSI_BOLD, ANSI_RESET);
    print_padded(12, COL2_INNER);
    printf("|" CLEAR_LINE "\n");

    /* Line 6: states with color */
    {
        const char *s0 = state_name(states[0]);
        const char *a0 = direction_arrow(dirs[0]);
        const char *s1 = state_name(states[1]);
        const char *a1 = direction_arrow(dirs[1]);

        /* Col 1: "|  State: <state> <arrow>" */
        /* "  State: " = 9 chars, state name variable, " " = 1, arrow = 1 */
        printf("|  State: %s%s%s %s", state_color(states[0]), s0, ANSI_RESET, a0);
        vis = 9 + (int)strlen(s0) + 1 + (int)strlen(a0);
        print_padded(vis, COL1_INNER);

        /* Col 2 */
        printf("|  State: %s%s%s %s", state_color(states[1]), s1, ANSI_RESET, a1);
        vis = 9 + (int)strlen(s1) + 1 + (int)strlen(a1);
        print_padded(vis, COL2_INNER);
        printf("|" CLEAR_LINE "\n");
    }

    /* Line 7: current floor */
    /* "|  Floor: N" = 10 visible for single digit, 11 for double */
    printf("|  Floor: %d", cur[0]);
    vis = (cur[0] >= 10) ? 11 : 10;
    print_padded(vis, COL1_INNER);

    printf("|  Floor: %d", cur[1]);
    vis = (cur[1] >= 10) ? 11 : 10;
    print_padded(vis, COL2_INNER);
    printf("|" CLEAR_LINE "\n");

    /* Line 8: door status */
    {
        const char *d0 = door_name(doors[0]);
        const char *d1 = door_name(doors[1]);

        /* "|  Door:  <status>" = 9 + strlen(status) visible chars */
        if (doors[0] == DOOR_OPEN)
            printf("|  Door:  %s%s%s", ANSI_CYAN, d0, ANSI_RESET);
        else
            printf("|  Door:  %s", d0);
        vis = 9 + (int)strlen(d0);
        print_padded(vis, COL1_INNER);

        if (doors[1] == DOOR_OPEN)
            printf("|  Door:  %s%s%s", ANSI_CYAN, d1, ANSI_RESET);
        else
            printf("|  Door:  %s", d1);
        vis = 9 + (int)strlen(d1);
        print_padded(vis, COL2_INNER);
        printf("|" CLEAR_LINE "\n");
    }

    /* Line 9: blank separator */
    printf("|");
    print_padded(0, COL1_INNER);
    printf("|");
    print_padded(0, COL2_INNER);
    printf("|" CLEAR_LINE "\n");

    /* Lines 10-17: floor display (8 down to 1) */
    for (f = NUM_FLOORS; f >= 1; f--) {
        /* ── Elevator 1 column ─── */
        /* "|  N " = 4 visible chars */
        printf("|  %d ", f);
        if (f == cur[0] && reqs[0][f]) {
            /* At this floor AND has a request */
            printf("%s[*R]%s", ANSI_YELLOW, ANSI_RESET);
            vis = 4 + 4;  /* "  N " + "[*R]" */
            print_padded(vis, COL1_INNER);
        } else if (f == cur[0]) {
            /* Elevator is here */
            printf("%s[**]%s << HERE", ANSI_GREEN, ANSI_RESET);
            vis = 4 + 4 + 8;  /* "  N " + "[**]" + " << HERE" */
            print_padded(vis, COL1_INNER);
        } else if (reqs[0][f]) {
            /* Pending request at this floor */
            printf("%s[ R]%s", ANSI_CYAN, ANSI_RESET);
            vis = 4 + 4;
            print_padded(vis, COL1_INNER);
        } else {
            /* Empty floor */
            printf("[ . ]");
            vis = 4 + 5;
            print_padded(vis, COL1_INNER);
        }

        /* ── Elevator 2 column ─── */
        printf("|  %d ", f);
        if (f == cur[1] && reqs[1][f]) {
            printf("%s[*R]%s", ANSI_YELLOW, ANSI_RESET);
            vis = 4 + 4;
            print_padded(vis, COL2_INNER);
        } else if (f == cur[1]) {
            printf("%s[**]%s << HERE", ANSI_GREEN, ANSI_RESET);
            vis = 4 + 4 + 8;
            print_padded(vis, COL2_INNER);
        } else if (reqs[1][f]) {
            printf("%s[ R]%s", ANSI_CYAN, ANSI_RESET);
            vis = 4 + 4;
            print_padded(vis, COL2_INNER);
        } else {
            printf("[ . ]");
            vis = 4 + 5;
            print_padded(vis, COL2_INNER);
        }

        printf("|" CLEAR_LINE "\n");
    }

    /* Line 18: separator */
    printf("+--------------------------------------------------------------+" CLEAR_LINE "\n");

    /* Lines 19-21: command help */
    printf("|  %sCOMMANDS:%s  f<N> = floor   e<N> = emergency stop          |" CLEAR_LINE "\n",
           ANSI_BOLD, ANSI_RESET);
    printf("|            c<N> = clear     m<N> = maintenance on           |" CLEAR_LINE "\n");
    printf("|            r<N> = resume    q    = quit system              |" CLEAR_LINE "\n");

    /* Line 22: bottom border */
    printf("+--------------------------------------------------------------+" CLEAR_LINE "\n");

    /* Line 23: request counts summary (outside the box) */
    printf("  Pending: Elev1=%d  Elev2=%d" CLEAR_LINE "\n",
           rcnt[0], rcnt[1]);

    fflush(stdout);
}

/* ---------------------------------------------------------------------------
 * display_thread — Periodically redraws the terminal dashboard.
 *
 * On entry, clears the screen once.  Then loops:
 *   1. Hide cursor (reduces visual flicker)
 *   2. Move cursor to home and draw the dashboard
 *   3. Show cursor and position it at the input line
 *   4. Sleep for DISPLAY_REFRESH_MS
 *
 * Instead of save/restore (which can interfere with async input),
 * the cursor is always repositioned to the input line after drawing.
 * ---------------------------------------------------------------------------*/
void *display_thread(void *arg)
{
    system_state_t *sys = (system_state_t *)arg;

    /* Initial clear */
    printf(CLEAR_SCREEN CURSOR_HOME);
    fflush(stdout);

    while (sys->running) {
        printf(CURSOR_HIDE);        /* Hide cursor to reduce flicker */
        printf(CURSOR_HOME);        /* Move to top-left corner       */

        draw_dashboard(sys);

        printf(CURSOR_SHOW);        /* Restore cursor visibility     */
        /* Position cursor at input line (below dashboard) */
        printf("\033[%d;3H", DASHBOARD_LINES + 2);
        fflush(stdout);

        sleep_ms(DISPLAY_REFRESH_MS);
    }

    /* Final redraw so the user sees the last state */
    printf(CURSOR_HOME);
    draw_dashboard(sys);
    printf("\n%s--- System shutting down ---%s\n", ANSI_DIM, ANSI_RESET);
    fflush(stdout);

    return NULL;
}
