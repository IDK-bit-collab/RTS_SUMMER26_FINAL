/*
 * Application 4 — Synchronization Quest (Part B of Quest 1)
 *
 * Scaffold level: ~70% complete.
 *
 * Scaffold Code - AI useage:
 *   Addition of the USE_PI_MUTEX compile-time switch and the H/M/L
 *     priority-inversion harness (lock plumbing + timestamp telemetry)
 *   Logic to allow for switching the lock primitive between an inheriting
 *     mutex and a non-inheriting binary semaphore
 *   Commenting of code including human readable summaries
 *
 * What this scaffold gives you (the baseline COMPILES and BEHAVES):
 *   - Binary semaphore — signals from ISR to a responder task
 *   - Counting semaphore — manages a pool of resources (3 slots, 4 consumers)
 *   - Mutex — protects a shared variable that two tasks modify
 *   - A built-in priority-inversion demo (tasks H/M/L) whose lock primitive is
 *     selected by one #define, so "show it both ways" is a flag flip rather
 *     than a manual swap. The H block->acquire wait is measured and logged.
 *
 * What you do:
 *   1. Refactor each primitive into its ROLE-APPROPRIATE use (justify in README).
 *   2. Theme the responder / pool / writer names, log strings, and resource.
 *   3. Implement the induced-failure section (remove one primitive, observe).
 *   4. Run the inversion demo in BOTH modes (flip USE_PI_MUTEX), quote the two
 *      H-wait numbers, and walk the timeline in your README.
 *
 * What you DON'T need to change:
 *   - The ISR, the debounce gate, or the three primitive create calls.
 *   - The H/M/L lock plumbing or the timestamp telemetry — just read the numbers
 *     it prints. Tune only the *_ITERS / *_DELAY_MS knobs if you want a cleaner
 *     separation between the two modes.
 *
 * ============================================================
 *  LOCK MODE  (priority-inversion demo)
 * ============================================================
 *
 * USE_PI_MUTEX selects the lock type shared by tasks H and L. Both modes run the
 * SAME H/M/L scenario and log the SAME fields (H's block->acquire wait, plus the
 * L and M timeline); only the lock primitive differs.
 *
 *   USE_PI_MUTEX = 1  -> H and L share a FreeRTOS MUTEX (priority inheritance
 *                        ON). When H blocks on the lock L holds, L inherits H's
 *                        priority, so M cannot preempt L. H waits about L's
 *                        remaining critical section — bounded.
 *   USE_PI_MUTEX = 0  -> H and L share a BINARY SEMAPHORE used as a lock (no
 *                        ownership, no inheritance). M preempts L while L still
 *                        holds the lock, so H waits for M to finish too — the
 *                        classic unbounded priority inversion.
 *
 * The separation only appears because L's critical section is CPU-bound (a fixed
 * iteration burn). If L merely slept, the CPU would be free, M would run in both
 * modes, and the two numbers would converge — which is why the demo burns cycles
 * instead of calling vTaskDelay inside the lock.
 *
 * ============================================================
 * Theme: Avionics
 * ============================================================
 */

#ifndef USE_PI_MUTEX
#define USE_PI_MUTEX 1
#endif

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_task_wdt.h"

#define BUTTON_GPIO GPIO_NUM_18

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "app4";

/* ---------- Synchronization primitives ---------- */
static SemaphoreHandle_t beacon_sig_sem;       /* binary — ISR → responder */
static SemaphoreHandle_t decoder_pool_sem;      /* counting — N=3 (resource pool) */
static SemaphoreHandle_t tracking_shared_mux;    /* mutex — protect shared_state */

/* Shared state guarded by shared_mux */
static int tracking_update_counter = 0;

