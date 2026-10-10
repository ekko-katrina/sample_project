"""Exercise the actual pairing control flow with deterministic BLE/RTOS mocks."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "components/BSP/TESLALINK/tesla_link.c").read_text()
pair = source[source.index("static esp_err_t pair_vehicle("):
              source.index("static esp_err_t connect_vehicle(")]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
typedef uint32_t TickType_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define TESLA_ERR_KEY_NOT_ON_WHITELIST 3
#define TESLA_LINK_ERR_SAVE_FAILED 4
#define TESLA_STATE_DISCONNECTED 0
#define TESLA_DOMAIN_VEHICLE_SECURITY 2
#define TESLA_STAGE_WAITING_APPROVAL 4
#define TESLA_STAGE_PAIRED 5
#define TESLA_STAGE_AUTHENTICATED 6
#define TESLA_ERROR_NONE 0
#define PAIRING_APPROVAL_MS 60000
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xffffffff
#define pdTRUE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static uint32_t g_revision = 1;
static int g_lock, attempts, successes_after, session_error, saved, save_error;
static int disconnected, monitored, authenticated, connected, scan_count;
static TickType_t tick, notified_at;
static int xSemaphoreTake(int lock, uint32_t timeout) { (void)lock; (void)timeout; return 1; }
static void xSemaphoreGive(int lock) { (void)lock; }
static TickType_t xTaskGetTickCount(void) { return tick; }
static int tesla_ble_get_state(void) { return connected; }
static int scan_and_wait_for_service(const char *vin) { (void)vin; connected = 1; scan_count++; return ESP_OK; }
static int tesla_ble_add_key_request(void) { return ESP_OK; }
static void tesla_ble_disconnect(void) { connected = 0; disconnected++; }
static int tesla_ble_start_session(int domain) {
    assert(domain == 2);
    attempts++;
    return successes_after && attempts >= successes_after ? ESP_OK : session_error;
}
static int save_paired(bool paired, uint32_t revision) {
    assert(paired && revision == g_revision);
    if (!save_error) saved++;
    return save_error;
}
static void set_stage(int stage, int error) {
    assert(error == 0);
    if (stage == 6) { assert(saved == 1); authenticated++; notified_at = tick; }
}
static int monitor_vehicle(void) {
    assert(authenticated == 1 && connected && disconnected == 0);
    monitored++; return ESP_OK;
}
static void ulTaskNotifyTake(int clear, TickType_t interval) {
    (void)clear; assert(interval <= 2000); tick += interval;
}
'''
harness += pair
harness += r'''
static void reset(void) {
    attempts = saved = disconnected = monitored = authenticated = scan_count = 0;
    connected = successes_after = save_error = 0;
    session_error = TESLA_ERR_KEY_NOT_ON_WHITELIST;
    tick = notified_at = 0; g_revision = 1;
}
int main(void) {
    reset(); successes_after = 1;
    assert(pair_vehicle("VIN", 1) == ESP_OK);
    assert(notified_at == 0 && monitored == 1 && attempts == 1 && scan_count == 1);
    reset(); successes_after = 3;
    assert(pair_vehicle("VIN", 1) == ESP_OK);
    assert(notified_at == 4000 && saved == 1 && disconnected == 0 && scan_count == 1);
    reset();
    assert(pair_vehicle("VIN", 1) == TESLA_ERR_KEY_NOT_ON_WHITELIST);
    assert(tick == 60000 && saved == 0 && authenticated == 0 && disconnected == 1);
    reset(); session_error = ESP_ERR_TIMEOUT;
    assert(pair_vehicle("VIN", 1) == ESP_FAIL);
    assert(saved == 0 && tick == 60000 && authenticated == 0);
    reset(); successes_after = 1; save_error = 99;
    assert(pair_vehicle("VIN", 1) == TESLA_LINK_ERR_SAVE_FAILED);
    assert(saved == 0 && authenticated == 0 && disconnected == 1);
    reset(); g_revision = 2;
    assert(pair_vehicle("VIN", 1) == ESP_ERR_INVALID_STATE);
    assert(saved == 0 && attempts == 0 && authenticated == 0);
    puts("tesla_pairing_host_test: PASS (6 cases)");
}
'''
with tempfile.TemporaryDirectory(prefix="tesla-pairing-test-") as directory:
    directory = pathlib.Path(directory)
    test = directory / "test.c"
    test.write_text(harness)
    binary = directory / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
