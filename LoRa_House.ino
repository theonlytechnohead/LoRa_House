#include <SPI.h>
#include <LoRa.h>
#include "sha.h"

#include <WiFi.h>
#include <ESPmDNS.h>

#include "TimeLib.h"
#include <WebServer.h>
#include <ArduinoJson.h>

#include <SPIFFS.h>

#include <Update.h>

#include "SSD1306.h"
#include "images.h"

#include "soc/rtc_wdt.h" // FreeRTOS WDT control for ESP32

#define SCK 5   // GPIO5  -- SX1278's SCK
#define MISO 19 // GPIO19 -- SX1278's MISO
#define MOSI 27 // GPIO27 -- SX1278's MOSI
#define SS 18   // GPIO18 -- SX1278's CS
#define RST 14  // GPIO14 -- SX1278's RESET
#define DI0 26  // GPIO26 -- SX1278's IRQ(Interrupt Request)

#define BAND 915E6 // LoRa band, XXXE6, where XXX is MHz
#define SYNC_WORD 232 //HEX: 0xE8 // ranges from 0x00-0xFF, default 0x12, see API docs

#define KEEPHOME "KeepHome"
#define DEFAULT_SSID "KeepHome"
#define DEFAULT_PASSWORD ""

// Multitasking flags
TaskHandle_t webserverTask;
bool updating = false;
bool lora = true;
bool wifi = true;
bool wifi_ap_mode;

// Constants
const char* getSignalInfoCommand = "GET_SIGNAL_INFO";
const char* updateReadyCommand = "READY_UPDATE";

// Webserver
String ID;
String ssid = DEFAULT_SSID;
String password = DEFAULT_PASSWORD;
WebServer server(80);
const char* uploadPage = "<form method='POST' action='/update' enctype='multipart/form-data'> <input type='file' name='update'> <input type='submit' value='Update KeepHome'> </form> <br> <form method='GET' action='/updateRemote' enctype='multipart/form-data'> <input type='submit' value='Prepare KeepBox for update'> </form>";
IPAddress ip;

// NTP
WiFiUDP Udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets
static const char ntpServerName[] = "nz.pool.ntp.org";
//const int timeZone = 12; // UTC+12 (NZST)
const int timeZone = 13; // UTC+13 (NZDT)

// Various variables
unsigned int lastMillis = 0;
unsigned int interval = 5000;
unsigned int timeout = 10000;
unsigned long counter = 1;
unsigned int droppedPackets = 0;

// Display and LoRa
SSD1306 display(0x3c, 4, 15);
String packSize = "--";
const char* payloadReceived;
int rssi = 0;


// Multitasking function(s)
void handleWebClient (void* parameter) {
  for (;;) { // Infinite loop
    vTaskDelay(10); // ESP32 defaults to 100Hz tick rate, so 10ms delay allows for 1 tick in order to run background tasks
    //Web stuff
    if (wifi) {
      server.handleClient();
    }
  }
}

void displayBasic () {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  if (updating) {
    display.drawString(0, 0, "Applying update...");
  } else if (wifi) {
    display.drawString(0, 0, "http://" + ip.toString() + "/");
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    //display.drawString(64, 10, "http://" + ssid + "-" + ID + ".local");
    if (wifi_ap_mode) {
      display.drawString(64, 10, ssid + "-" + ID);
      display.drawString(64, 20, "p/w: " + password);
    }
    display.setTextAlignment(TEXT_ALIGN_LEFT);
  }
  
  if (lora) {
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    //display.drawString(64, 10, "Sending packets"); 
  }
}

// Displays LoRa data
void displayPayload () {
  displayBasic();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  if (lora) {
    display.drawString(0 , 25, payloadReceived);
  }

  display.display();
}

void displayMessage (String message) {
  displayBasic();

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64 , 35, message.c_str());

  display.display();
}

void displayWiFi () {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  //display.drawString(0, 0, "http:// " + ip.toString() + " /");
  display.drawString(0, 0, "http:// " + ssid + "-" + ID + ".local /");
  
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 15, ssid + "-" + ID);
  display.drawString(64, 30, "p/w: " + password);

  display.display();
}


