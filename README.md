# RTEMS Real-Time Embedded Control

**Embedded C development on RTEMS: multithreaded GPIO control on a Raspberry Pi, with supporting analysis of real-time scheduling, debugging, and priority inheritance.**

**Author:** Mazen Younis  
**Institution:** Berliner Hochschule für Technik (BHT)  
**Degree:** M.Eng. Computer Engineering / Embedded Systems  
**Course:** Vertiefung Echtzeitsysteme (Advanced Real-Time Systems)  
**Year:** 2026

## Overview

This repository presents work from an advanced real-time systems course. It combines **an RTEMS application written in C and tested on Raspberry Pi hardware** with a separate **investigation of RTEMS scheduling and synchronisation** using existing RTEMS examples.

The central engineering challenge is coordinating time-dependent GPIO operations, interrupt-triggered events, and shared application state across multiple threads. The supporting scheduling study investigates what happens when tasks compete for CPU time and shared resources, including a documented case of **priority inversion and priority inheritance**.

The work covers two complementary areas:

| Component | Focus | Evidence |
|---|---|---|
| **GPIO control application (Labor 3)** | C, RTEMS, POSIX threads, GPIO interrupts, semaphores, mutexes, runtime configuration | [Source](src/gpio_blink.c) · [Hardware results](results/README.md) · [Report](docs/reports/RTEMS_GPIO_Embedded_Control_Report.pdf) |
| **Scheduling and debugging study (Labor 2)** | `SCHED_FIFO`, thread states, GDB, QEMU, RTEMS Capture Engine, priority inheritance | [Analysis](docs/scheduling/README.md) · [Diagrams](docs/scheduling/) · [Report](docs/reports/RTEMS_Thread_Scheduling_Analysis_Report.pdf) |

> **Validation scope:** Hardware photographs, serial-console output, and academic reports document the original 2026 experiments. The C source published here is a **reviewed portfolio version with subsequent corrections**; it has **not been independently rebuilt with the original RTEMS toolchain or retested on hardware**. The results are not claimed as a fresh validation of that revised source.

## 1. RTEMS GPIO control on Raspberry Pi

The main application uses **RTEMS 7** with POSIX threading and synchronisation APIs. It was designed for a Raspberry Pi with a Freenove GPIO breakout board.

### GPIO assignments

| GPIO pin | Direction | Role |
|---|---|---|
| **GPIO4** | Output | Periodic switching, with a configurable interval |
| **GPIO17** | Input | Falling-edge interrupt input, connected to GPIO4 by a jumper wire |
| **GPIO18** | Output | Toggles when an interrupt event is processed |

### Thread architecture

| Execution context | POSIX priority | Responsibility |
|---|---:|---|
| **Thread A** | 80 | Switch GPIO4 periodically and read the configured interval |
| **Thread B** | 70 | Wait on a semaphore, process GPIO17 interrupt events, and toggle GPIO18 |
| **Thread C** | 60 | Print a periodic snapshot of the application state to the serial console |
| **GPIO17 ISR** | — | Signal an event to Thread B using `sem_post()` |

The application uses `SCHED_FIFO` for the three POSIX threads. A mutex protects shared state, including the blink interval and event count. A semaphore allows the interrupt handler to notify Thread B without busy-wait polling.

```text
        Thread A                        GPIO17 ISR                 Thread B
   Periodic GPIO4 output ----jumper--> falling-edge event ----> semaphore wait
                                                                  |
                                                                  v
                                                            Toggle GPIO18
                                                                  |
                                                                  v
                                                            Update event count

             Serial input --> Change blink interval (mutex-protected)
             Thread C    --> Periodic status output (mutex-protected snapshot)
```

### Hardware and experimental evidence

**Raspberry Pi and breakout board used in the laboratory:**

![Raspberry Pi and Freenove GPIO breakout board](results/hardware_setup.jpg)

The original laboratory tests documented periodic GPIO switching, interrupt-driven GPIO18 updates, and changes to the blinking interval during execution without restarting the program.

[**Explore hardware photos and runtime screenshots →**](results/README.md)

## 2. Scheduling, debugging, and priority inheritance

The second study analyses **existing RTEMS sample applications** rather than presenting them as original application implementations.

For the POSIX example `psx_example_3`, the investigation used **QEMU and GDB** to examine thread creation, priority-based execution, condition-variable timeouts, thread joins, and state transitions.

For the RTEMS Capture Engine example, execution traces were used to reconstruct a **priority-inversion scenario** involving three tasks:

