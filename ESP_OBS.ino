#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WiFiSettings.h>
#include <WebSocketsClient.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "html.h"
#include <Preferences.h>
#include <uri/UriRegex.h>


WebServer server(80);
WebSocketsClient webSocket;
Preferences preferences;
boolean connected = false;
unsigned long previousMillis = 0;
boolean ledState = false;
const int BUTTONS = 10;

const int buttonPins[BUTTONS] = {15, 13, 18, 14, 19, 27, 22, 26, 35, 34};
const int ledPins[BUTTONS] = {2, 12, 23, 25, 16, 33, 17, 32, 5, 21};
const int jsonSize = 50*1024;
String scenes[BUTTONS];
String serverIndex;
// Variables will change:
int scene = 0;
int buttonState[BUTTONS];             // the current reading from the input pin
int lastButtonState[BUTTONS];   // the previous reading from the input pin

// the following variables are unsigned longs because the time, measured in
// milliseconds, will quickly become a bigger number than can be stored in an int.
unsigned long lastDebounceTime[BUTTONS];  // the last time the output pin was toggled
unsigned long debounceDelay = 50;    // the debounce time; increase if the output flickers

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  DynamicJsonDocument doc(jsonSize);
  DeserializationError error;
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WSc] Disconnected!\n");
      connected = false;
      break;
    case WStype_CONNECTED:
      Serial.printf("[WSc] Connected to url: %s\n", payload);
      connected = true;
      scene=0;
      setLeds();
      break;
    case WStype_TEXT:
      Serial.printf("[WSc] get text: %s\n", payload);
      error = deserializeJson(doc, payload, jsonSize);
      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        break;
      }
      if (! doc["scenes"].isNull()) {
        for (int i = 0; i < BUTTONS; i++) {
          if (i < doc["scenes"].size()) {
            const char* name = doc["scenes"][i]["name"];
            scenes[i] = String(name);
          } else {
            scenes[i] = String("");
          }
        }
        buildIndex();
      } else if (!doc["scene-name"].isNull()) {
        for (byte scenenum = 0; scenenum < BUTTONS; scenenum++) {
          if (scenes[scenenum] == doc["scene-name"]) {
            scene = scenenum + 1;
            setLeds();
            continue;
          }
        }
      }
      break;
    case WStype_BIN:
      Serial.printf("[WSc] get binary length: %u\n", length);
      break;
    case WStype_ERROR:
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      break;
  }

}

void setLeds() {
  for (byte pin = 0; pin < BUTTONS; pin++) {
    if (pin == (scene - 1)) {
      digitalWrite(ledPins[pin], HIGH);
      Serial.println(ledPins[pin]);
    } else {
      digitalWrite(ledPins[pin], LOW);
    }
  }
}

void buildIndex() {
  serverIndex = index_html_head;
  for (int i = 0; i < BUTTONS; i++) {
    serverIndex.concat("      <input type=\"radio\" name=\"scene\" onchange=\"toggleCheckbox(this)\" id=\"output");
    serverIndex.concat(i + 1);
    serverIndex.concat("\" value=\"");
    serverIndex.concat(scenes[i]);
    serverIndex.concat("\">");
    serverIndex.concat(scenes[i]);
    serverIndex.concat("<br>\n");
  }
  serverIndex.concat(index_html_tail);
}


void alert_connect() {
  int lastPin = 9;
  for (int i = 0; i < BUTTONS; i = i++) {
    digitalWrite(ledPins[i], HIGH);
    digitalWrite(ledPins[lastPin], LOW);
    delay(500);
    lastPin = i;
  }
  for (int i = 0; i < BUTTONS; i = i++) {
    digitalWrite(ledPins[i], LOW);
  }
}


