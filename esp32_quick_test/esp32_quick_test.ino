#include <WiFi.h>
#include <WebServer.h>

const char* AP_SSID     = "ESP32-RC-CAR";
const char* AP_PASSWORD = "password123";

WebServer server(80);

void handleRoot() {
  server.send(200, "text/html", "<h1>🏎️ ESP32 Test Page is Working!</h1><p>Wi-Fi and Web Server are operational.</p>");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- ESP32 Quick Wi-Fi Test ---");

  pinMode(2, OUTPUT);
  digitalWrite(2, HIGH); // Turn on built-in blue LED

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.print("SoftAP SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Web server ready at: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  server.handleClient();
  delay(2);
}

