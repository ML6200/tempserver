#ifndef CONFIG_H
#define CONFIG_H
// ===========================================================================
// Config
// ===========================================================================
#include <string>

struct Config
{
    std::string serial_port;
    int http_port = 8080;
    std::string log_file = "readings.csv";
    std::string hsrc = "dashboard.html";
    long long window_ms = 6LL * 3600 * 1000;

    double hum_crit_low = 30;
    double hum_warning_low = 40;
    double hum_optimal_low = 45;
    double hum_optimal_high = 55;
    double hum_warning_high = 60;
    double hum_crit_high = 70;

    double temp_crit_low = 16;
    double temp_warning_low = 18;
    double temp_optimal_low = 20;
    double temp_optimal_high = 24;
    double temp_warning_high = 33;
    double temp_crit_high = 34;

    // Fan control. The server computes a PWM duty (0-255) from the latest
    // temperature and sends "F<duty>\n" over serial; the Arduino applies it to
    // the fan pin. Off below fan_on_temp, ramps from fan_min_duty up to
    // fan_max_duty as temperature rises to fan_full_temp.
    bool fan_enabled = true;
    double fan_on_temp = 24;      // start cooling above this (deg C)
    double fan_full_temp = 35;    // full speed at/above this (deg C)
    int fan_min_duty = 100;       // minimum duty when on (overcome stiction)
    int fan_max_duty = 255;       // duty at/above fan_full_temp
    double fan_hysteresis = 1.0;  // deg C; turn-off point = fan_on_temp - this
};
#endif
