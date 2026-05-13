#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "img_converters.h"
#include "esp_system.h"

static const char *TAG = "camera_httpd";
static uint8_t cameraCaptureFailCount = 0;
static constexpr uint8_t kMaxCaptureFailBeforeRestart = 5;

static esp_err_t image_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        cameraCaptureFailCount++;
        if (cameraCaptureFailCount >= kMaxCaptureFailBeforeRestart) {
            ESP_LOGE(TAG, "Too many capture failures, restarting");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        return httpd_resp_send_500(req);
    }

    cameraCaptureFailCount = 0;

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=image.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char ts[32];
    snprintf(ts, sizeof(ts), "%ld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
    httpd_resp_set_hdr(req, "X-Timestamp", ts);

    if (fb->format == PIXFORMAT_JPEG) {
        esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        return res;
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
    esp_camera_fb_return(fb);
    if (!converted) {
        ESP_LOGE(TAG, "JPEG conversion failed");
        cameraCaptureFailCount++;
        if (cameraCaptureFailCount >= kMaxCaptureFailBeforeRestart) {
            ESP_LOGE(TAG, "Too many conversion failures, restarting");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        return httpd_resp_send_500(req);
    }

    esp_err_t res = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);
    free(jpg_buf);
    return res;
}

void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 1;

    httpd_uri_t image_uri = {
        .uri = "/image.jpg",
        .method = HTTP_GET,
        .handler = image_handler,
        .user_ctx = NULL,
    };

    httpd_handle_t camera_httpd = NULL;
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &image_uri);
    } else {
        ESP_LOGE(TAG, "Web server start failed, restarting");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}
