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
};
#endif