| Task | RTEMS Classic API priority | Role |
|---|---:|---|
| `CT1a` | 102 | Lower-priority task holding a mutex |
| `CT1b` | 101 | Intermediate-priority task |
| `CT1c` | 100 | Higher-priority task waiting for the mutex |

In the RTEMS Classic API, a **lower numerical value means higher priority**. When `CT1c` blocks on a mutex held by `CT1a`, priority inheritance temporarily raises the effective priority of `CT1a` from **102 to 100**. The captured events illustrate how the mutex holder can run and release the shared resource.

**Selected scheduling evidence:**

![RTEMS scheduling Gantt diagram](docs/scheduling/P5_Gantt_Diagram.png)

[**Explore the thread state diagram, sequence diagram, and scheduling analysis →**](docs/scheduling/README.md)

## Technologies and concepts

**Languages and platforms:** C, RTEMS 7, Raspberry Pi, ARM cross-compilation  
**Concurrency:** POSIX threads (`pthread`), `SCHED_FIFO`, mutexes, semaphores, interrupt handling  
**Tooling:** Waf, QEMU, GDB, RTEMS Capture Engine, serial-console debugging  
**Systems topics:** real-time scheduling, task states, GPIO, synchronisation, priority inversion, priority inheritance

## Repository structure

```text
rtems-realtime-embedded-control/
├── README.md
├── src/
│   └── gpio_blink.c
├── results/
│   ├── README.md
│   ├── hardware_setup.jpg
│   ├── hardware_leds.JPG
│   ├── terminal_default.png
│   └── terminal_multiple_changes.png
└── docs/
    ├── scheduling/
    │   ├── README.md
    │   ├── Zustandsgraph_psx_example_3.png
    │   ├── sequence_diagram_psx_example_3.png
    │   └── P5_Gantt_Diagram.png
    └── reports/
        ├── README.md
        ├── RTEMS_Thread_Scheduling_Analysis_Report.pdf
        └── RTEMS_GPIO_Embedded_Control_Report.pdf
```

## Build and execution context

The embedded application is **RTEMS-specific**; `src/gpio_blink.c` cannot be compiled or executed as an ordinary desktop C program.

The original GPIO laboratory setup used an **RTEMS 7 toolchain**, a Raspberry Pi-compatible **`arm/raspberrypi2` BSP**, the RTEMS examples/Waf build environment, and a **115200-baud serial console**. The report describes producing a bootable image for the Raspberry Pi.

This portfolio repository currently **does not contain a self-contained Waf project or a verified automated build recipe**. For the exact original lab workflow, consult the [GPIO implementation report](docs/reports/RTEMS_GPIO_Embedded_Control_Report.pdf). Rebuilding the revised source requires a compatible RTEMS environment and verification against the selected BSP and GPIO APIs.

The scheduling study used RTEMS examples executed in **QEMU** and inspected with **GDB** and the **RTEMS Capture Engine**; see the [scheduling report](docs/reports/RTEMS_Thread_Scheduling_Analysis_Report.pdf).

## Key engineering lessons

- Keeping interrupt handlers short and handing work to a thread helps separate time-sensitive event notification from application logic.
- Mutex-protected state and semaphore-based event signalling address different synchronisation needs.
- Real-time thread priorities affect execution order, but do not by themselves establish deadline guarantees.
- Priority inheritance can mitigate priority inversion when a high-priority task waits for a resource owned by a lower-priority task.
- Hardware testing, debugging traces, and reproducible source-code builds are **different kinds of evidence** and should not be conflated.

## Limitations and public repository scope

- Functional hardware tests were documented for the **original course implementation**. The corrected public source has **not been revalidated on hardware**.
- No worst-case execution time, interrupt latency distribution, scheduling deadline analysis, or formal hard real-time guarantee is provided.
- The scheduling diagrams and trace analysis examine RTEMS-provided example applications.
- The repository contains selected source, documentation, and evidence rather than a complete RTEMS toolchain, board image, or copies of third-party operating-system sources.

## Documentation

- [Labor 2 — RTEMS Thread Scheduling and Analysis](docs/reports/RTEMS_Thread_Scheduling_Analysis_Report.pdf)
- [Labor 3 — RTEMS GPIO Embedded Control](docs/reports/RTEMS_GPIO_Embedded_Control_Report.pdf)
- [Scheduling and priority-inheritance diagrams](docs/scheduling/README.md)
- [Raspberry Pi hardware evidence and terminal output](results/README.md)

## Attribution

This academic project was completed by **Mazen Younis** at **Berliner Hochschule für Technik (BHT)** in 2026. The RTEMS scheduling study analyses upstream RTEMS example programs; those examples are not claimed as original source authored for this repository.
