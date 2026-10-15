# RTEMS Embedded Control — Hardware Test Results

This directory contains selected photographs and terminal
screenshots documenting the RTEMS GPIO control application
developed for the Vertiefung Echtzeitsysteme course at
Berliner Hochschule für Technik (BHT).

## Hardware Environment

- Raspberry Pi
- Freenove GPIO Breakout Board
- RTEMS 7
- Serial console communication

## Functional Tests

The original laboratory experiments verified:

1. Periodic GPIO4 switching using a POSIX thread.
2. Falling-edge interrupt detection on GPIO17.
3. Interrupt-driven GPIO18 toggling.
4. Synchronisation using POSIX mutexes and semaphores.
5. Periodic status output through the serial console.
6. Runtime modification of the blinking interval.

## Experimental Evidence

The following evidence will be included:

- Hardware setup photograph
- LEDs operating on the physical hardware
- Default serial-console output
- Runtime interval-change demonstration

## Validation Scope

These results document the original laboratory tests
performed on Raspberry Pi hardware in 2026.

The source code published in this portfolio repository
includes subsequent corrections and has not been
independently revalidated on the physical hardware.

The experiments demonstrate functional behaviour,
not formally verified hard real-time timing guarantees.
