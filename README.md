# Real-Time Elevator Control System using QNX Neutrino RTOS

A multi-threaded elevator simulation demonstrating real-time operating system concepts on QNX Neutrino. The system simulates **2 elevator cars** servicing an **8-floor building** with a live terminal dashboard, QNX message-passing IPC, and POSIX thread synchronisation.

---

## Architecture

```
┌───────────────────────────────────────────────────────────────┐
│                     elevator_sim (single binary)              │
│                                                               │
│  ┌─────────────┐    MsgSend     ┌──────────────────┐          │
│  │ Input Thread │──────────────►│ Request Manager   │          │
│  │ (user cmds)  │◄──────────────│ (MsgReceive loop) │          │
│  └─────────────┘    MsgReply    └────────┬─────────┘          │
│                                   mutex/cond │                 │
│                              ┌───────────┴───────────┐        │
│                              ▼                       ▼        │
│                     ┌──────────────┐       ┌──────────────┐   │
│                     │ Elevator 1   │       │ Elevator 2   │   │
│                     │ (SCAN alg.)  │       │ (SCAN alg.)  │   │
│                     └──────────────┘       └──────────────┘   │
│                              │                       │        │
│                              ▼                       ▼        │
│                     ┌──────────────────────────────────┐      │
│                     │ Shared State (mutex-protected)   │      │
│                     └───────────────┬──────────────────┘      │
│                                     │                         │
│                     ┌───────────────┴──────────────────┐      │
│                     ▼                                  ▼      │
│            ┌──────────────┐                   ┌────────────┐  │
│            │ Display Thd  │                   │ Logger     │  │
│            │ (ANSI dash.) │                   │ (file I/O) │  │
│            └──────────────┘                   └────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

### Threads

| Thread           | Purpose                                          | Key APIs                       |
| ---------------- | ------------------------------------------------ | ------------------------------ |
| Request Manager  | Receives IPC messages, dispatches to best elevator | `MsgReceive`, `MsgReply`       |
| Elevator 1       | State machine: move, open/close doors             | `pthread_cond_wait`, `nanosleep` |
| Elevator 2       | Independent elevator car (same logic)              | Same as Elevator 1             |
| Display          | Renders live ANSI terminal dashboard (500 ms)      | ANSI escape codes, `nanosleep` |
| Input Handler    | Reads user commands, sends via `MsgSend`           | `ConnectAttach`, `MsgSend`     |

### QNX IPC (Message Passing)

The system uses QNX Neutrino's synchronous message-passing kernel API:

1. **`ChannelCreate()`** — Request manager creates a channel
2. **`ConnectAttach()`** — Input thread connects to the channel
3. **`MsgSend()`** — Input thread sends command (blocks until reply)
4. **`MsgReceive()`** — Request manager receives command
5. **`MsgReply()`** — Request manager replies (unblocks sender)

A POSIX compatibility layer (`#ifndef __QNX__`) is included so the project compiles and runs on Linux/macOS for development and testing.

### Synchronisation Primitives

| Primitive          | Purpose                                              |
| ------------------ | ---------------------------------------------------- |
| `pthread_mutex_t`  | Protects per-elevator state and the log file         |
| `pthread_cond_t`   | Signals elevator threads when new work is assigned   |
| QNX IPC            | Synchronous handshake between input → request manager |

---

## Project Structure

```
qnx_project/
├── include/
│   └── elevator.h          # Types, constants, function declarations, compat layer
├── src/
│   ├── main.c              # Entry point, input handler, QNX compat implementation
│   ├── elevator.c          # Elevator state machine (SCAN algorithm)
│   ├── request_manager.c   # QNX IPC message receiver & dispatcher
│   ├── display.c           # ANSI terminal dashboard renderer
│   └── logger.c            # Thread-safe timestamped file logger
├── Makefile                 # Build for QNX (qcc) or host (gcc)
└── README.md                # This file
```

---

## Building

### Prerequisites

- **QNX SDP 7.x+** (for QNX target builds)
- **GCC** (for host/Linux development builds)
- **make**

### Build for QNX

```bash
make TARGET=qnx
```

This uses `qcc` with `aarch64le` as the target. Edit the `CFLAGS` in `Makefile` to change the target architecture (e.g., `x86_64`).

