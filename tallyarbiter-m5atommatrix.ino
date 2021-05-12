#include "M5Atom.h"

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <SocketIOclient.h>
#include <Arduino_JSON.h>
#include <PinButton.h>
#include <Preferences.h>

#define WHITE   0xFFFFFF //   0 64  0
#define GREEN   0xFF0000 //   0 64  0
#define RED     0x00FF00 // 255  0  0
#define BLACK   0x000000 //   0 64  0
#define YELLOW  0xFFFF00 //   0 64  0


/* USER CONFIG VARIABLES
 *  Change the following variables before compiling and sending the code to your device.
 */

bool CUT_BUS = true; // true = Programm + Preview = Red Tally; false = Programm + Preview = Yellow Tally

//Wifi SSID and password
const char * networkSSID = "NetworkSSID";
const char * networkPass = "NetworkPass";

//For static IP Configuration, change USE_STATIC to true and define your IP address settings below
bool USE_STATIC = false; // true = use static, false = use DHCP

IPAddress clientIp(192, 168, 2, 5); // Static IP
IPAddress subnet(255, 255, 255, 0); // Subnet Mask
IPAddress gateway(192, 168, 2, 1); // Gateway

//Tally Arbiter Server
const char * tallyarbiter_host = "192.168.0.137"; //IP address of the Tally Arbiter Server
const int tallyarbiter_port = 4455;

/* END OF USER CONFIG */

Preferences preferences;
const byte led_program = 10;
const int led_preview = 25;   //OPTIONAL Led for preview on pin G25

//M5StickC variables
PinButton btnAction(39); //the "Action" button on the device

//Tally Arbiter variables
SocketIOclient socket;
JSONVar BusOptions;
JSONVar Devices;
JSONVar DeviceStates;
String DeviceId = "unassigned";
String DeviceName = "Unassigned";
String ListenerType = "Atom-1";
bool mode_preview = false;
bool mode_program = false;
String LastMessage = "";

//General Variables
bool networkConnected = false;
int currentBrightness = 10; //60 is Max level

void setup() {
  pinMode (led_preview, OUTPUT);
  Serial.begin(115200);
  while (!Serial);

  // Initialize the M5Atom object
  logger("Initializing M5-Atom.", "info-quiet");
  
  M5.begin(true, false, true);
  setCpuFrequencyMhz(80);    //Save battery by turning down the CPU clock
  btStop();                 //Save battery by turning off BlueTooth
  delay(50);
  fillDisplay(BLACK);
  M5.dis.setBrightness(currentBrightness);

  logger("Tally Arbiter M5-Atom Listener Client booting.", "info");

  delay(100); //wait 100ms before moving on
  connectToNetwork(); //starts Wifi connection
  while (!networkConnected) {
    delay(200);
  }

  // Enable interal led for program trigger
  pinMode(led_program, OUTPUT);
  digitalWrite(led_program, HIGH);


  preferences.begin("tally-arbiter", false);
  if(preferences.getString("deviceid").length() > 0){
    DeviceId = preferences.getString("deviceid");
  }
  if(preferences.getString("devicename").length() > 0){
    DeviceName = preferences.getString("devicename");
  }
  preferences.end();

  connectToServer();

}

uint8_t FSM = 0;

void loop() {
  socket.loop();
  btnAction.update();
  
  if (btnAction.isClick())
  {

      switch (FSM)
      {
      case 0:
          M5.dis.setBrightness(20);
          break;
      case 1:
          M5.dis.setBrightness(40);
          break;
      case 2:
          M5.dis.setBrightness(60);
          break;
      case 3:
          M5.dis.setBrightness(10);
          break;
      default:
          break;
      }

      FSM++;
      if (FSM >= 4)
      {
          FSM = 0;
      }
  }
  // Maybe add a small pause here for better cooling? delay(50);
}

void logger(String strLog, String strType) {
  Serial.println(strLog);
}

void connectToNetwork() {
  logger("Connecting to SSID: " + String(networkSSID), "info");

  WiFi.disconnect(true);
  WiFi.onEvent(WiFiEvent);

  WiFi.mode(WIFI_STA); //station
  WiFi.setSleep(false);

  if(USE_STATIC == true) {
    WiFi.config(clientIp, gateway, subnet);
  }

  WiFi.begin(networkSSID, networkPass);
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case SYSTEM_EVENT_STA_GOT_IP:
      logger("Network connected!", "info");
      logger(WiFi.localIP().toString(), "info");
      networkConnected = true;
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      logger("Network connection lost!", "info");
      networkConnected = false;
      break;
  }
}

void ws_emit(String event, const char *payload = NULL) {
  if (payload) {
    String msg = "[\"" + event + "\"," + payload + "]";
    socket.sendEVENT(msg);
  } else {
    String msg = "[\"" + event + "\"]";
    socket.sendEVENT(msg);
  }
}

void connectToServer() {
  logger("Connecting to Tally Arbiter host: " + String(tallyarbiter_host), "info");
  socket.onEvent(socket_event);
  socket.begin(tallyarbiter_host, tallyarbiter_port);
}

void socket_event(socketIOmessageType_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case sIOtype_CONNECT:
      socket_Connected((char*)payload, length);
      break;

    case sIOtype_DISCONNECT:
    case sIOtype_ACK:
    case sIOtype_ERROR:
    case sIOtype_BINARY_EVENT:
    case sIOtype_BINARY_ACK:
      // Not handled
      break;

    case sIOtype_EVENT:
      String msg = (char*)payload;
      String type = msg.substring(2, msg.indexOf("\"",2));
      String content = msg.substring(type.length() + 4);
      content.remove(content.length() - 1);

//      logger("Got event '" + type + "', data: " + content, "info-quiet");

      if (type == "bus_options") BusOptions = JSON.parse(content);
      if (type == "reassign") socket_Reassign(content);
      if (type == "flash") socket_Flash();
      if (type == "messaging") socket_Messaging(content);

      if (type == "deviceId") {
        DeviceId = content.substring(1, content.length()-1);
        SetDeviceName();
      }

      if (type == "devices") {
        Devices = JSON.parse(content);
        SetDeviceName();
      }

      if (type == "device_states") {
        DeviceStates = JSON.parse(content);
        processTallyData();
      }

      break;
  }
}

