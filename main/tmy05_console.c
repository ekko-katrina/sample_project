#include "tmy05_console.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "myble.h"

#define TAG "FRIDGE-CONSOLE"
#define CONSOLE_PROMPT "fridge>"

static const ble_sim_profile_info_t *active_profile(void)
{
    return ble_sim_profile_info(ble_sim_get_active_profile());
}

static const char *power_text(tmy05_power_t power)
{
    return power == TMY05_POWER_ON ? "开机" : "关机";
}

static const char *unit_text(tmy05_temperature_unit_t unit)
{
    return unit == TMY05_UNIT_FAHRENHEIT ? "华氏度" : "摄氏度";
}

static const char *mode_text(tmy05_mode_t mode)
{
    switch (mode) {
    case TMY05_MODE_COOLING:
        return active_profile()->has_child_lock ? "制冷" : "冷藏";
    case TMY05_MODE_MAX:
        return "MAX";
    case TMY05_MODE_ECO:
        return "ECO";
    case TMY05_MODE_HEATING:
        return "制热";
    default:
        return "未知";
    }
}

static const char *lock_text(tmy05_child_lock_t child_lock)
{
    return child_lock == TMY05_CHILD_LOCK_ON ? "上锁" : "解锁";
}

static const char *fault_text(uint8_t fault_code)
{
    if (!active_profile()->has_child_lock && fault_code <= 0x07U) {
        static const char *const errors[] = {
            "无故障", "Er0", "Er1", "Er2", "Er3", "Er4", "Er5", "Er6"
        };
        return errors[fault_code];
    }

    switch (fault_code) {
    case TMY05_FAULT_NONE:
        return "无故障";
    case TMY05_FAULT_NTC_SHORT:
        return "NTC短路";
    case TMY05_FAULT_NTC_OPEN:
        return "NTC开路";
    case TMY05_FAULT_HIGH_TEMPERATURE:
        return "75度高温保护";
    default:
        return "未知故障";
    }
}

static void print_state(const char *prefix, const tmy05_state_t *state)
{
    const ble_sim_profile_info_t *profile = active_profile();
    printf("%s档位=%s，电源=%s，单位=%s，模式=%s，",
           prefix, profile->display_name, power_text(state->power),
           unit_text(state->unit), mode_text(state->mode));
    if (profile->has_child_lock) {
        printf("童锁=%s，", lock_text(state->child_lock));
    }
    printf("目标温度=%d，实际温度=%d，", state->target_temperature,
           state->actual_temperature);
    if (profile->has_voltage) {
        printf("实际电压=%u.%02uV，",
               state->actual_voltage_centi_volts / 100U,
               state->actual_voltage_centi_volts % 100U);
    }
    printf("故障=%s(%u)\n", fault_text(state->fault_code), state->fault_code);
}

static bool get_state(tmy05_state_t *state)
{
    if (!ble_tmy05_get_state(state)) {
        ESP_LOGE(TAG, "读取冰箱状态失败");
        return false;
    }
    return true;
}

static int apply_body_operation(const char *description, tmy05_state_t *state)
{
    if (!ble_tmy05_set_state(state)) {
        ESP_LOGE(TAG, "状态参数无效，未执行：%s", description);
        return 1;
    }

    ESP_LOGI(TAG, "模拟机身操作：%s", description);
    print_state("当前状态：", state);
    return 0;
}

static bool parse_int16(const char *text, int16_t *value)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < INT16_MIN || parsed > INT16_MAX) {
        return false;
    }

    *value = (int16_t)parsed;
    return true;
}

static int status_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    tmy05_state_t state;
    if (!get_state(&state)) {
        return 1;
    }
    print_state("当前状态：", &state);
    return 0;
}