/* ---------- ISR: signal the responder ---------- */
static volatile int64_t last_edge_us;
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - last_edge_us < 200) return;       /* debounce */
    last_edge_us = now;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(beacon_sig_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---------- Responder task: waits on the binary sem ---------- */
static void beacon_responder_task(void *arg)
{
    for (;;) {
        if (xSemaphoreTake(beacon_sig_sem, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "[beacon] UAV Beacon Pulse Active");
            /* TODO(YOU): theme-appropriate response.
             * Avionics: log a radar pulse.
             * Medical:  patient-call ACK.
             * Industrial: E-STOP acknowledge + set safe-state flag.
             * Space:    ground command receive.
             * Security: tamper-detected record.
             */
        }
    }
}

/* ---------- Pool-consumer task: takes from the counting sem ----------
 * Models a producer/consumer where the pool has 3 slots. */
static void telemetry_decoder_task(void *arg)
{
    int id = (int)(uintptr_t)arg;
    for (;;) {
        if (xSemaphoreTake(decoder_pool_sem, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGI(TAG, "[decoder#%d] acquired telemetry decoder", id);
            vTaskDelay(pdMS_TO_TICKS(500 + (id * 200)));   // simulated work 
            xSemaphoreGive(decoder_pool_sem);
            ESP_LOGI(TAG, "[decoder#%d] released telemetry decoder", id);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            ESP_LOGW(TAG, "[decoder#%d] no telemetry decoder available", id);
            
        }
    }
}

/* ---------- Two tasks racing on shared_state — guarded by mutex ---------- */
static void tracking_writer_task(void *arg)
{
    int id = (int)(uintptr_t)arg;
    const char *writer_name = (id == 1) ? "navigation" : "guidance";

    for (;;) {
        if (xSemaphoreTake(tracking_shared_mux, portMAX_DELAY) == pdTRUE) {
            int old = tracking_update_counter;
            tracking_update_counter = old + 1;
            ESP_LOGI(TAG,"[%s] tracking_update_counter %d -> %d", writer_name, old, tracking_update_counter);
            xSemaphoreGive(tracking_shared_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(150 + (id * 73)));
    }
}

/* ============================================================
 *  Priority-inversion demo  (tasks H / M / L)
 * ============================================================
 *
 * Classic three-task inversion on one core:
 *   - L (low,  prio 5)  grabs the lock and runs a long CPU-bound section.
 *   - H (high, prio 15) tries the lock shortly after and blocks on it.
 *   - M (mid,  prio 10) becomes ready a little later and burns CPU. It does NOT
 *     touch the lock — it is pure interference.
 *
 * MUTEX mode  (USE_PI_MUTEX=1): when H blocks, L inherits prio 15, so M cannot
 *   preempt L; L finishes its section and hands the lock to H. H's wait is
 *   bounded by L's remaining critical section.
 * BINARY-SEM mode (USE_PI_MUTEX=0): L stays at prio 5; M preempts L while L holds
 *   the lock, so H waits for M to drain too. The wait inflates by M's run time.
 *
 * Read the "[PI][H] ... waited N us" line in each mode; that delta is the lesson.
 */
#if USE_PI_MUTEX
#define PI_LOCK_CREATE() xSemaphoreCreateMutex()
#define PI_LOCK_NAME     "MUTEX (priority inheritance ON)"
#else
#define PI_LOCK_CREATE() xSemaphoreCreateBinary()
#define PI_LOCK_NAME     "BINARY SEM (no inheritance)"
#endif

static SemaphoreHandle_t pi_lock;

/* Stagger knobs — control the ordering, not the durations. */
#define PI_H_DELAY_MS  50      /* H tries the lock 50 ms after start (after L holds it) */
#define PI_M_DELAY_MS  100     /* M becomes ready 100 ms after start */

/* Work knobs — fixed-iteration CPU burns. TUNE on Wokwi using the logged
 * wall-clock durations: aim for L ~500 ms and M ~1000 ms when each runs alone.
 * Absolute values do not affect WHICH mode wins; they set how large the gap is. */
#define PI_L_ITERS  20000000UL
#define PI_M_ITERS  40000000UL

static volatile uint32_t pi_sink;       /* defeats dead-code elimination */
static void pi_burn(uint32_t iters)
{
    uint32_t x = pi_sink ? pi_sink : 1u;
    for (uint32_t i = 0; i < iters; i++) { x ^= (x << 5); x += i; }
    pi_sink = x;
}

static void pi_low_task(void *arg)
{
    /* L is created last in app_main, so it grabs the lock immediately. */
    xSemaphoreTake(pi_lock, portMAX_DELAY);
    int64_t t_acq = esp_timer_get_time();
    ESP_LOGI(TAG, "[PI][L] took lock @ %lld us — entering CPU-bound section",
             (long long)t_acq);
    pi_burn(PI_L_ITERS);
    int64_t t_rel = esp_timer_get_time();
    xSemaphoreGive(pi_lock);
    ESP_LOGI(TAG, "[PI][L] released lock @ %lld us (held %lld us wall-clock)",
             (long long)t_rel, (long long)(t_rel - t_acq));
    vTaskDelete(NULL);
}

static void pi_med_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(PI_M_DELAY_MS));
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "[PI][M] ready @ %lld us — burning CPU (takes no lock)",
             (long long)t0);
    pi_burn(PI_M_ITERS);
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "[PI][M] done  @ %lld us (ran %lld us wall-clock)",
             (long long)t1, (long long)(t1 - t0));
    vTaskDelete(NULL);
}

