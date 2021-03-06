/* ------------------ RFID ESP Door lock ---------------
   
   wiring the MFRC522 to ESP8266 (ESP-12)
	RST     = GPIO5
	SDA(SS) = GPIO4 
	MOSI    = GPIO13
	MISO    = GPIO12
	SCK     = GPIO14
	GND     = GND
	3.3V    = 3.3V
   ----------------------------------------------------- */

/* ------------------ Includes ------------------------- */
#include <ESP8266WiFi.h>		// Base of the whole project
#include <ESP8266mDNS.h>        // Zero-config Library (Bonjour, Avahi) http://esp-rfid.local
#include <WiFiUdp.h>            // Library for manipulating UDP packets which is used by NTP Client to get Timestamps
#include <NTPClient.h>          // To timestamp RFID scans we get Unix Time from NTP Server
#include <SPI.h>				// SPI library to communicate with the peripherals as well as SPIFFS
#include <MFRC522.h>			// RFID library
#include <FS.h>					// SPIFFS Library for access to the onboard storage
#include <ArduinoJson.h>		// JSON Library for Encoding and Parsing Json object to send browser. We do that because Javascript has built-in JSON parsing.
#include <ESPAsyncTCP.h>		// Async TCP Library is mandatory for Async Web Server
#include <ESPAsyncWebServer.h>	// Async Web Server with built-in WebSocket Plug-in
#include <SPIFFSEditor.h>		// This creates a web page on server which can be used to edit text based files.
#include <TimeLib.h>			// Library for converting epochtime to a date
#include <SD.h>					// SD card library for storing the log

/* ------------------ Definitions ---------------------- */
#define MFRC522_SS 15
#define SD_SS 2
#define relayPin 5
#define greedLed 5
#define redLed 4
#define blueLed 0

/* ------------------ Object declarations -------------- */
MFRC522 mfrc522 = MFRC522(); // Create MFRC522 instance
// Create AsyncWebServer instance on port "80"
AsyncWebServer server(80);
// Create WebSocket instance on URL "/ws"
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

// Create UDP instance for NTP Client
WiFiUDP ntpUDP;

// Create NTP Client instance
NTPClient timeClient(ntpUDP);

/* ------------------ Variables ------------------------ */
String admin_pass;
const char *auth_pass;

// Variables for whole scope
String filename = "/P/";
//flag to use from web update to reboot the ESP
bool shouldReboot = false;

bool SDAvailable = false;
bool SDMutex = false;
bool denyAcc = false;
bool activateRelay = false;
unsigned long previousMillis = 0;
int activateTime;

String dateTimeStamp;

extern "C" uint32_t _SPIFFS_start;
extern "C" uint32_t _SPIFFS_end;

/* ------------------ Setup ---------------------------- */
void setup() {
	Serial.begin(115200);    // Initialize serial communications
	Serial.println();
  	Serial.println(F("[ INFO ] ESP RFID v0.1"));

  	// Start SPIFFS filesystem
  	SPIFFS.begin();

  	pinMode(relayPin, OUTPUT);
  	pinMode(redLed, OUTPUT);
  	pinMode(blueLed, OUTPUT);
  	digitalWrite(relayPin, LOW);

  	if (loadConfiguration()) Serial.println(F("[ INFO ] Loaded configuration"));
  	else Serial.println(F("[ WARN ] Failed to load configuration"));

  	// Start NTP Client
  	timeClient.setTimeOffset(2 * 3600); //Timezone offset
  	timeClient.begin();

	setupWebserver();
	if (!SD.begin(SD_SS)) {
		SDAvailable = false;
		Serial.println(F("[ INFO ] SD Card failed to connect or not present"));
	}
	else {
		SDAvailable = true;
		Serial.println(F("[ INFO ] SD Card initialized"));
	}
	turnOnLed(blueLed);  	
}

