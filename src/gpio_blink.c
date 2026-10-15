/**
 * @file    gpio_blink.c
 * @brief   RTEMS GPIO control with POSIX threads and GPIO interrupts
 *
 * BHT Berlin - Master's programme in Embedded Systems
 * Course: Vertiefung Echtzeitsysteme (2026)
 *
 * Portfolio review of the course source (not revalidated on RTEMS hardware).
 * This source uses RTEMS GPIO services alongside POSIX threads, mutexes,
 * and semaphores. It is not a general-purpose Linux GPIO application.
 *
 * Hardware (as documented in the submitted course report):
 *   - Raspberry Pi and Freenove breakout board
 *   - GPIO4  -> LED A (configurable blinking interval)
 *   - GPIO18 -> LED B (toggles after GPIO17 falling-edge event)
 *   - GPIO17 -> Input connected to GPIO4 via jumper wire
 *
 * Threads (POSIX SCHED_FIFO):
 *   - thread_a (priority 80): toggles GPIO4
 *   - thread_b (priority 70): waits for IRQ notification, toggles GPIO18
 *   - thread_c (priority 60): reports status via serial console
 *
 * Requires a suitable RTEMS toolchain, Raspberry Pi BSP and GPIO support.
 * Build integration must follow the RTEMS environment used for this BSP.
 */

/* POSIX-Kompatibilität unter RTEMS sicherstellen */
#define _GNU_SOURCE

#include <rtems.h>
#include <rtems/bspIo.h>        /* printk()                  */
#include <bsp/gpio.h>           /* rtems_gpio_*              */

/* POSIX-Header */
#include <pthread.h>            /* pthread_*, Mutex, Kondition*/
#include <semaphore.h>          /* sem_t (POSIX Semaphor)    */
#include <sched.h>              /* SCHED_FIFO, sched_param   */
#include <time.h>               /* nanosleep(), clock_gettime*/
#include <errno.h>              /* errno, EINTR              */
#include <stdio.h>              /* printf()                  */
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>             /* strerror()                */

/* =========================================================================
 * Konstanten und Konfiguration
 * ========================================================================= */

#define GPIO_LED_A              4       /**< GPIO-Pin LED A (Blinken)       */
#define GPIO_LED_B              18      /**< GPIO-Pin LED B (Toggle)         */
#define GPIO_IRQ_IN             17      /**< GPIO-Pin Interrupt-Eingang      */

/*
 * QEMU-Loopback: QEMU emuliert die physische Drahtbrücke GPIO4 -> GPIO17 nicht.
 * Bei SIMULATE_GPIO_IRQ == 1 ruft thread_a den Callback beim
 * High->Low-Uebergang direkt auf und loest damit denselben Semaphor-
 * /thread_b-Pfad aus (Software-Ereignisinjektion, kein echter GPIO-IRQ).
 * Auf echter Hardware (mit Drahtbruecke) MUSS dies 0 sein, um doppelte
 * Ereignisse zu vermeiden. Dieses Simulationsverhalten ist nicht
 * unabhaengig auf QEMU nachgetestet worden.
 */
#ifndef SIMULATE_GPIO_IRQ
#define SIMULATE_GPIO_IRQ       0
#endif

#define BLINK_INTERVAL_DEFAULT_MS   500
#define BLINK_INTERVAL_MIN_MS       1
#define BLINK_INTERVAL_MAX_MS       1000

/**
 * POSIX-Echtzeit-Prioritäten für SCHED_FIFO.
 * Höherer Wert = höhere Priorität (umgekehrt zu RTEMS-nativer API!).
 */
#define PRIO_THREAD_A   80      /** Hoechste Prioritaet: GPIO4-Steuerung     */
#define PRIO_THREAD_B   70      /** Mittel: GPIO18-Toggle                  */
#define PRIO_THREAD_C   60      /** Niedrig: Konsolenausgabe               */

/** Stack-Größe für alle Threads */
#define THREAD_STACK_SIZE   (16 * 1024)

/* =========================================================================
 * Gemeinsamer Zustand (shared state)
 * ========================================================================= */

/** Blinkintervall in ms - durch mutex_state geschützt */
static volatile uint32_t blink_interval_ms = BLINK_INTERVAL_DEFAULT_MS;

