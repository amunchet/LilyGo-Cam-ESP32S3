/**
 * @file      main.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2022  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2022-09-16
 *
 */
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "esp_camera.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include <secrets.h>

#if (ESP_ARDUINO_VERSION)  > ESP_ARDUINO_VERSION_VAL(3,0,0)
#error "Please use ESP32 core version lower than V 3.0.0, 2.0.17 is recommended"
#endif

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
#include "utilities.h"


void        startCameraServer();

XPowersPMU  PMU;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, U8X8_PIN_NONE);

static constexpr framesize_t kCameraFrameSize = FRAMESIZE_UXGA;
static constexpr pixformat_t  kCameraPixelFormat = PIXFORMAT_JPEG;
static constexpr int          kCameraXclkFreqHz = 20000000;
static constexpr int          kCameraJpegQuality = 12;
static constexpr int          kCameraFrameBufferCount = 1;
static constexpr uint32_t     kWatchdogTimeoutSeconds = 20;
static constexpr uint32_t     kWifiLossRestartTimeoutMs = 30000;
static constexpr uint32_t     kCameraHealthCheckIntervalMs = 15000;
static constexpr uint8_t      kMaxCameraHealthFailures = 3;

static bool screenReady = false;
static uint32_t lastWifiOkMs = 0;
static uint32_t lastCameraHealthCheckMs = 0;
static uint8_t consecutiveCameraHealthFailures = 0;

static void restartNow(const char *reason)
{
    Serial.printf("Restart requested: %s\n", reason);
    if (screenReady) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.setFontPosTop();
        u8g2.drawStr(0, 0, "Restarting...");
        u8g2.drawUTF8(0, 16, reason);
        u8g2.sendBuffer();
    }
    delay(300);
    ESP.restart();
}

static void setupWatchdog()
{
    if (esp_task_wdt_init(kWatchdogTimeoutSeconds, true) != ESP_OK) {
        restartNow("WDT init failed");
    }
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        restartNow("WDT add failed");
    }
}

static void drawNetworkScreen()
{
    if (!WiFi.isConnected()) {
        return;
    }

    const String ssid = WiFi.SSID();
    const String ipAddress = WiFi.localIP().toString();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_8x13_mf);
    u8g2.setFontPosTop();
    u8g2.drawStr(0, 0, "AP:");
    u8g2.drawUTF8(28, 0, ssid.c_str());
    u8g2.drawUTF8(0, 24, ipAddress.c_str());
    u8g2.sendBuffer();
}



void setup()
{

    Serial.begin(115200);
    setupWatchdog();

    // Avoid boot deadlock if no USB serial monitor is attached.
    uint32_t serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 2000) {
        delay(10);
        esp_task_wdt_reset();
    }

    delay(3000);

    Serial.println();

    /*********************************
     *  step 1 : Initialize power chip,
     *  turn on camera power channel
    ***********************************/
    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
        Serial.println("Failed to initialize power.....");
        restartNow("PMU init failed");
    }
    //Set the working voltage of the camera, please do not modify the parameters
    PMU.setALDO1Voltage(1800);  // CAM DVDD  1500~1800
    PMU.enableALDO1();
    PMU.setALDO2Voltage(2800);  // CAM DVDD 2500~2800
    PMU.enableALDO2();
    PMU.setALDO4Voltage(3000);  // CAM AVDD 2800~3000
    PMU.enableALDO4();

    // TS Pin detection must be disable, otherwise it cannot be charged
    PMU.disableTSPinMeasure();

    Wire.begin(I2C_SDA, I2C_SCL);
    u8g2.begin();
    screenReady = true;
    u8g2.setFlipMode(0);
    drawNetworkScreen();


    /*********************************
     * step 2 : start network in station mode
    ***********************************/
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID1, WIFI_SSID_PASSWORD1);

    Serial.printf("Connecting to %s", WIFI_SSID1);
    unsigned long connectStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        esp_task_wdt_reset();
        Serial.print(".");
        if (millis() - connectStart > 20000) {
            Serial.println();
            Serial.println("WiFi connection failed");
            restartNow("WiFi connect timeout");
        }
    }

    Serial.println();
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    lastWifiOkMs = millis();
    drawNetworkScreen();



    /*********************************
     *  step 3 : Initialize camera
    ***********************************/
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = kCameraXclkFreqHz;
    config.frame_size = kCameraFrameSize;
    config.pixel_format = kCameraPixelFormat;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = kCameraJpegQuality;
    config.fb_count = kCameraFrameBufferCount;

    if (config.pixel_format == PIXFORMAT_JPEG && psramFound()) {
        config.fb_count = 2;
        config.grab_mode = CAMERA_GRAB_LATEST;
    } else if (!psramFound()) {
        config.frame_size = FRAMESIZE_SVGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
    }

    // camera init
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x Please check if the camera is connected well.", err);
        restartNow("Camera init failed");
    }

    sensor_t *s = esp_camera_sensor_get();
    // initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1); // flip it back
        s->set_brightness(s, 1); // up the brightness just a bit
        s->set_saturation(s, -2); // lower the saturation
    }
#if defined(LILYGO_ESP32S3_CAM_PIR_VOICE)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
#endif



    /*********************************
     *  step 4 : start camera web server
    ***********************************/
    startCameraServer();

}

void loop()
{
    esp_task_wdt_reset();

    if (WiFi.status() == WL_CONNECTED) {
        lastWifiOkMs = millis();
    } else if (millis() - lastWifiOkMs > kWifiLossRestartTimeoutMs) {
        restartNow("WiFi lost");
    }

    if (millis() - lastCameraHealthCheckMs > kCameraHealthCheckIntervalMs) {
        lastCameraHealthCheckMs = millis();
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            consecutiveCameraHealthFailures++;
            if (consecutiveCameraHealthFailures >= kMaxCameraHealthFailures) {
                restartNow("Camera health failed");
            }
        } else {
            consecutiveCameraHealthFailures = 0;
            esp_camera_fb_return(fb);
        }
    }

    drawNetworkScreen();
    delay(1000);
}