/* ------------------ Main loop ------------------------ */
void loop() {
	// check for a new update and restart
	if (shouldReboot) {
		Serial.println(F("[ UPDT ] Rebooting..."));
		delay(100);
		ESP.restart();
	}
	unsigned long currentMillis = millis();
	if (currentMillis - previousMillis >= activateTime && activateRelay) {
		activateRelay = false;
		digitalWrite(relayPin, LOW);
		turnOnLed(blueLed);
	}
	if (currentMillis - previousMillis >= activateTime && denyAcc) {
		denyAcc = false;
		turnOnLed(blueLed);
	}
	if (activateRelay) {
		turnOnLed(greedLed);
		digitalWrite(relayPin, HIGH);
	}
	// Get Time from NTP Server
	timeClient.update();

	// Another loop for RFID Events, since we are using polling method instead of Interrupt we need to check RFID hardware for events
	rfidloop();
}

/* ------------------ RFID Functions ------------------- */
// RFID Specific Loop
void rfidloop() {
  	//If a new PICC placed to RFID reader continue
  	if ( ! mfrc522.PICC_IsNewCardPresent()) {
    	delay(50);
    	return;
  	}
  	//Since a PICC placed get Serial (UID) and continue
  	if ( ! mfrc522.PICC_ReadCardSerial()) {
    	delay(50);
    	return;
  	}
  	// We got UID tell PICC to stop responding
  	mfrc522.PICC_HaltA();

	// There are Mifare PICCs which have 4 byte or 7 byte UID
	// Get PICC's UID and store on a variable
	Serial.print(F("[ INFO ] PICC's UID: "));
	String uid = "";
	for (int i = 0; i < mfrc522.uid.size; ++i) {
	    uid += String(mfrc522.uid.uidByte[i], HEX);
	}
	Serial.print(uid);
	// Get PICC type
	MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
	String type = mfrc522.PICC_GetTypeName(piccType);

	// We are going to use filesystem to store known UIDs.
	int isKnown = 0;  // First assume we don't know until we got a match
	// If we know the PICC we need to know if its User have an Access
	int haveAcc = 0;  // First assume User do not have access
	String timedAccess = "";
	// Prepend /P/ on filename so we distinguish UIDs from the other files
	filename = "/P/";
	filename += uid;

	fs::File f = SPIFFS.open(filename, "r");
	// Check if we could find it above function returns true if the file is exist
	if (f) {
	    isKnown = 1; // we found it and label it as known
	    // Now we need to read contents of the file to parse JSON object contains Username and Access Status
	    size_t size = f.size();
	    // Allocate a buffer to store contents of the file.
	    std::unique_ptr<char[]> buf(new char[size]);
	    // We don't use String here because ArduinoJson library requires the input
	    // buffer to be mutable. If you don't use ArduinoJson, you may as well
	    // use configFile.readString instead.
	    f.readBytes(buf.get(), size);
	    DynamicJsonBuffer jsonBuffer;
	    JsonObject& json = jsonBuffer.parseObject(buf.get());
	    // Check if we succesfully parse JSON object
	    if (json.success()) {
	    	// Get username Access Status
			String username = json["user"];
			haveAcc = json["haveAcc"];
			Serial.println(" = known PICC");
			Serial.print("[ INFO ] User Name: ");
			Serial.print(username);
			// Check if user have an access
			if (haveAcc == 1) {
				allowAccess();
			}
			else if (haveAcc == 0) {
				denyAccess();
			}
			else if (haveAcc == 2) {
				//Check timed access
        		const char* timedAccessBuffer = json["timedAcc"];
				timedAccess = String(timedAccessBuffer);
				int dayCount = (timedAccess.length()/13);
				int allowedDays[dayCount];
				for (int i = 0; i < dayCount; i++) {
					allowedDays[i] = String(timedAccess.charAt(i*14)).toInt();
				}
				int currentDay = timeClient.getDay();
				bool checkTime = false;
				int checkTimeOnPos = 0;
				//Check if current day is one of the allowed days
				for (int i = 0; i < dayCount; i++)
				{
					if (currentDay == allowedDays[i]) {
						checkTime = true;
						checkTimeOnPos = i;
						break;
					} 
				}
				if (checkTime) {
					String fromTime = timedAccess.substring((checkTimeOnPos*14) + 2,(checkTimeOnPos*14) + 7);
					String untillTime = timedAccess.substring((checkTimeOnPos*14) + 8,(checkTimeOnPos*14) + 13);
					int currentHour = timeClient.getHours(), currentMinute = timeClient.getMinutes();
					if ((fromTime.substring(0,2).toInt()) < currentHour && (untillTime.substring(0,2).toInt()) > currentHour) allowAccess(); //Within the hours
					else if ((fromTime.substring(0,2).toInt()) == currentHour && (fromTime.substring(3).toInt()) < currentMinute) allowAccess(); //Same hour as from time check the minutes
					else if ((untillTime.substring(0,2).toInt()) == currentHour && (untillTime.substring(3).toInt()) > currentMinute) allowAccess(); //Same hour as to time check the minutes
					else denyAccess();
				}
				else {
					denyAccess();
				}

			}
			// Also inform Administrator Portal
			// Encode a JSON Object and send it to All WebSocket Clients
			DynamicJsonBuffer jsonBuffer2;
			JsonObject& root = jsonBuffer2.createObject();
			root["command"] = "piccscan";
			// UID of Scanned RFID Tag
			root["uid"] = uid;
			// Type of PICC
			root["type"] = type;
			// A boolean 1 for known tags 0 for unknown
			root["known"] = isKnown;
			// An int 1 for granted 0 for denied access, 2 for timed
			root["access"] = haveAcc;
			// Username
			root["user"] = username;
			// Timed access
			if (haveAcc == 2) root["timed"] = timedAccess;
			else root["timed"] = "";
			//TODO: add time schedule
			// 0:sunday, 1:monday etc
			//Format: (0_12:00-26:00 1_08:00-16:00)
			size_t len = root.measureLength();
			AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
			if (buffer) {
        		root.printTo((char *)buffer->get(), len + 1);
        		ws.textAll(buffer);
      		}
    	}
    	else {
      		Serial.println("");
      		Serial.println(F("[ WARN ] Failed to parse User Data"));
    	}
    	f.close();
  	}
  	else {
		// If we don't know the UID, inform Administrator Portal so admin can give access or add it to database
		Serial.println(" = unknown PICC");
		denyAccess();
		DynamicJsonBuffer jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();
		root["command"] = "piccscan";
		// UID of Scanned RFID Tag
		root["uid"] = uid;
		// Type of PICC
		root["type"] = type;
		// A boolean 1 for known tags 0 for unknown
		root["known"] = isKnown;
		size_t len = root.measureLength();
		AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
		if (buffer) {
			root.printTo((char *)buffer->get(), len + 1);
			ws.textAll(buffer);
		}
  	}
  	// So far got we got UID of Scanned RFID Tag, checked it if it's on the database and access status, informed Administrator Portal
}