void socket_Connected(const char * payload, size_t length) {
  logger("Connected to Tally Arbiter server.", "info");
  String deviceObj = "{\"deviceId\": \"" + DeviceId + "\", \"listenerType\": \"" + ListenerType + "\"}";
  char charDeviceObj[1024];
  strcpy(charDeviceObj, deviceObj.c_str());
  ws_emit("bus_options");
  ws_emit("device_listen_m5", charDeviceObj);
}

// Make all pixels the same color
void fillDisplay(int fillColor)
{
  for(int i = 0; i < 25; i++)
  {
      M5.dis.drawpix(i, fillColor);
  }
}


void socket_Flash() {
  //flash the screen white 3 times
  fillDisplay(WHITE);
  delay(500);
  fillDisplay(BLACK);
  delay(500);
  fillDisplay(WHITE);
  delay(500);
  fillDisplay(BLACK);
  delay(500);
  fillDisplay(WHITE);
  delay(500);
  fillDisplay(BLACK);
  delay(500);

  evaluateMode();
}

String strip_quot(String str) {
  if (str[0] == '"') {
    str.remove(0, 1);
  }
  if (str.endsWith("\"")) {
    str.remove(str.length()-1, 1);
  }
  return str;
}

void socket_Reassign(String payload) {
  String oldDeviceId = payload.substring(0, payload.indexOf(','));
  String newDeviceId = payload.substring(oldDeviceId.length()+1);
  oldDeviceId = strip_quot(oldDeviceId);
  newDeviceId = strip_quot(newDeviceId);

  String reassignObj = "{\"oldDeviceId\": \"" + oldDeviceId + "\", \"newDeviceId\": \"" + newDeviceId + "\"}";
  char charReassignObj[1024];
  strcpy(charReassignObj, reassignObj.c_str());
  ws_emit("listener_reassign_object", charReassignObj);
  ws_emit("devices");
  fillDisplay(WHITE);
  delay(200);
  fillDisplay(BLACK);
  delay(200);
  fillDisplay(WHITE);
  delay(200);
  fillDisplay(BLACK);
  DeviceId = newDeviceId;
  preferences.begin("tally-arbiter", false);
  preferences.putString("deviceid", newDeviceId);
  preferences.end();
  SetDeviceName();
}

void socket_Messaging(String payload) {
  String strPayload = String(payload);
  int typeQuoteIndex = strPayload.indexOf(',');
  String messageType = strPayload.substring(0, typeQuoteIndex);
  messageType.replace("\"", "");
  int messageQuoteIndex = strPayload.lastIndexOf(',');
  String message = strPayload.substring(messageQuoteIndex + 1);
  message.replace("\"", "");
  LastMessage = messageType + ": " + message;
  evaluateMode();
}

void processTallyData() {
  for (int i = 0; i < DeviceStates.length(); i++) {
    if (getBusTypeById(JSON.stringify(DeviceStates[i]["busId"])) == "\"preview\"") {
      if (DeviceStates[i]["sources"].length() > 0) {
        mode_preview = true;
      }
      else {
        mode_preview = false;
      }
    }
    if (getBusTypeById(JSON.stringify(DeviceStates[i]["busId"])) == "\"program\"") {
      if (DeviceStates[i]["sources"].length() > 0) {
        mode_program = true;
      }
      else {
        mode_program = false;
      }
    }
  }

  evaluateMode();
}

String getBusTypeById(String busId) {
  for (int i = 0; i < BusOptions.length(); i++) {
    if (JSON.stringify(BusOptions[i]["id"]) == busId) {
      return JSON.stringify(BusOptions[i]["type"]);
    }
  }

  return "invalid";
}

void SetDeviceName() {
  for (int i = 0; i < Devices.length(); i++) {
    if (JSON.stringify(Devices[i]["id"]) == "\"" + DeviceId + "\"") {
      String strDevice = JSON.stringify(Devices[i]["name"]);
      DeviceName = strDevice.substring(1, strDevice.length() - 1);
      break;
    }
  }
  preferences.begin("tally-arbiter", false);
  preferences.putString("devicename", DeviceName);
  preferences.end();
  evaluateMode();
}

void evaluateMode() {

  if (mode_preview && !mode_program) {
    logger("The device is in preview.", "info-quiet");
    fillDisplay(GREEN);
    
    digitalWrite(led_program, HIGH);
    digitalWrite (led_preview, HIGH);
  }
  else if (!mode_preview && mode_program) {
    logger("The device is in program.", "info-quiet");
    fillDisplay(RED);
    
    digitalWrite(led_program, LOW);
    digitalWrite(led_preview, LOW);
  }
  else if (mode_preview && mode_program) {
    logger("The device is in preview+program.", "info-quiet");
    
    if (CUT_BUS == true) {
      fillDisplay(RED);
    }
    else {
      fillDisplay(YELLOW);
    }
    digitalWrite(led_program, LOW);
    digitalWrite (led_preview, HIGH);
  }
  else {
    digitalWrite(led_program, HIGH);
    digitalWrite(led_preview, LOW);
    
    fillDisplay(BLACK);
  }
}