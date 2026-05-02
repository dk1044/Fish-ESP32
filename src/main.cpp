#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <ESPmDNS.h>

bool streamEnabled = false;
unsigned long streamEnableTime = 0;
#define STREAM_DELAY_MS 180000 // 3 minutes

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define LIGHT_GPIO_NUM     4

const char* ssid = "SKY67NSU";
const char* password = "Add Password";

AsyncWebServer server(80);
WiFiServer streamServer(81);

void otaTask(void *pvParameters) {
  for (;;) {
    ElegantOTA.loop();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
void streamTask(void *pvParameters) {
  WiFiClient client = *((WiFiClient*)pvParameters);
  delete (WiFiClient*)pvParameters;

  client.print(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: close\r\n"
    "\r\n"
  );

  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    client.printf(
      "--frame\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: %u\r\n"
      "\r\n",
      fb->len
    );

    size_t sent = 0;
    while (sent < fb->len) {
      size_t toSend = min((size_t)1024, fb->len - sent);
      client.write(fb->buf + sent, toSend);
      sent += toSend;
      vTaskDelay(1);
    }

    client.print("\r\n");
    esp_camera_fb_return(fb);
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }

  client.stop();
  vTaskDelete(NULL);
}

void setupRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html",
      "<h1>ESP32-CAM</h1>"
      "<p><a href='http://192.168.1.184:81'>View Stream</a></p>"
      "<p><a href='/light/on'>Light ON</a></p>"
      "<p><a href='/light/off'>Light OFF</a></p>"
      "<p><a href='/update'>OTA Update</a></p>"
      "<p><a href='/restart'>Restart ESP32</a></p>"
    );
  });

  server.on("/light/on", HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(LIGHT_GPIO_NUM, HIGH);
    request->send(200, "text/plain", "Light ON");
  });

  server.on("/light/off", HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(LIGHT_GPIO_NUM, LOW);
    request->send(200, "text/plain", "Light OFF");
  });

    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "Restarting... OTA window open for 3 minutes at http://192.168.1.184/update");
    delay(500);
    ESP.restart();
  });
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32-CAM Booting...");

  delay(500);
  pinMode(LIGHT_GPIO_NUM, OUTPUT);
  digitalWrite(LIGHT_GPIO_NUM, LOW);

  camera_config_t config;
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
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 10;
  config.fb_count = 2;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed");
    return;
  }

  IPAddress local_IP(192, 168, 1, 184);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress primaryDNS(8, 8, 8, 8);
  IPAddress secondaryDNS(8, 8, 4, 4);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Static IP failed to configure");
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  WiFi.setSleep(false);
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  if (!MDNS.begin("esp32cam")) {
    Serial.println("Error starting mDNS");
  } else {
    Serial.println("mDNS started: http://esp32cam.local");
  }

  setupRoutes();
  ElegantOTA.begin(&server);
  xTaskCreatePinnedToCore(
  otaTask,
  "ota",
  8192,
  NULL,
  1,
  NULL,
  0  // core 0
);
  server.begin();
  streamServer.begin();
  streamEnableTime = millis() + STREAM_DELAY_MS;
  Serial.println("Stream paused for 3 minutes to allow OTA...");
  Serial.println("Stream at http://192.168.1.184:81/");
  Serial.println("OTA at http://192.168.1.184/update");
  Serial.println("Server Ready");
}

void loop() {
  if (!streamEnabled && millis() > streamEnableTime) {
    streamEnabled = true;
    Serial.println("Stream enabled");
  }

  if (streamEnabled) {
    WiFiClient client = streamServer.accept();
    if (client) {
      WiFiClient *clientPtr = new WiFiClient(client);
      xTaskCreatePinnedToCore(
        streamTask,
        "stream",
        8192,
        clientPtr,
        1,
        NULL,
        1
      );
    }
  }
}