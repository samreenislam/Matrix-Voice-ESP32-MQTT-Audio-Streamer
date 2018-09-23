/* ************************************************************************* *
 * Matrix Voice Audio Streamer
 * 
 * This program is written to be a streaming audio server running on the Matrix Voice.
 This is typically used for Snips.AI, it will then be able to replace
 * the Snips Audio Server, by publishing small wave messages to the hermes proticol
 * See https://snips.ai/ for more information
 * 
 * Author:  Paul Romkes
 * Date:    September 2018
 * Version: 2
 * 
 * Changelog:
 * ==========
 * v1:
 *  - first code release. It needs a lot of improvement, no hardcoding stuff
 * v2:
 *  - Change to Arduino IDE
 * ************************************************************************ */
#include <WiFi.h>
extern "C" {
  #include "freertos/FreeRTOS.h"
  #include "freertos/timers.h"
}
#include <AsyncMqttClient.h>
#include <MQTT.h>

#include "wishbone_bus.h"
#include "everloop.h"
#include "everloop_image.h"
#include "microphone_array.h"
#include "microphone_core.h"
#include "voice_memory_map.h"
#define RATE 16000
#define SITEID "matrixvoice"
#define SSID "SSID"
#define PASSWORD "PASSWORD"
#define MQTT_IP IPAddress(192, 168, 178, 194)
#define MQTT_HOST "192.168.178.194"
#define MQTT_PORT 1883
#define CHUNK 256 //set to multiplications of 256, voice return a set of 256
#define WIDTH 2
#define CHANNELS 1

WiFiClient net;
AsyncMqttClient asyncClient;
//We also need a sync client, asynch leads to errors on the audio thread
MQTTClient audioServer(2000);

TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;

namespace hal = matrix_hal;
uint16_t voicebuffer[CHUNK];
uint8_t voicemapped[CHUNK*WIDTH];
hal::WishboneBus wb;
hal::Everloop everloop;
hal::MicrophoneArray mics;    
std::string audioFrameTopic = std::string("hermes/audioServer/") + SITEID + std::string("/audioFrame");
std::string playFinishedTopic = std::string("hermes/audioServer/") + SITEID + std::string("/playFinished");
std::string playBytesTopic = std::string("hermes/audioServer/") + SITEID + std::string("/playBytes/#");

struct wavfile_header {
  char  riff_tag[4];    //4
  int   riff_length;    //4
  char  wave_tag[4];    //4
  char  fmt_tag[4];     //4
  int   fmt_length;     //4
  short audio_format;   //2
  short num_channels;   //2
  int   sample_rate;    //4
  int   byte_rate;      //4
  short block_align;    //2
  short bits_per_sample;//2
  char  data_tag[4];    //4
  int   data_length;    //4
};

struct wavfile_header header;

// ---------------------------------------------------------------------------
// EVERLOOP (The Led Ring)
// ---------------------------------------------------------------------------
void setEverloop(int red, int green, int blue, int white) {
    hal::EverloopImage image1d;
    for (hal::LedValue& led : image1d.leds) {
      led.red = red;
      led.green = green;
      led.blue = blue;
      led.white = white;
    }

    everloop.Write(&image1d);
}

// ---------------------------------------------------------------------------
// Network functions
// ---------------------------------------------------------------------------
void connectToWifi() {
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(SSID, PASSWORD);
}

void connectToMqtt() {
  Serial.println("Connecting to MQTT...");
  asyncClient.connect();
  connectAudio();
}

void connectAudio() {
 while (!audioServer.connect("MatrixVoiceAudio")) {
    delay(1000);
 }
}

void WiFiEvent(WiFiEvent_t event) {
  Serial.printf("[WiFi-event] event: %d\n", event);
  switch(event) {
  case SYSTEM_EVENT_STA_GOT_IP:
      setEverloop(0,0,10,0);
      Serial.println("WiFi connected");
      Serial.println("IP address: ");
      Serial.println(WiFi.localIP());
      connectToMqtt();
      break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("WiFi lost connection");
      xTimerStop(mqttReconnectTimer, 0); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
      xTimerStart(wifiReconnectTimer, 0);
      break;
  default:
    break;
  }
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  asyncClient.subscribe(playBytesTopic.c_str(), 0);
  asyncClient.subscribe("hermes/hotword/toggleOff",0);
  asyncClient.subscribe("hermes/hotword/toggleOn",0);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");
  if (WiFi.isConnected()) {
    xTimerStart(mqttReconnectTimer, 0);
  }
}