### Build for Host (Linux / macOS)

```bash
make
```

Uses `gcc` with POSIX threads. The QNX message-passing APIs are simulated by the compatibility layer in `main.c`.

### Clean

```bash
make clean
```

---

## Running

```bash
./elevator_sim
```

The terminal dashboard appears immediately. Type commands at the `>` prompt below the dashboard.

### Commands

| Command | Action                              | Example |
| ------- | ----------------------------------- | ------- |
| `f<N>`  | Request floor N (1–8)               | `f5`    |
| `e<N>`  | Emergency stop on elevator N (1–2)  | `e1`    |
| `c<N>`  | Clear emergency on elevator N       | `c1`    |
| `m<N>`  | Enable maintenance on elevator N    | `m2`    |
| `r<N>`  | Disable maintenance on elevator N   | `r2`    |
| `q`     | Quit the system                     | `q`     |

### Example Session

```
> f5          ← Request floor 5 (auto-dispatched to nearest elevator)
> f3          ← Request floor 3
> e1          ← Emergency stop elevator 1
> c1          ← Clear emergency, elevator 1 resumes
> m2          ← Put elevator 2 in maintenance mode
> r2          ← Take elevator 2 out of maintenance
> q           ← Clean shutdown
```

### Dashboard

The dashboard refreshes every 500 ms showing:

- **State**: IDLE, MOVING UP/DOWN, DOOR OPEN, EMERGENCY, MAINTENANCE
- **Floor**: Current floor (1–8) with direction arrow
- **Door**: OPEN or CLOSED
- **Floor map**: `[**]` = elevator position, `[ R]` = pending request, `[*R]` = both

### Log File

All events are logged to `elevator_log.txt` with millisecond timestamps:

```
[18:05:32.142] System: All threads started — ready for commands
[18:05:34.501] Request Manager: Floor 5 → assigned to Elevator 1
[18:05:34.502] Elevator 1: Moving UP from floor 1 toward floor 5
[18:05:36.503] Elevator 1: Arrived at floor 2
[18:05:38.504] Elevator 1: Arrived at floor 3
...
```

---

## Elevator Scheduling

The system uses the **SCAN (elevator) algorithm**:

1. Continue servicing requests in the current direction of travel
2. When no more requests ahead, reverse direction
3. When idle, pick the nearest pending request
4. The request manager dispatches to the **nearest available** elevator, preferring idle cars

---

## State Machine

```
                    ┌──────────────┐
                    │     IDLE     │◄──────────────────────────┐
                    └──────┬───────┘                           │
                           │ request assigned                  │
                    ┌──────┴───────┐                           │
              ┌─────┤  MOVING UP   │──── arrive ──► DOOR OPEN ─┘
              │     │  MOVING DOWN │                 (3 sec)
              │     └──────────────┘
              │            │
         emergency    maintenance
              │            │
              ▼            ▼
       EMERGENCY STOP   MAINTENANCE
       (wait clear)     (wait clear)
              │            │
              └────► IDLE ◄┘
```

---

## Design Decisions

1. **Single executable**: All functionality in one binary avoids process management complexity and simplifies deployment on embedded targets.
2. **QNX IPC + compatibility layer**: The `#ifndef __QNX__` compat layer means the same source code works on QNX and POSIX systems, making development and testing easier.
3. **Per-elevator mutex + condvar**: Each elevator has its own synchronisation primitives, minimising contention between cars.
4. **SCAN algorithm**: Matches real-world elevator scheduling — efficient and easy to understand for educational purposes.
5. **ANSI dashboard**: No external dependencies (ncurses not required). Works in any VT100-compatible terminal.

---

## QNX-Specific Notes

- On QNX Neutrino, `ChannelCreate()` / `MsgSend()` / `MsgReceive()` / `MsgReply()` are kernel-level operations providing priority-inheritance and deterministic latency — ideal for real-time systems.
- `nanosleep()` is used instead of `sleep()` for sub-second, real-time-friendly timing.
- The project can be extended with QNX timers (`timer_create`, `TimerTimeout`) for hardware-accurate timing.

---

## License

University coursework — for educational use only.