void allowAccess() {
	activateRelay = true;  // Give user Access to Door, Safe, Box whatever you like
	previousMillis = millis();
	Serial.println(" has access");
}

void denyAccess() {
	turnOnLed(redLed);
	denyAcc = true;
	previousMillis = millis();
	Serial.println(" does not have access");
}

// Configure RFID Hardware
void setupRFID(int rfid_ss, int rfid_rst, int rfid_gain) {
 	SPI.begin();           // MFRC522 Hardware uses SPI protocol
 	mfrc522.PCD_Init(rfid_ss, rfid_rst);    // Initialize MFRC522 Hardware
  	// Set RFID Hardware Antenna Gain
  	// This may not work with some boards
  	mfrc522.PCD_SetAntennaGain(rfid_gain);
  	Serial.printf("[ INFO ] RFID SS_PIN: %u and Gain Factor: %u", rfid_ss, rfid_gain);
  	Serial.println("");
  	ShowReaderDetails(); // Show details of PCD - MFRC522 Card Reader details
}

void ShowReaderDetails() {
  	// Get the MFRC522 software version
  	byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  	Serial.print(F("[ INFO ] MFRC522 Version: 0x"));
  	Serial.print(v, HEX);
  	if (v == 0x91)
    	Serial.print(F(" = v1.0"));
  	else if (v == 0x92)
    	Serial.print(F(" = v2.0"));
  	else if (v == 0x88)
    	Serial.print(F(" = clone"));
  	else
    	Serial.print(F(" (unknown)"));
  	Serial.println("");
  	// When 0x00 or 0xFF is returned, communication probably failed
  	if ((v == 0x00) || (v == 0xFF)) {
    	Serial.println(F("[ WARN ] Communication failure, check if MFRC522 properly connected"));
  	}
}