/** Aktueller LED-A-Zustand */
static volatile bool led_a_state = false;

/** Zähler der High->LowUeergaenge an GPIO4 */
static volatile uint32_t toggle_count = 0;

/** Laufzeit in Sekunden */
static volatile uint32_t uptime_sec = 0;

/* =========================================================================
 * POSIX-Synchronisationsobjekte
 * ========================================================================= */

/**
 * Mutex schuetzt alle geteilten Variablen.
 * Attribute werden in sync_init() beim einmaligen pthread_mutex_init()
 * gesetzt: PTHREAD_MUTEX_ERRORCHECK und PTHREAD_PRIO_INHERIT.
 */
static pthread_mutex_t mutex_state;

/**
 * Ereignis-Semaphor: GPIO17-Callback signalisiert thread_b ueber
 * sem_post(). Keine POSIX-Bedingungsvariable wird verwendet.
 * Die Freigabe aus dem Interrupt-Kontext muss vom eingesetzten
 * RTEMS-BSP / der POSIX-Implementierung unterstuetzt werden.
 */
static sem_t sem_isr_event;

/* =========================================================================
 * Hilfsmakro: POSIX-Returncodes prüfen
 * ========================================================================= */

/**
 * Prueft einen POSIX-Returncode (0 = Erfolg).
 * Gibt Fehlermeldung aus und beendet die Applikation bei Fehler.
 */
#define CHECK_POSIX(rc, msg) \
    do { \
        int _rc = (rc); \
        if (_rc != 0) { \
            printk("[FEHLER] %s: %s: %s (rc=%d)\n", \
                   __func__, (msg), strerror(_rc), _rc); \
            rtems_fatal_error_occurred((uint32_t)_rc); \
        } \
    } while (0)

/* =========================================================================
 * Hilfsfunktion: Millisekunden schlafen (nanosleep)
 * ========================================================================= */

/**
 * @brief  Schläft für die angegebene Anzahl Millisekunden.
 *         Behandelt EINTR (Unterbrechung durch Signal) korrekt.
 * @param  ms  Schlafzeit in Millisekunden
 */
static void sleep_ms(uint32_t ms)
{
    struct timespec req = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };

    /* nanosleep() kann durch Signale unterbrochen werden -> neu starten */
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ; /* weiter warten mit verbleibender Zeit (req wird aktualisiert) */
}

/* =========================================================================
 * Oeffentliche API: Blinkintervall zur Laufzeit aendern
 * ========================================================================= */

/**
 * @brief  Setzt das Blinkintervall thread-sicher.
 * @param  ms  Neues Intervall [BLINK_INTERVAL_MIN_MS .. BLINK_INTERVAL_MAX_MS]
 * @return true bei Erfolg, false bei ungueltigem Wert
 */
bool set_blink_interval(uint32_t ms)
{
    if (ms < BLINK_INTERVAL_MIN_MS || ms > BLINK_INTERVAL_MAX_MS) {
        printk("[WARN] set_blink_interval: %u ms ungueltig [%u..%u]\n",
               ms, BLINK_INTERVAL_MIN_MS, BLINK_INTERVAL_MAX_MS);
        return false;
    }

    CHECK_POSIX(pthread_mutex_lock(&mutex_state),   "lock in set_blink_interval");
    blink_interval_ms = ms;
    CHECK_POSIX(pthread_mutex_unlock(&mutex_state), "unlock in set_blink_interval");

    printk("[INFO] Blinkintervall geaendert: %u ms\n", ms);
    return true;
}

/* =========================================================================
 * Interrupt Service Routine - GPIO17 (fallende Flanke)
 * ========================================================================= */

/**
 * @brief  ISR: wird bei High->Low-Flanke an GPIO17 aufgerufen.
 *
 * sem_post() ist async-signal-safe und darf im ISR-Kontext aufgerufen
 * werden. Kein Mutex, kein printk, kein malloc in der ISR!
 */
static rtems_gpio_irq_state gpio17_isr(void *arg)
{
    (void)arg;

    /*
     * sem_post() weckt thread_b aus dem sem_wait() auf.
     * Returncode kann im ISR nicht geprüft werden.
     */
    sem_post(&sem_isr_event);

    return IRQ_HANDLED;
}