static void pi_high_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(PI_H_DELAY_MS));
    int64_t t_block = esp_timer_get_time();
    ESP_LOGI(TAG, "[PI][H] wants lock @ %lld us — blocking", (long long)t_block);
    xSemaphoreTake(pi_lock, portMAX_DELAY);
    int64_t t_acq = esp_timer_get_time();
    int64_t wait = t_acq - t_block;
    ESP_LOGW(TAG, "[PI][H] ACQUIRED @ %lld us — waited %lld us (~%lld ms)  [lock=%s]",
             (long long)t_acq, (long long)wait, (long long)(wait / 1000), PI_LOCK_NAME);
    xSemaphoreGive(pi_lock);
    vTaskDelete(NULL);
}

static void start_inversion_demo(void)
{
    pi_lock = PI_LOCK_CREATE();
#if !USE_PI_MUTEX
    /* A binary semaphore is created empty; prime it once so it starts "unlocked". */
    xSemaphoreGive(pi_lock);
#endif
    ESP_LOGI(TAG, "[PI] inversion demo lock = %s", PI_LOCK_NAME);

    /* Create H and M first (they delay before acting), then L LAST so L wins the
     * lock the instant it is created instead of starving app_main while it burns. */
    xTaskCreatePinnedToCore(pi_high_task, "H", 4096, NULL, 15, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(pi_med_task,  "M", 4096, NULL, 10, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(pi_low_task,  "L", 4096, NULL,  5, NULL, APP_CPU_NUM);
}

/* ---------- app_main ---------- */
void app_main(void)
{
    esp_task_wdt_reconfigure(&(esp_task_wdt_config_t){.timeout_ms = 10000, .idle_core_mask = 0, .trigger_panic = false });
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== App 4 [Avionics] starting — sync quest ====");
    ESP_LOGI(TAG, "Lock mode: %s (USE_PI_MUTEX=%d)", PI_LOCK_NAME, USE_PI_MUTEX);

    beacon_sig_sem    = xSemaphoreCreateBinary();
    decoder_pool_sem   = xSemaphoreCreateCounting(3, 3);
    tracking_shared_mux = xSemaphoreCreateMutex();

    /* ISR + responder */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

    xTaskCreatePinnedToCore(beacon_responder_task, "beacon_responder", 4096, NULL, 12, NULL, APP_CPU_NUM);

    /* Pool consumers — 4 contending for 3 slots */
    for (int i = 1; i <= 4; i++) {
        xTaskCreatePinnedToCore(telemetry_decoder_task, "telemetry_decoder", 4096,
                                (void*)(uintptr_t)i, 5, NULL, APP_CPU_NUM);
    }

    /* Shared-state writers — both update under the mutex */
    xTaskCreatePinnedToCore(tracking_writer_task, "Nav_writer", 4096, (void*)1, 8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(tracking_writer_task, "Guidance_writer", 4096, (void*)2, 8, NULL, APP_CPU_NUM);

    /* Priority-inversion demo (H/M/L). For the cleanest H-wait numbers, you can
     * temporarily comment out the pool/writer creation above so Core 1 carries
     * only this demo. */
    start_inversion_demo();
}