/* ------------------ WiFi Functions ------------------- */
// Try to connect Wi-Fi
bool connectSTA(const char* sta_ssid, const char* sta_pass) {
  	WiFi.mode(WIFI_STA);
  	// First connect to a wi-fi network
  	WiFi.begin(sta_ssid, sta_pass);
  	// Inform user we are trying to connect
  	Serial.print(F("[ INFO ] Trying to connect WiFi: "));
  	Serial.print(sta_ssid);
  	// We try it for 20 seconds and give up on if we can't connect
  	unsigned long now = millis();
  	uint8_t timeout = 20; // define when to time out in seconds
  	// Wait until we connect or 20 seconds pass
  	do {
    	if (WiFi.status() == WL_CONNECTED) {
      		break;
    	}
    	delay(500);
    	Serial.print(F("."));
  	}
  	while (millis() - now < timeout * 1000);
  	// We now out of the while loop, either time is out or we connected. check what happened
  	if (WiFi.status() == WL_CONNECTED) { // Assume time is out first and check
    	Serial.println();
    	Serial.print(F("[ INFO ] Client IP address: ")); // Great, we connected, inform
    	Serial.println(WiFi.localIP());
    	return true;
  	}
  	else { // We couln't connect, time is out, inform
    	Serial.println();
    	Serial.println(F("[ WARN ] Couldn't connect in time"));
    	return false;
  	}
}

// Fallback to AP Mode, so we can connect to ESP if there is no Internet connection
bool setupAP(const char* ap_ssid, const char* ap_pass) {
  	WiFi.mode(WIFI_AP);
  	Serial.print(F("[ INFO ] Configuring access point... "));
  	bool result = WiFi.softAP(ap_ssid, ap_pass);
  	Serial.println(result ? "Ready" : "Failed!");
  	// Access Point IP
  	IPAddress myIP = WiFi.softAPIP();
  	Serial.print(F("[ INFO ] AP IP address: "));
  	Serial.println(myIP);
  	return result;
}

/* ------------------ SPIFFS Functions ----------------- */
bool loadConfiguration() {
  	fs::File configFile = SPIFFS.open("/auth/config.json", "r");
  	if (!configFile) {
    	Serial.println(F("[ WARN ] Failed to open config file"));
    	return false;
  	}
	size_t size = configFile.size();
  	// Allocate a buffer to store contents of the file.
  	std::unique_ptr<char[]> buf(new char[size]);
  	// We don't use String here because ArduinoJson library requires the input
  	// buffer to be mutable. If you don't use ArduinoJson, you may as well
  	// use configFile.readString instead.
  	configFile.readBytes(buf.get(), size);
	DynamicJsonBuffer jsonBuffer;
  	JsonObject& json = jsonBuffer.parseObject(buf.get());
  	if (!json.success()) {
    	Serial.println(F("[ WARN ] Failed to parse config file"));
    	return false;
  	}

  	//Webserver variables
  	const char *admin_pass_buffer = json["auth_pass"];
    admin_pass = String(admin_pass_buffer);
  	auth_pass = const_cast<char*>(admin_pass.c_str());

  	//Wifi variables
  	const char *wifi_hostname = json["wifi_hostname"];
  	const char *ap_ssid = json["ap_ssid"];
  	const char *ap_pass = json["ap_pass"];
  	const char *sta_ssid = json["sta_ssid"];
  	const char *sta_pass = json["sta_pass"];

  	//Hardware variables
  	int rfidgain = json["rfid_gain"];
  	activateTime = json["relay_time"];
    
  	configFile.close();
  	
  	setupRFID(MFRC522_SS,UINT8_MAX,rfidgain);

  	WiFi.hostname(wifi_hostname);
  	if (!connectSTA(sta_ssid, sta_pass)){ // If unable to connect to WiFi setup Access point
  		if (!setupAP(ap_ssid, ap_pass)) return false;
  	}

  	// Start mDNS service so we can connect to http://esp-rfid.local (if Bonjour installed on Windows or Avahi on Linux)
  	if (!MDNS.begin(wifi_hostname)) {
   		Serial.println(F("Error setting up MDNS responder!"));
  	}
  	// Add Web Server service to mDNS
  	MDNS.addService("http", "tcp", 80);

  	return true;
}