static int power_command(int argc, char **argv)
{
    if (argc != 2) {
        printf("用法：power on|off|toggle\n");
        return 1;
    }

    tmy05_state_t state;
    if (!get_state(&state)) {
        return 1;
    }

    if (strcmp(argv[1], "on") == 0) {
        state.power = TMY05_POWER_ON;
    } else if (strcmp(argv[1], "off") == 0) {
        state.power = TMY05_POWER_OFF;
    } else if (strcmp(argv[1], "toggle") == 0) {
        state.power = state.power == TMY05_POWER_ON ? TMY05_POWER_OFF : TMY05_POWER_ON;
    } else {
        printf("用法：power on|off|toggle\n");
        return 1;
    }

    return apply_body_operation(power_text(state.power), &state);
}

static int lock_command(int argc, char **argv)
{
    if (!active_profile()->has_child_lock) {
        printf("当前 %s 协议没有童锁字段\n", active_profile()->display_name);
        return 1;
    }
    if (argc != 2) {
        printf("用法：lock on|off|toggle\n");
        return 1;
    }

    tmy05_state_t state;
    if (!get_state(&state)) {
        return 1;
    }

    if (strcmp(argv[1], "on") == 0) {
        state.child_lock = TMY05_CHILD_LOCK_ON;
    } else if (strcmp(argv[1], "off") == 0) {
        state.child_lock = TMY05_CHILD_LOCK_OFF;
    } else if (strcmp(argv[1], "toggle") == 0) {
        state.child_lock = state.child_lock == TMY05_CHILD_LOCK_ON
                               ? TMY05_CHILD_LOCK_OFF
                               : TMY05_CHILD_LOCK_ON;
    } else {
        printf("用法：lock on|off|toggle\n");
        return 1;
    }

    return apply_body_operation(lock_text(state.child_lock), &state);
}

static int mode_command(int argc, char **argv)
{
    if (argc != 2) {
        printf("用法：mode cool|max|eco|heat|toggle\n");
        return 1;
    }

    tmy05_state_t state;
    if (!get_state(&state)) {
        return 1;
    }

    if (strcmp(argv[1], "cool") == 0) {
        state.mode = TMY05_MODE_COOLING;
    } else if (strcmp(argv[1], "max") == 0) {
        state.mode = TMY05_MODE_MAX;
    } else if (strcmp(argv[1], "eco") == 0) {
        state.mode = TMY05_MODE_ECO;
    } else if (strcmp(argv[1], "heat") == 0) {
        state.mode = TMY05_MODE_HEATING;
    } else if (strcmp(argv[1], "toggle") == 0) {
        uint8_t candidate = (uint8_t)state.mode;
        do {
            candidate = (candidate + 1U) % ((uint8_t)TMY05_MODE_HEATING + 1U);
        } while ((active_profile()->supported_mode_mask & (1U << candidate)) == 0U);
        state.mode = (tmy05_mode_t)candidate;
    } else {
        printf("用法：mode cool|max|eco|heat|toggle\n");
        return 1;
    }

    if ((active_profile()->supported_mode_mask & (1U << state.mode)) == 0U) {
        printf("%s 不支持 %s 模式\n",
               active_profile()->display_name, mode_text(state.mode));
        return 1;
    }

    char description[32];
    snprintf(description, sizeof(description), "切换为%s模式", mode_text(state.mode));
    return apply_body_operation(description, &state);
}

static int unit_command(int argc, char **argv)
{
    if (argc != 2) {
        printf("用法：unit c|f|toggle\n");
        return 1;
    }

    tmy05_state_t state;
    if (!get_state(&state)) {
        return 1;
    }

    if (strcmp(argv[1], "c") == 0) {
        state.unit = TMY05_UNIT_CELSIUS;
    } else if (strcmp(argv[1], "f") == 0) {
        state.unit = TMY05_UNIT_FAHRENHEIT;
    } else if (strcmp(argv[1], "toggle") == 0) {
        state.unit = state.unit == TMY05_UNIT_CELSIUS
                         ? TMY05_UNIT_FAHRENHEIT
                         : TMY05_UNIT_CELSIUS;
    } else {
        printf("用法：unit c|f|toggle\n");
        return 1;
    }

    char description[32];
    snprintf(description, sizeof(description), "温度单位改为%s", unit_text(state.unit));
    return apply_body_operation(description, &state);
}

