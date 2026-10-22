#ifndef LHDC_DONGLE_LOG_H
#define LHDC_DONGLE_LOG_H
// No formatting or blocking logging from the codec worker.
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#endif