/* ------------------ Webserver Functions -------------- */
void setupWebserver(){
	// Start WebSocket Plug-in and handle incoming message on "onWsEvent" function
  	server.addHandler(&ws);
  	ws.onEvent(onWsEvent);

  	// Configure web server
  	// Add Text Editor (http://esp-rfid.local/edit) to Web Server. This feature likely will be dropped on final release.
  	server.addHandler(new SPIFFSEditor("admin", admin_pass));

  	// Serve confidential files in /auth/ folder with a Basic HTTP authentication
  	server.serveStatic("/auth/", SPIFFS, "/auth/").setDefaultFile("users.htm").setAuthentication("admin", auth_pass);
  	// Serve all files in root folder
 	server.serveStatic("/", SPIFFS, "/");
  	// Handle what happens when requested web file couldn't be found
  	server.onNotFound([](AsyncWebServerRequest * request) {
    	request->send(404);
  	});

  	// Simple Firmware Update Handler
  	server.on("/auth/update", HTTP_POST, [](AsyncWebServerRequest * request) {
    	shouldReboot = !Update.hasError();
    	AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot ? "OK" : "FAIL");
    	response->addHeader("Connection", "close");
    	request->send(response);
  	}, [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
    	Serial.printf("[ UPDT ] Firmware update started: %s\n", filename.c_str());
      	Update.runAsync(true);
      	if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
        	Update.printError(Serial);
      	}
    }
    if (!Update.hasError()) {
      	if (Update.write(data, len) != len) {
        	Update.printError(Serial);
      	}
    }
    if (final) {
      	if (Update.end(true)) {
        	Serial.printf("[ UPDT ] Firmware update finished: %uB\n", index + len);
      	} else {
        	Update.printError(Serial);
      	}
    }
  	});

	// Simple SPIFFs Update Handler
	server.on("/auth/spiupdate", HTTP_POST, [](AsyncWebServerRequest * request) {
		shouldReboot = !Update.hasError();
	    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot ? "OK" : "FAIL");
	    response->addHeader("Connection", "close");
	    request->send(response);
	}, [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
	if (!index) {
	    Serial.printf("[ UPDT ] SPIFFS update started: %s\n", filename.c_str());
	    Update.runAsync(true);
	    size_t spiffsSize = ((size_t) &_SPIFFS_end - (size_t) &_SPIFFS_start);
	    if (!Update.begin(spiffsSize, U_SPIFFS)) {
	    	Update.printError(Serial);
	    }
	}
	if (!Update.hasError()) {
		if (Update.write(data, len) != len) {
			Update.printError(Serial);
		}
	}
	if (final) {
		if (Update.end(true)) {
			Serial.printf("[ UPDT ] SPIFFS update finished: %uB\n", index + len);
	    } else {
	        Update.printError(Serial);
	    }
	}
	});

  	// Start Web Server
  	server.begin();
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  	if (type == WS_EVT_ERROR) {
    	Serial.printf("[ WARN ] WebSocket[%s][%u] error(%u): %s\r\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
  	}
  	else if (type == WS_EVT_DATA) {
    	AwsFrameInfo * info = (AwsFrameInfo*)arg;
   	 	String msg = "";

    	if (info->final && info->index == 0 && info->len == len) {
      		//the whole message is in a single frame and we got all of it's data
      		for (size_t i = 0; i < info->len; i++) {
        		msg += (char) data[i];
      	}

      	// We should always get a JSON object (stringfied) from browser, so parse it
      	DynamicJsonBuffer jsonBuffer;
      	JsonObject& root = jsonBuffer.parseObject(msg);
      	if (!root.success()) {
        	Serial.println(F("[ WARN ] Couldn't parse WebSocket message"));
        	return;
      	}

      	// Web Browser sends some commands, check which command is given
      	const char * command = root["command"];
      
      	// Check whatever the command is and act accordingly
      	if (strcmp(command, "remove")  == 0) {
        	const char* uid = root["uid"];
	        filename = "/P/";
	        filename += uid;
	        SPIFFS.remove(filename);
      	}
      	else if (strcmp(command, "configfile")  == 0) {
        	fs::File f = SPIFFS.open("/auth/config.json", "w+");
        	if (f) {
          		f.print(msg);
          		f.close();
          		ESP.reset();
        	}
      	}
      	else if (strcmp(command, "picclist")  == 0) {
        	sendPICClist();
      	}
      	else if (strcmp(command, "userfile")  == 0) {
        	const char* uid = root["uid"];
        	filename = "/P/";
        	filename += uid;
        	fs::File f = SPIFFS.open(filename, "w+");
        	// Check if we created the file
        	if (f) {
          		f.print(msg);
          		f.close();
        	}
      	}
      	else if (strcmp(command, "testrelay")  == 0) {
        	activateRelay = true;
        	previousMillis = millis();
      	}
      	else if (strcmp(command, "scan")  == 0) {
	        WiFi.scanNetworksAsync(printScanResult);
      	}
      	else if (strcmp(command, "getconf")  == 0) {
	        fs::File configFile = SPIFFS.open("/auth/config.json", "r");
	        if (configFile) {
	          	size_t len = configFile.size();
	          	AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
	          	if (buffer) {
	            	configFile.readBytes((char *)buffer->get(), len + 1);
	            	ws.textAll(buffer);
	          	}
	          	configFile.close();
	        }
      	}
      	else if (strcmp(command, "loglist") ==0) {

      	}
      	else if (strcmp(command, "clearlog") ==0) {

      	}
    }
  }
}

