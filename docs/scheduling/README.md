# RTEMS Real-Time Scheduling and Thread Analysis

An investigation of thread scheduling, synchronisation,
debugging and priority inheritance using RTEMS 7.

**Author:** Mazen Younis  
**Institution:** Berliner Hochschule für Technik (BHT)  
**Course:** Vertiefung Echtzeitsysteme  
**Year:** 2026

## Overview

This study investigates how a real-time operating system
schedules concurrent tasks and manages shared resources.

The experiments were conducted using RTEMS 7, QEMU,
GDB and the RTEMS Capture Engine.

Two RTEMS example applications were analysed:

1. `psx_example_3` — POSIX thread scheduling and synchronisation.
2. `capture` — Task scheduling, priority inversion and
   priority inheritance.

The work focuses on understanding and verifying the
behaviour of existing RTEMS examples rather than
developing a new scheduling algorithm.

## 1. POSIX Thread Scheduling

The `psx_example_3` application demonstrates
priority-based scheduling using POSIX threads.

The investigation covers:

- Thread creation with `pthread_create()`
- Priority-based scheduling using `SCHED_FIFO`
- Thread blocking through `pthread_cond_timedwait()`
- Synchronisation using `pthread_join()`
- Thread state transitions

The observed execution sequence was reconstructed
using source-code inspection and GDB debugging.

## 2. Dynamic Analysis with GDB

The application was executed under QEMU and examined
using the GNU Debugger.

Breakpoints were used to investigate:

- Thread creation
- Condition-variable waiting
- Thread execution order
- Thread termination and joining

The analysis compared the expected scheduling behaviour
with the observed runtime execution.

## 3. Priority Inversion and Priority Inheritance

The RTEMS Capture Engine was used to investigate
priority inversion in a three-task example.

The tasks have the following RTEMS priorities:

| Task | Priority | Role |
|---|---:|---|
| CT1a | 102 | Holds the shared mutex |
| CT1b | 101 | Intermediate-priority task |
| CT1c | 100 | Highest-priority task |

In the RTEMS Classic API, a lower numerical priority
represents a higher scheduling priority.

When CT1c attempts to acquire the mutex held by CT1a,
priority inversion occurs.

Priority inheritance temporarily raises CT1a's
effective priority from 102 to 100, allowing it
to execute and release the mutex.

The recorded execution traces demonstrate this
priority inheritance mechanism.

## 4. Experimental Evidence

The original laboratory work includes:

- POSIX thread state diagram
- Thread interaction sequence diagram
- RTEMS scheduling Gantt diagram
- GDB debugging transcript
- RTEMS Capture Engine execution trace
- Complete academic laboratory report

Selected diagrams and supporting documentation
will be added to this directory.

## 5. Key Learning Outcomes

This investigation demonstrates:

- Understanding of priority-based real-time scheduling
- Analysis of thread states and synchronisation
- Debugging embedded applications using GDB and QEMU
- Identification of priority inversion
- Interpretation of priority inheritance
- Analysis of recorded RTEMS scheduler events

## Scope

The scheduling experiments analyse RTEMS example
applications and their observed behaviour.

They complement the independently implemented
GPIO control application in this repository.

The experiments do not establish formal worst-case
execution-time or scheduling guarantees.