/* =========================================================================
 * Thread A: GPIO4 Blinken
 * ========================================================================= */

/**
 * @brief  Thread A -schaltet GPIO4 zyklisch ein und aus.
 *
 * Liest das Intervall zu Beginn jeder Phase neu (Mutex-geschuetzt),
 * damit Laufzeitaenderungen sofort wirken.
 */
static void *thread_a(void *arg)
{
    (void)arg;
    printk("[Thread A] gestartet (SCHED_FIFO, Prio %d)\n", PRIO_THREAD_A);

    while (true) {
        uint32_t interval_ms;

        /* --- AN-Phase --- */
        CHECK_POSIX(pthread_mutex_lock(&mutex_state),   "Thread A: lock AN");
        led_a_state  = true;
        interval_ms  = blink_interval_ms;
        CHECK_POSIX(pthread_mutex_unlock(&mutex_state), "Thread A: unlock AN");

        rtems_gpio_set(GPIO_LED_A);
        sleep_ms(interval_ms);

        /* --- AUS-Phase --- */
        CHECK_POSIX(pthread_mutex_lock(&mutex_state),   "Thread A: lock AUS");
        led_a_state  = false;
        interval_ms  = blink_interval_ms;
        CHECK_POSIX(pthread_mutex_unlock(&mutex_state), "Thread A: unlock AUS");

        rtems_gpio_clear(GPIO_LED_A);   /* High->Low-Flanke an GPIO4 */

#if SIMULATE_GPIO_IRQ
        /* QEMU-Loopback: emuliert die Drahtbrücke GPIO4 -> GPIO17 (s.o.). */
        gpio17_isr(NULL);
#endif

        sleep_ms(interval_ms);
    }

    return NULL;    /* wird nie erreicht */
}

/* =========================================================================
 * Thread B: GPIO18 Toggle (interruptgesteuert)
 * ========================================================================= */

/**
 * @brief  Thread B - wartet auf ISR-Semaphor, togglet GPIO18, zaehlt Ereignisse.
 *
 * sem_wait() blockiert, bis gpio17_isr() sem_post() aufruft.
 * EINTR wird behandelt (sem_wait kann durch Signale unterbrochen werden).
 */
static void *thread_b(void *arg)
{
    (void)arg;
    bool led_b_state = false;

    printk("[Thread B] gestartet (SCHED_FIFO, Prio %d), warte auf ISR...\n",
           PRIO_THREAD_B);

    while (true) {
        /* Blockiere bis ISR sem_post() aufruft */
        while (sem_wait(&sem_isr_event) == -1) {
            if (errno == EINTR)
                continue;   /* Signal - erneut versuchen */
            /* Anderer Fehler: fatal */
            printk("[FEHLER] Thread B: sem_wait fehlgeschlagen: %s\n",
                   strerror(errno));
            rtems_fatal_error_occurred(1);
        }

        /* GPIO18 togglen */
        led_b_state = !led_b_state;
        if (led_b_state) {
            rtems_gpio_set(GPIO_LED_B);
        } else {
            rtems_gpio_clear(GPIO_LED_B);
        }

        /* Zähler thread-sicher eroehen */
        CHECK_POSIX(pthread_mutex_lock(&mutex_state),   "Thread B: lock count");
        toggle_count++;
        CHECK_POSIX(pthread_mutex_unlock(&mutex_state), "Thread B: unlock count");
    }

    return NULL;
}

/* =========================================================================
 * Thread C: Konsolenausgabe (Sekundentakt)
 * ========================================================================= */

/**
 * @brief  Thread C - gibt jede Sekunde den Systemzustand aus.
 *
 * Verwendet clock_nanosleep() mit absolutem Zeitpunkt für paezisen
 * Sekundentakt (verhindert Drift durch Ausfuehrungszeit).
 */