// Callback for LoRa received
void receiveLoRaData(int packetSize) {
  payloadReceived = "";
  String packet;
  packSize = String(packetSize, DEC);
  for (int i = 0; i < packetSize; i++) {
    packet += (char) LoRa.read();
  }
  rssi = LoRa.packetRssi();
  //auto rssi = "RSSI: " + String(LoRa.packetRssi(), DEC);
  //auto snr = "SNR: " + String(LoRa.packetSnr(), 1);
  lastMillis = millis();
  
  DynamicJsonDocument doc(2048);

  DeserializationError err = deserializeJson(doc, packet);

  vTaskDelay(100);

  if (!err) {
    String checksum = doc["checksum"];
    auto command = doc["command"];
    payloadReceived = doc["payload"];

    String calc = SHA256(payloadReceived);

    if (checksum == calc) {
      counter++;
      if (command == getSignalInfoCommand) {
        displayMessage(payloadReceived);
        getSignalInfo();
      }
    } else {
      incrementDroppedPackets();
      displayMessage("Invalid checksum");
      getSignalInfo();
    }
  } else {
    incrementDroppedPackets();
    displayMessage("Couldn't deserialize!");
    getSignalInfo();
  }
}

// Send a JSON document over LoRa
void sendLoraJson (DynamicJsonDocument doc) {
  LoRa.idle();
  digitalWrite(2, HIGH);

  char output[measureJson(doc) + 1];
  serializeJson(doc, output, sizeof(output));
  //Serial.println(output);

  // send packet
  LoRa.beginPacket();
  LoRa.print(output);
  LoRa.endPacket();

  LoRa.receive();

  digitalWrite(2, LOW);
}

void getSignalInfo () {
  DynamicJsonDocument doc(1024);

  String payload = String(millis() / 1000) + " hello: " + counter; // BE CAREFUL YOU IDIOT
  
  String checksum = SHA256(payload);

  doc["checksum"] = checksum;
  doc["command"] = getSignalInfoCommand;
  doc["payload"] = payload;

  sendLoraJson(doc);
}


// Prepare remote unit for upgrade
void enableRemoteUpgrade () {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "Enable remote unit upgrade now...");
  lora = false;
  LoRa.idle();
  vTaskDelay(4000);
  DynamicJsonDocument doc(JSON_OBJECT_SIZE(3) + JSON_ARRAY_SIZE(1));
  String payload = "Enable wifi for update";
  
  String checksum = SHA256(payload);

  doc["checksum"] = checksum;
  doc["command"] = updateReadyCommand;
  doc["payload"] = payload;
  sendLoraJson(doc);
  lora = true;
}

// Long-term logging functions
void incrementReboots () {
  time_t t = now();
  String timeNow = "";
  timeNow += year(t);
  timeNow += "/";
  int months = month(t);
  if (months < 10) {
    timeNow += "0";
  }
  timeNow += months;
  timeNow += "/";
  int days = day(t);
  if (days < 10) {
    timeNow += "0";
  }
  timeNow += days;
  timeNow += " ";
  int hours = hour(t);
  if (hours < 10) {
    timeNow += "0";
  }
  timeNow += hours;
  timeNow += ":";
  int minutes = minute(t);
  if (minutes < 10) {
    timeNow += "0";
  }
  timeNow += minutes;
  timeNow += ":";
  int seconds = second(t);
  if (seconds < 10) {
    timeNow += "0";
  }
  timeNow += seconds;
  File loggingFile = SPIFFS.open("/log.txt", FILE_APPEND);
  loggingFile.println("Rebooted at " + timeNow);
  loggingFile.close();
}