/*
   setup function
*/
void setup(void) {
  Serial.begin(115200);
  SPIFFS.begin(true);  // Will format on the first run after failing to mount
  for (byte pin = 0; pin < BUTTONS; pin++) {
    pinMode(buttonPins[pin], INPUT_PULLDOWN);
    buttonState[pin] = LOW;
    lastButtonState[pin] = LOW;
    lastDebounceTime[pin] = 0;
    scenes[pin] = String("");
    pinMode(ledPins[pin], OUTPUT);
    digitalWrite(ledPins[pin], LOW);
  }

  // Connect to WiFi with a timeout of 30 seconds
  // Launches the portal if the connection failed
  WiFiSettings.connect(true, 30);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(WiFi.SSID());
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  IPAddress nullIp(255,255,255,255);
  if (WiFi.localIP() == nullIp) {
    WiFiSettings.portal();
  }

  /*use mdns for host name resolution*/
  if (!MDNS.begin("obscontrol")) { //http://obscontrol.local
    Serial.println("Error setting up MDNS responder!");
    while (1) {
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");
  buildIndex();
  /*return index page which is stored in serverIndex */
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex.c_str());
  });
  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", updateIndex);
  });
  server.on("/state", HTTP_GET, []() {
    server.send(200, "text/plain", String(scene).c_str());
  });
  server.on("/reset", HTTP_GET, []() {
    server.send(200, "text/plain", "Resetting");
    SPIFFS.format();
    ESP.restart();
  });
  server.on(UriRegex("^\\/set\\/([^:]*):([0-9]+)$"), []() {
    String host = server.pathArg(0);
    String port = server.pathArg(1);

    Serial.println("Host: '" + host + "' and Port: '" + port + "'");
    preferences.begin("obscontrol", false);
    preferences.putString( "server_host", host);
    preferences.putInt("server_port", port.toInt());
    preferences.end();
    startWS();
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex.c_str());
  });

  /*handling uploading firmware file */
  server.on("/upload", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  server.begin();
  startWS();
}

void startWS() {
  preferences.begin("obscontrol", false);
  String host = preferences.getString( "server_host", "default.example.org");
  int    port = preferences.getInt("server_port", 4444);
  preferences.end();
  Serial.printf("Connecting to OBS @ %s:%d\n", host, port);
  webSocket.begin(host, port, "/");
  // event handler
  webSocket.onEvent(webSocketEvent);
  // try ever 5000 again if connection has failed
  webSocket.setReconnectInterval(5000);
  webSocket.sendTXT("{\"request-type\": \"GetSceneList\", \"message-id\": \"esp\"}");
}

void loop(void) {
  server.handleClient();
  webSocket.loop();
  if (connected) {
    // read the state of the switch into a local variable:
    int newScene = 0;
    for (byte pin = 0; pin < BUTTONS; pin++) {
      int reading = digitalRead(buttonPins[pin]);

      if (reading != lastButtonState[pin]) {
        // reset the debouncing timer
        lastDebounceTime[pin] = millis();
      }

      if ((millis() - lastDebounceTime[pin]) > debounceDelay) {
        if (reading != buttonState[pin]) {
          buttonState[pin] = reading;
          if (buttonState[pin] == HIGH) {
            newScene = pin + 1;
          }
        }
      }
      // save the reading. Next time through the loop, it'll be the lastButtonState:
      lastButtonState[pin] = reading;
    }
    if (newScene != 0) {
      scene = newScene;
      Serial.println(scene);
      setLeds();
      String payload = "{\"request-type\": \"SetCurrentScene\", \"message-id\": \"esp\", \"scene-name\": \"";
      payload.concat( scenes[scene - 1]);
      payload.concat("\"}");
      Serial.println(payload);
      webSocket.sendTXT(payload);
      webSocket.sendTXT("{\"request-type\": \"GetSceneList\", \"message-id\": \"esp\"}");

    }
  } else {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= 500) {
      previousMillis = currentMillis;
      ledState=!ledState;
      for (int i = 0; i < BUTTONS; i = i + 2) {
        digitalWrite(ledPins[i], ledState);
        digitalWrite(ledPins[i + 1], !ledState);
      }
    }
  }
}