static void *thread_c(void *arg)
{
    (void)arg;
    struct timespec next_wakeup;

    printk("[Thread C] gestartet (SCHED_FIFO, Prio %d)\n", PRIO_THREAD_C);

    /* Startzeit holen */
    clock_gettime(CLOCK_MONOTONIC, &next_wakeup);

    while (true) {
        bool     snap_led_a;
        uint32_t snap_count;
        uint32_t snap_interval;
        uint32_t snap_uptime;

        /* Naechsten Aufwachzeitpunkt um 1 Sekunde erhoehen */
        next_wakeup.tv_sec += 1;

        /* Absoluter, driftfreier Sekundentakt */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wakeup, NULL);

        /* Zustandsschnappschuss unter Mutex */
        CHECK_POSIX(pthread_mutex_lock(&mutex_state),   "Thread C: lock");
        snap_led_a    = led_a_state;
        snap_count    = toggle_count;
        snap_interval = blink_interval_ms;
        uptime_sec++;
        snap_uptime   = uptime_sec;
        CHECK_POSIX(pthread_mutex_unlock(&mutex_state), "Thread C: unlock");

        /* Konsolenausgabe - printf puffert, printk ist direkt */
        printk("[T=%4us] GPIO4: %-3s | Toggles GPIO18: %4u | Intervall: %5u ms\n",
               snap_uptime,
               snap_led_a ? "AN" : "AUS",
               snap_count,
               snap_interval);
    }

    return NULL;
}

/* =========================================================================
 * Hilfsfunktion: pthread mit Echtzeit-Priorität anlegen
 * ========================================================================= */

/**
 * @brief  Erstellt einen pthread mit SCHED_FIFO-Scheduling und definierter
 *         Prioritaet sowie Stack-Grsse
 *
 * @param  tid        Zeiger auf pthread_t-Variable (Ausgabe)
 * @param  priority   POSIX-Echtzeitpriorität (höher = wichtiger)
 * @param  start_fn   Thread-Funktion
 * @param  arg        Argument fuer Thread-Funktion
 * @param  name       Name fuer Debugausgabe
 */
static void create_rt_thread(pthread_t *tid,
                              int priority,
                              void *(*start_fn)(void *),
                              void *arg,
                              const char *name)
{
    pthread_attr_t      attr;
    struct sched_param  param;

    CHECK_POSIX(pthread_attr_init(&attr), "attr_init");

    /* Eigene Scheduling-Policy statt Vererbung vom Eltern-Thread */
    CHECK_POSIX(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED),
                "setinheritsched");

    /* SCHED_FIFO: prioritaetsbasiertes, praemptives FIFO-Scheduling */
    CHECK_POSIX(pthread_attr_setschedpolicy(&attr, SCHED_FIFO),
                "setschedpolicy");

    param.sched_priority = priority;
    CHECK_POSIX(pthread_attr_setschedparam(&attr, &param), "setschedparam");

    /* Stack-Größe explizit setzen */
    CHECK_POSIX(pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE),
                "setstacksize");

    CHECK_POSIX(pthread_create(tid, &attr, start_fn, arg), "pthread_create");
    CHECK_POSIX(pthread_attr_destroy(&attr), "attr_destroy");

    printk("[Init] Thread '%s' erstellt (SCHED_FIFO, Prio %d)\n", name, priority);
}

/* =========================================================================
 * GPIO-Initialisierung
 * ========================================================================= */