void incrementDroppedPackets () {
  time_t t = now();
  String timeNow = "";
  timeNow += year(t);
  timeNow += "/";
  int months = month(t);
  if (months < 10) {
    timeNow += "0";
  }
  timeNow += months;
  timeNow += "/";
  int days = day(t);
  if (days < 10) {
    timeNow += "0";
  }
  timeNow += days;
  timeNow += " ";
  int hours = hour(t);
  if (hours < 10) {
    timeNow += "0";
  }
  timeNow += hours;
  timeNow += ":";
  int minutes = minute(t);
  if (minutes < 10) {
    timeNow += "0";
  }
  timeNow += minutes;
  timeNow += ":";
  int seconds = second(t);
  if (seconds < 10) {
    timeNow += "0";
  }
  timeNow += seconds;
  File loggingFile = SPIFFS.open("/log.txt", FILE_APPEND);
  loggingFile.println("Dropped packet at " + timeNow);
  loggingFile.close();
}

/*-------- NTP code ----------*/
const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

time_t getNtpTime () {
  IPAddress ntpServerIP; // NTP server's ip address

  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  Serial.println("Transmit NTP Request");
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.print(ntpServerName);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Receive NTP Response");
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.println("No NTP Response :-(");
  return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
void sendNTPpacket (IPAddress &address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

// HTTP POST, the actual point of the unit(s)
void handleRoot () {
  File loggingFile = SPIFFS.open("/log.txt", FILE_READ);
  String logFile = "\n";
  while (loggingFile.available()) {
    logFile += char(loggingFile.read());
  }
  loggingFile.close();
  
  server.send(200, "text/plain", "Hello world!    Log:" + logFile); // Send HTTP status 200 (Ok) and send some text to the browser/client
}

void handlePost () {
  Serial.println("Got POST request!");
  DynamicJsonDocument doc(1024); // Space for 1kb of JSON data in the heap

  doc["time"] = millis() / 1000;
  doc["additional"] = "RSSI: " + rssi;
  
  String data = server.arg("plain"); // Get the plain text from the post request (all the data)
  if (server.hasArg("keepbox")) {
    if (server.arg("keepbox") == "getdata") {
      // handle stuffs (l8r...)
    }
  }
  if (server.hasArg("WiFimode")) {
    if (server.arg("WiFimode") == "set") {
      // handle setting wifi mode
      // server.arg("WiFimode") // == 1 for AP, 0 for STA
      File wifi_ap_modeFile = SPIFFS.open("/wifi_ap_mode.txt", "w");
      wifi_ap_modeFile.print(server.arg("newWiFimode"));
      wifi_ap_modeFile.close();
    } else if (server.arg("WiFimode") == "get") {
      File wifi_ap_modeFile = SPIFFS.open("/wifi_ap_mode.txt", "r");
      String temp;
      temp += char(wifi_ap_modeFile.read());
      wifi_ap_modeFile.close();
      doc["WiFimode"] = temp;
    }
  }
  if (server.hasArg("SSID")) {
    if (server.arg("SSID") == "set") {
      // handle setting SSID to connect to / broadcast
      File ssidFile = SPIFFS.open("/ssid.txt", "w");
      ssidFile.print(server.arg("newWiFiSSID"));
      ssidFile.close();
    } else if (server.arg("SSID") == "get") {
      doc["SSID"] = ssid;
    }
  }
  if (server.hasArg("password")) {
    if (server.arg("password") == "set") {
      // handle setting password of network to connect to / broadcast
      File passwordFile = SPIFFS.open("/password.txt", "w");
      passwordFile.print(server.arg("newPassword"));
      passwordFile.close();
    } else if (server.arg("password") == "get") {
      File passwordFile = SPIFFS.open("/password.txt", "w");
      String temp = password;
      doc["password"] = temp;
    }
  }
  
  char output[measureJson(doc) + 1]; // output buffer, needs + 1 to fix off-by-one length error (for null-terminator)
  serializeJson(doc, output, sizeof(output)); // change the document into proper json, and put it into the output
  server.send(200, "text/json", output); // send the output to the client as json text
  Serial.println(output);
}

void setupMDNS (WiFiEvent_t event, WiFiEventInfo_t info) {
  String hostname = DEFAULT_SSID + ID;
  ip = WiFi.localIP();
  MDNS.begin(hostname.c_str());
  MDNS.addService("_http", "_tcp", 80);
  gotIP();
}

void setSoftAPIP () {
  ip = WiFi.softAPIP();
  gotIP();
}

// Setup webserver when connection is established
void gotIP () {
  String hostname = DEFAULT_SSID + ID;

  displayWiFi();

  server.on("/", handleRoot); // Call the 'handleRoot' function when a client requests URI "/"
  server.on("/post", HTTP_POST, handlePost); // Call the 'handlePost' function when a client sends a POST request to URI "/post"
  server.on("/updateRemote", enableRemoteUpgrade);
  server.on("/upload", HTTP_GET, [] () {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", uploadPage);
  });

  server.on("/update", HTTP_POST, [] () {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "Something went wrong" : "All done!");
    ESP.restart();
  }, []() {
    lora = false;
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      if (!Update.begin()) {
        //start with max available size
        Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        //Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        //true to set the size to the current progress
      } else {
        Update.printError(Serial);
      }
    } else {
      //Serial.printf("Update Failed Unexpectedly (likely broken connection): status=%d\n", upload.status);
    }
  });

  server.begin();

  vTaskDelay(5000);
  displayPayload();
  lora = true;
}

