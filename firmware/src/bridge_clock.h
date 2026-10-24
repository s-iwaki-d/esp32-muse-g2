#pragma once

#include <stdbool.h>

/* Bridge task only. Starts/retries SNTP after Wi-Fi connects and returns true
 * only after this boot has received network time and the clock is valid. */
bool bridge_clock_ready(bool network_ready);