static void gpio_init(void)
{
    rtems_status_code sc;

    sc = rtems_gpio_initialize();
    if (sc != RTEMS_SUCCESSFUL) {
        printk("[FEHLER] rtems_gpio_initialize: %d\n", sc);
        rtems_fatal_error_occurred((uint32_t)sc);
    }

    /* GPIO4: Ausgang, LED A */
    sc = rtems_gpio_request_pin(GPIO_LED_A, DIGITAL_OUTPUT, false, false, NULL);
    if (sc != RTEMS_SUCCESSFUL) {
        printk("[FEHLER] GPIO4 konfigurieren: %d\n", sc);
        rtems_fatal_error_occurred((uint32_t)sc);
    }
    rtems_gpio_clear(GPIO_LED_A);

    /* GPIO18: Ausgang, LED B */
    sc = rtems_gpio_request_pin(GPIO_LED_B, DIGITAL_OUTPUT, false, false, NULL);
    if (sc != RTEMS_SUCCESSFUL) {
        printk("[FEHLER] GPIO18 konfigurieren: %d\n", sc);
        rtems_fatal_error_occurred((uint32_t)sc);
    }
    rtems_gpio_clear(GPIO_LED_B);

    /* GPIO17: Eingang, Interrupt (Drahtbruecke von GPIO4 erforderlich!) */
    sc = rtems_gpio_request_pin(GPIO_IRQ_IN, DIGITAL_INPUT, false, false, NULL);
    if (sc != RTEMS_SUCCESSFUL) {
        printk("[FEHLER] GPIO17 konfigurieren: %d\n", sc);
        rtems_fatal_error_occurred((uint32_t)sc);
    }

    /* ISR: fallende Flanke an GPIO17 aktivieren und Handler registrieren.
     * rtems_gpio_enable_interrupt() nimmt Interrupt-Typ, Handler-Flag und
     * threaded-handling-Flag entgegen (rtems_gpio_interrupt_handler_install()
     * hat nur 3 Parameter und setzt voraus, dass der Interrupt bereits aktiv ist). */
    sc = rtems_gpio_enable_interrupt(
            GPIO_IRQ_IN,
            FALLING_EDGE,
            UNIQUE_HANDLER,
            false,
            gpio17_isr,
            NULL);
    if (sc != RTEMS_SUCCESSFUL) {
        printk("[FEHLER] ISR registrieren: %d\n", sc);
        rtems_fatal_error_occurred((uint32_t)sc);
    }

    printk("[Init] GPIO konfiguriert: LED_A=%d, LED_B=%d, IRQ=%d\n",
           GPIO_LED_A, GPIO_LED_B, GPIO_IRQ_IN);
}

/* =========================================================================
 * POSIX-Synchronisationsobjekte initialisieren
 * ========================================================================= */

static void sync_init(void)
{
    /* Mutex mit Priority-Inheritance-Protokoll */
    pthread_mutexattr_t mattr;
    CHECK_POSIX(pthread_mutexattr_init(&mattr), "mutexattr_init");
    CHECK_POSIX(pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT),
                "mutexattr setprotocol INHERIT");
    CHECK_POSIX(pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_ERRORCHECK),
                "mutexattr settype ERRORCHECK");
    CHECK_POSIX(pthread_mutex_init(&mutex_state, &mattr), "mutex_state init");
    CHECK_POSIX(pthread_mutexattr_destroy(&mattr), "mutexattr_destroy");

    /*
     * POSIX-Semaphor (unbenannt, prozesslokal).
     * Startwert 0: thread_b blockiert sofort auf sem_wait().
     */
    if (sem_init(&sem_isr_event, 0, 0) != 0) {
        printk("[FEHLER] sem_init: %s\n", strerror(errno));
        rtems_fatal_error_occurred(1);
    }

    printk("[Init] POSIX-Synchronisationsobjekte angelegt\n");
}

/* =========================================================================
 * RTEMS Init-Task (Einstiegspunkt)
 * ========================================================================= */

/**
 * @brief  RTEMS Init-Task - startet die Applikation.
 *
 * Initialisiert erst die Synchronisationsobjekte, danach das GPIO-System
 * und drei POSIX-Threads. Die serielle Konsole liest Aenderungen des
 * Blinkintervalls ein; es gibt keinen automatischen Wechsel nach 10 s.
 */