void setupWiFi () {
  WiFi.onEvent(setupMDNS, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
  //String temp = ssid + ID;
  WiFi.begin(ssid.c_str(), password.c_str());
}

// WiFi connection and setup
void setupSoftAP () {
  String temp = ssid + "-" + ID;
  WiFi.softAP(temp.c_str(), password.c_str());
  setSoftAPIP();
}

void displayInit() {
  pinMode(2, OUTPUT);
  pinMode(16, OUTPUT);
  digitalWrite(16, LOW);    // set GPIO16 low to reset OLED
  vTaskDelay(50);
  digitalWrite(16, HIGH); // while OLED is running, must set GPIO16 in high、

  vTaskDelay(500);

  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
}

/*
void loop () {
  vTaskDelay(10); // ESP32 default 100Hz tick rate, so 10ms delay allows for 1 tick in order to run background tasks
  Serial.println(millis());
}*/


void setup () {
  Serial.begin(115200);
  Serial.println("Booting...");

  randomSeed(analogRead(34)); // Random unconnected pin

  // OLED, LED
  pinMode(16, OUTPUT); // OLED RST
  pinMode(2, OUTPUT); // LED

  digitalWrite(16, LOW);    // set GPIO16 low to reset OLED
  vTaskDelay(50);
  digitalWrite(16, HIGH); // while OLED is running, must set GPIO16 in high

  // Initialize OLED display
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Booting...");
  display.display();

  vTaskDelay(50);

  pinMode(0, INPUT); // PRG button!

  if (!SPIFFS.begin()) {
    display.clear();
    display.drawString(0, 0, "SPIFFS borked");
    display.display();
    vTaskDelay(1000);
  } else {
    if (SPIFFS.exists("/id.txt")) {
      File idFile = SPIFFS.open("/id.txt", "r");
      ID = "";
      while (idFile.available()) {
        ID += char(idFile.read());
      }
      idFile.close();
    } else {
      File idFile = SPIFFS.open("/id.txt", "w");
      String temp;
      for (int i = 0; i < 3; i++) {
        temp += String(random(10));
      }
      ID = temp;
      idFile.print(temp);
      idFile.close();
    }
    display.clear();
    display.drawString(0, 0, "Setting up WiFi...");
    display.display();
    vTaskDelay(50);
    if (wifi) {
      if (SPIFFS.exists("/wifi_ap_mode.txt")) {
        File wifi_ap_modeFile = SPIFFS.open("/wifi_ap_mode.txt", "r");
        String temp;
        temp += char(wifi_ap_modeFile.read());
        if (temp == "1") {
          wifi_ap_mode = true;
        } else {
          wifi_ap_mode = false;
        }
        wifi_ap_modeFile.close();
      } else {
        File wifi_ap_modeFile = SPIFFS.open("/wifi_ap_mode.txt", "w");
        wifi_ap_modeFile.print("1");
        wifi_ap_mode = true;
        wifi_ap_modeFile.close();
      }
    }
    if (SPIFFS.exists("/ssid.txt")) {
      File ssidFile = SPIFFS.open("/ssid.txt", "r");
      ssid = "";
      while (ssidFile.available()) {
        ssid += char(ssidFile.read());
      }
      ssidFile.close();
    } else {
      File ssidFile = SPIFFS.open("/ssid.txt", "w");
      ssidFile.print(ssid);
      ssidFile.close();
    }
    if (SPIFFS.exists("/password.txt")) {
      File passwordFile = SPIFFS.open("/password.txt", "r");
      password = "";
      while (passwordFile.available()) {
        password += char(passwordFile.read());
      }
      passwordFile.close();
    } else {
      File passwordFile = SPIFFS.open("/password.txt", "w");
      password = "";
      for (int i = 0; i < 8; i++) {
        password += String(random(10));
      }
      passwordFile.print(password);
      passwordFile.close();
    }
    if (wifi_ap_mode) {
      setupSoftAP(); // Problem here!!! or hereafter
    } else {
      setupWiFi();
    }
  }

  display.clear();
  display.drawString(0, 0, "WiFi setup complete!");
  display.display();
  vTaskDelay(50);


  display.clear();
  display.drawString(0, 0, "Setting up LoRa...");
  display.display();
  vTaskDelay(50);
  
  // LoRa setup
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DI0);
  if (!LoRa.begin(BAND)) {
    // Failed to start LoRa
    display.clear();
    display.drawString(0, 0, "LoRa borked");
    display.display();
    while (1) {
      rtc_wdt_feed(); // satiate the WDT
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  } else {
    display.clear();
    display.drawString(0, 0, "LoRa setup complete!");
    display.display();
    vTaskDelay(50);
  }

  // Magic LoRa settings
  LoRa.sleep();
  LoRa.setSyncWord(SYNC_WORD);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(10);
  LoRa.setCodingRate4(8);
  LoRa.setSignalBandwidth(250E3);
  LoRa.idle();

  xTaskCreatePinnedToCore(
      handleWebClient, // Function to implement the task
      "webserverTask", // Name of the task
      8192,  // Stack size in words (causes stack overflow (DUH!!!!) if too low
      NULL,  // Task input parameter
      0,  // Priority of the task, 0 is lowest
      &webserverTask,  // Task handle
      0); // Core where the task should run, code runs on core 1 by default
  
  // Wait until everything is ready, then send first packet
  vTaskDelay(500);

  // NTP
  Udp.begin(localPort);
  setSyncProvider(getNtpTime);
  setSyncInterval(300); // Seconds between re-sync
  incrementReboots();
  
  lastMillis = millis();
  if (lora) {
    displayPayload();
    getSignalInfo();
  }
}

void loop () {
  vTaskDelay(10); // ESP32 default 100Hz tick rate, so 10ms delay allows for 1 tick in order to run background tasks
  if (!digitalRead(0) && !updating) {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 0, "Resetting KeepHome...");
    display.display();
    vTaskDelay(50);
    lora = false;
    wifi = false;
    vTaskDelay(50);
    SPIFFS.format();
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 0, "Reset complete!\nRestarting now...");
    display.display();
    vTaskDelay(1000);
    ESP.restart();
  }
  if (lora) {
    // Try to receive a packet
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      receiveLoRaData(packetSize);
    }

    if (millis() - lastMillis > interval) {
      payloadReceived = "";
      counter = 1;
      droppedPackets = droppedPackets + 1;
      incrementDroppedPackets();
      displayMessage("No response...");
    }

    // If it's been 10 seconds without a response, restart from the beginning and try again
    if (millis() - lastMillis > timeout) {
      payloadReceived = "";
      incrementDroppedPackets();
      displayMessage("No connection!");
      lastMillis = millis() - interval;
      getSignalInfo();
    }
  }
}