void sendPICClist() {
  	Dir dir = SPIFFS.openDir("/P/");
  	DynamicJsonBuffer jsonBuffer;
  	JsonObject& root = jsonBuffer.createObject();
  	root["command"] = "picclist";

  	JsonArray& data = root.createNestedArray("piccs");
  	JsonArray& data2 = root.createNestedArray("users");
  	JsonArray& data3 = root.createNestedArray("access");
  	JsonArray& data4 = root.createNestedArray("timed");
  	while (dir.next()) {
    	fs::File f = SPIFFS.open(dir.fileName(), "r");
	    size_t size = f.size();
	    // Allocate a buffer to store contents of the file.
	    std::unique_ptr<char[]> buf(new char[size]);
	    // We don't use String here because ArduinoJson library requires the input
	    // buffer to be mutable. If you don't use ArduinoJson, you may as well
	    // use configFile.readString instead.
	    f.readBytes(buf.get(), size);
	    DynamicJsonBuffer jsonBuffer2;
	    JsonObject& json = jsonBuffer2.parseObject(buf.get());
	    if (json.success()) {
	      	String username = json["user"];
	      	int haveAcc = json["haveAcc"];
	      	String timedAccess = json["timedAcc"];
	      	data2.add(username);
	      	data3.add(haveAcc);
	      	data4.add(timedAccess);
	    }
	    data.add(dir.fileName());
	    f.close();
  	}
  	size_t len = root.measureLength();
 	AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  	if (buffer) {
    	root.printTo((char *)buffer->get(), len + 1);
    	ws.textAll(buffer);
  	}
}