rtems_task Init(rtems_task_argument arg)
{
    (void)arg;

    printk("\n=== BHT RTEMS GPIO Blink Demo (pthreads) ===\n");
    printk("GPIO4  -> LED A (Blinken, Standard: %u ms)\n",
           BLINK_INTERVAL_DEFAULT_MS);
    printk("GPIO18 -> LED B (Toggle bei GPIO4 High->Low)\n");
    printk("GPIO17 -> Interrupt-Eingang (Drahtbruecke von GPIO4!)\n\n");

    /* ISR kann erst nach Initialisierung des Semaphors aktiv werden. */
    sync_init();
    gpio_init();

    /* Threads anlegen */
    pthread_t tid_a, tid_b, tid_c;
    create_rt_thread(&tid_a, PRIO_THREAD_A, thread_a, NULL, "thread_a");
    create_rt_thread(&tid_b, PRIO_THREAD_B, thread_b, NULL, "thread_b");
    create_rt_thread(&tid_c, PRIO_THREAD_C, thread_c, NULL, "thread_c");

    (void)tid_a; (void)tid_b; (void)tid_c;   /* Threads laufen eigenstaendig weiter */

    /*
     * Laufzeit-Steuerung über die serielle Konsole (UART):
     * Der Init-Task liest zeilenweise von stdin. Jede eingegebene Zahl im
     * Bereich 1..1000 setzt das Blinkintervall von GPIO4 sofort neu
     * (thread-sicher über set_blink_interval()). Ungültige Eingaben werden
     * abgewiesen, der Rest der Applikation läuft unverändert weiter.
     *
     * Bedienung: ueber die serielle Konsole eine Zahl + ENTER eingeben.
     */
    char line[32];

    printk("\n>> Laufzeit-Steuerung aktiv: Zahl 1..1000 (ms) eingeben + ENTER,\n");
    printk(">> um das GPIO4-Blinkintervall zu setzen.\n\n");

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char    *end;
        long     val = strtol(line, &end, 10);

        /* Keine Ziffer in der Zeile? -> ignorieren */
        if (end == line) {
            continue;
        }

        if (val < 1 || val > 1000) {
            printk("[WARN] Ungueltig: %ld — bitte eine Zahl zwischen 1 und 1000 eingeben.\n",
                   val);
            continue;
        }

        /* set_blink_interval() prüft selbst nochmals und sperrt den Mutex */
        set_blink_interval((uint32_t)val);
    }

    /* stdin-EOF (im Normalbetrieb nicht erwartet): Init-Task beenden. */
    rtems_task_delete(RTEMS_SELF);
}

/* =========================================================================
 * RTEMS-Konfiguration
 * =========================================================================
 *
 * Wichtig: CONFIGURE_POSIX_API aktiviert die POSIX-Schnittstelle.
 * Ohne dieses Define sind pthread_create, sem_init etc. nicht verfuegbar.
 * ========================================================================= */

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER

/*
 * Objekt-Verwaltung: CONFIGURE_UNLIMITED_OBJECTS lässt RTEMS alle Objektklassen
 * (Tasks, Semaphore, …) nach Bedarf nachallozieren — inklusive des klassischen
 * Semaphors, das das GPIO-Subsystem intern anlegt. Das erspart das manuelle
 * Abzählen von CONFIGURE_MAXIMUM_SEMAPHORES (sonst scheitert rtems_gpio_initialize
 * mit RTEMS_UNSATISFIED). CONFIGURE_UNIFIED_WORK_AREAS legt Workspace und Heap
 * zusammen, damit größere Stacks/Objekte den gemeinsamen Speicher nutzen.
 * (Konfiguration aus der vorliegenden Kursdatei; BSP-spezifisch pruefen.)
 */
#define CONFIGURE_UNLIMITED_OBJECTS
#define CONFIGURE_UNIFIED_WORK_AREAS

/* POSIX API aktivieren - zwingend für pthreads/sem_t */
#define CONFIGURE_POSIX_API

/* Tick-Rate: 1 ms für nanosleep-Genauigkeit */
#define CONFIGURE_MICROSECONDS_PER_TICK         1000

/* Startgrößen der unbegrenzten Objektklassen (wachsen bei Bedarf weiter) */
#define CONFIGURE_MAXIMUM_POSIX_THREADS         20
#define CONFIGURE_MAXIMUM_POSIX_SEMAPHORES      4
#define CONFIGURE_MAXIMUM_POSIX_MUTEXES         4
#define CONFIGURE_MAXIMUM_TASKS                 rtems_resource_unlimited(5)

/* Init-Task-Stack: gross genug fuer gpio_init und sync_init */
#define CONFIGURE_INIT_TASK_STACK_SIZE          (16 * 1024)

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ATTRIBUTES          RTEMS_DEFAULT_ATTRIBUTES
#define CONFIGURE_INIT_TASK_INITIAL_MODES       RTEMS_DEFAULT_MODES

/*
 * Init-Task-Prioritaet: niedriger als alle pthreads, damit die Threads
 * nach dem Start sofort die CPU bekommen.
 * (RTEMS- und POSIX-Prioritaeten sind nicht direkt gleichzusetzen.)
 */
#define CONFIGURE_INIT_TASK_PRIORITY            100

#define CONFIGURE_INIT
#include <rtems/confdefs.h>