// ---------------------------------------------------------------------------
// MQTT Callback
// ---------------------------------------------------------------------------
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  String topicstr(topic);
  if (len + index == total) {
    if (topicstr.indexOf("toggleOff") > 0) {
      setEverloop(0,10,0,0);
    } else if (topicstr.indexOf("toggleOn") > 0) {
      setEverloop(0,0,10,0);
    } else if (topicstr.indexOf("playBytes") > 0) {
      String s = "{\"id\":\"" + topicstr.substring(39) + "\",\"siteId\":\"" + SITEID + "\",\"sessionId\":null}";
      asyncClient.publish(playFinishedTopic.c_str(), 0, false, s.c_str());
    }
  }
}
// ---------------------------------------------------------------------------
// Audiostream
// ---------------------------------------------------------------------------
void AudioSender( void * parameter ) {
  for(;;){
    if (audioServer.connected()) {
      //We are connected! Read the mics
       mics.Read();
      
      //NumberOfSamples() = kMicarrayBufferSize / kMicrophoneChannels = 4069 / 8 = 512
      for (uint32_t s = 0; s < CHUNK; s++) {
          voicebuffer[s] = mics.Beam(s);
      }

      //voicebuffer will hold 256 samples of 2 bytes, but we need it as 1 byte
      //We do a memcpy, because I need to add the wave header as well
      memcpy(voicemapped,voicebuffer,CHUNK*WIDTH);
  
      uint8_t payload[sizeof(header)+(CHUNK*WIDTH)];
      uint8_t payload2[sizeof(header)+(CHUNK*WIDTH)];
      //Add the wave header
      memcpy(payload,&header,sizeof(header));
      memcpy(&payload[sizeof(header)], voicemapped, sizeof(voicemapped));

      audioServer.publish(audioFrameTopic.c_str(), (char *)payload, sizeof(payload));
      
      //also send to second half as a wav
      for (uint32_t s = CHUNK; s < CHUNK*WIDTH; s++) {
          voicebuffer[s-CHUNK] = mics.Beam(s);
      }
  
      memcpy(voicemapped,voicebuffer,CHUNK*WIDTH);
  
      //Add the wave header
      memcpy(payload2,&header,sizeof(header));
      memcpy(&payload2[sizeof(header)], voicemapped, sizeof(voicemapped));
      
      audioServer.publish(audioFrameTopic.c_str(), (char *)payload2, sizeof(payload2));
    }
  }
  vTaskDelete(NULL);
}
 
void setup() {
  wb.Init();
  everloop.Setup(&wb);
  
  //setup mics
  mics.Setup(&wb);
  mics.SetSamplingRate(RATE);
  //mics.SetGain(5);

   // Microphone Core Init
  hal::MicrophoneCore mic_core(mics);
  mic_core.Setup(&wb);

  setEverloop(10,0,0,0);

  strncpy(header.riff_tag,"RIFF",4);
  strncpy(header.wave_tag,"WAVE",4);
  strncpy(header.fmt_tag,"fmt ",4);
  strncpy(header.data_tag,"data",4);

  header.riff_length = (uint32_t)sizeof(header) + (CHUNK * WIDTH);
  header.fmt_length = 16;
  header.audio_format = 1;
  header.num_channels = 1;
  header.sample_rate = RATE;
  header.byte_rate = RATE * WIDTH;
  header.block_align = WIDTH;
  header.bits_per_sample = WIDTH * 8;
  header.data_length = CHUNK * WIDTH;

  Serial.begin(115200);
 
  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToWifi));
 
  WiFi.onEvent(WiFiEvent);

  asyncClient.onConnect(onMqttConnect);
  asyncClient.onDisconnect(onMqttDisconnect);
  asyncClient.onMessage(onMqttMessage);
  asyncClient.setServer(MQTT_IP, MQTT_PORT);
  audioServer.begin(MQTT_HOST, net);

  connectToWifi(); 

  xTaskCreate(AudioSender,"AudioSender",10000,NULL,2,NULL);
  
}

void loop() {
}