// Send Scanned SSIDs to websocket clients as JSON object
void printScanResult(int networksFound) {
  	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.createObject();
	root["command"] = "ssidlist";
	JsonArray& data = root.createNestedArray("ssid");
	for (int i = 0; i < networksFound; ++i) {
		// Print SSID for each network found
		data.add(WiFi.SSID(i));
	}
	size_t len = root.measureLength();
	AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
	if (buffer) {
		root.printTo((char *)buffer->get(), len + 1);
		ws.textAll(buffer);
	}
	WiFi.scanDelete();
}

/* ------------------ Logging Functions ---------------- */
String getDateTime() {
	unsigned long epochTime = timeClient.getEpochTime();
	uint8_t dateMonth = month(epochTime);
	uint8_t dateDay = day(epochTime);
	String dateTime =  String(year(epochTime)) + "-";
	if (dateMonth < 10) dateTime += "0" + String(dateMonth) + "-";
	else dateTime += String(dateMonth) + "-";
	if (dateDay < 10) dateTime += "0" + String(dateDay);
	else dateTime += String(dateDay);
	dateTime += " " + timeClient.getFormattedTime();
	return dateTime;
}

String getDate() {
	unsigned long epochTime = timeClient.getEpochTime();
	uint8_t dateMonth = month(epochTime);
	uint8_t dateDay = day(epochTime);
	String dateTime =  String(year(epochTime)) + "-";
	if (dateMonth < 10) dateTime += "0" + String(dateMonth) + "-";
	else dateTime += String(dateMonth) + "-";
	if (dateDay < 10) dateTime += "0" + String(dateDay);
	else dateTime += String(dateDay);
	return dateTime;
}

bool createLog(String dataString, String filename) {
	if (!SDAvailable) return false;
	if (SDMutex) waitForMutex();
	SDMutex = true;
	sd::File dataFile = SD.open(filename, FILE_WRITE);
	if (dataFile) {
		dataFile.println(dataString);
		dataFile.close();
		SDMutex = false;
		return true;
	}
	else {
		Serial.println(F("[ WARN ] Error opening file on SD card"));
		SDMutex = false;
		return false;
	}
}

bool readLog(String filename) {
	if (!SDAvailable) return false;
	if (SDMutex) waitForMutex();
	SDMutex = true;
	sd::File dataFile = SD.open(filename);
	if (!dataFile) {
		Serial.println(F("[ WARN ] Error opening file on SD card"));
		SDMutex = false;
		return false;
	}

	DynamicJsonBuffer jsonBuffer;
  	JsonObject& root = jsonBuffer.createObject();
  	root["command"] = "loglist";

  	JsonArray& data = root.createNestedArray("datetime");
  	JsonArray& data2 = root.createNestedArray("piccs");
  	JsonArray& data3 = root.createNestedArray("users");

  	while (dataFile.available()) {
  		
  	}

  	size_t len = root.measureLength();
  	AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  	if (buffer) {
    	root.printTo((char *)buffer->get(), len + 1);
    	ws.textAll(buffer);
    	return true;
  	}
  	else return false;
}

bool readUserLog(String UID) {
	if (!SDAvailable) return false;
	if (SDMutex) waitForMutex();
	return true;

}

void waitForMutex() {
	while (SDMutex)
	{}
}

/* ------------------ Misc Functions ------------------- */
void turnOnLed(int pin) {
	switch (pin) {
	    case greedLed:
	    	digitalWrite(redLed, LOW);
	    	digitalWrite(blueLed, LOW);
	      	break;
	    case redLed:
		    digitalWrite(redLed, HIGH);
	    	digitalWrite(blueLed, LOW);
	      	break;
	    case blueLed:
		    digitalWrite(redLed, LOW);
	    	digitalWrite(blueLed, HIGH);
	      	break;
	    default:
	    	break;
	}
}
