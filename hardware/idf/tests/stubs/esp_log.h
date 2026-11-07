#pragma once
// Host-test stub: the bridge's logging is not what the split test is about.
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...)  do { (void)(tag); } while (0)
#define ESP_LOGW(tag, fmt, ...)  do { (void)(tag); } while (0)
#define ESP_LOGE(tag, fmt, ...)  do { (void)(tag); } while (0)