static int temperature_command(int argc, char **argv, bool actual)
{
    if (argc != 2) {
        printf("用法：%s <有符号整数>\n", actual ? "actual" : "target");
        return 1;
    }

    int16_t temperature;
    if (!parse_int16(argv[1], &temperature)) {
        printf("温度必须是 -32768 到 32767 之间的整数\n");
        return 1;
    }

    tmy05_state_t state;
    if (!get_state(&state)) {
        return 1;
    }

    if (actual) {
        state.actual_temperature = temperature;
    } else {
        state.target_temperature = temperature;
    }

    char description[48];
    snprintf(description,
             sizeof(description),
             "%s温度改为%d",
             actual ? "实际" : "目标",
             temperature);
    return apply_body_operation(description, &state);
}

static int target_command(int argc, char **argv)
{
    return temperature_command(argc, argv, false);
}

static int actual_command(int argc, char **argv)
{
    return temperature_command(argc, argv, true);
}

static int fault_command(int argc, char **argv)
{
    if (argc != 2) {
        printf("用法：fault 0|1|2|3\n");
        return 1;
    }

    char *end = NULL;
    long fault = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || fault < TMY05_FAULT_NONE ||
        fault > active_profile()->maximum_fault_code) {
        printf("当前档位故障码范围：0 到 %u\n",
               active_profile()->maximum_fault_code);
        return 1;
    }

    tmy05_state_t state;
    if (!get_state(&state)) {
        return 1;
    }
    state.fault_code = (uint8_t)fault;

    char description[48];
    snprintf(description, sizeof(description), "故障状态改为%s", fault_text(state.fault_code));
    return apply_body_operation(description, &state);
}

static int voltage_command(int argc, char **argv)
{
    if (!active_profile()->has_voltage) {
        printf("当前 %s 状态帧没有电压字段\n", active_profile()->display_name);
        return 1;
    }
    if (argc != 2) {
        printf("用法：voltage <百分之一伏数值，例如 1265>\n");
        return 1;
    }

    char *end = NULL;
    errno = 0;
    unsigned long voltage = strtoul(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || voltage > UINT16_MAX) {
        printf("电压传输值必须是 0 到 65535 的整数，例如 12.65V 输入 1265\n");
        return 1;
    }

    tmy05_state_t state;
    if (!get_state(&state)) {
        return 1;
    }
    state.actual_voltage_centi_volts = (uint16_t)voltage;

    char description[48];
    snprintf(description, sizeof(description), "实际电压改为%lu.%02luV",
             voltage / 100UL, voltage % 100UL);
    return apply_body_operation(description, &state);
}

static void register_command(const char *name,
                             const char *help,
                             const char *hint,
                             esp_console_cmd_func_t function)
{
    const esp_console_cmd_t command = {
        .command = name,
        .help = help,
        .hint = hint,
        .func = function,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&command));
}

void tmy05_console_init(void)
{
    esp_console_register_help_command();
    register_command("status", "显示当前冰箱完整状态", NULL, status_command);
    register_command("power", "模拟机身开关机", "<on|off|toggle>", power_command);
    register_command("lock", "模拟机身童锁操作", "<on|off|toggle>", lock_command);
    register_command("mode", "模拟切换工作模式", "<cool|max|eco|heat|toggle>", mode_command);
    register_command("unit", "模拟切换摄氏/华氏", "<c|f|toggle>", unit_command);
    register_command("target", "修改目标温度", "<temperature>", target_command);
    register_command("actual", "修改实际温度", "<temperature>", actual_command);
    register_command("fault", "模拟故障码", "<0..7，取决于当前型号>", fault_command);
    register_command("voltage", "修改实际电压传输值（V x 100）", "<centivolts>", voltage_command);

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = CONSOLE_PROMPT;
    repl_config.max_cmdline_length = 128;

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "机身模拟控制台已启动，输入 help 查看命令");
}
