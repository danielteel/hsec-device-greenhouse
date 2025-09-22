#include <Arduino.h>
#include <WiFi.h>
#include <Esp.h>
#include <esp_wifi.h>
#include "time.h"
#include "camera.h"
#include "utils.h"
#include "secrets.h"
#include "net.h"
#include <time.h> 
#include "storage.h"
#include "DHTesp.h"


const uint8_t dht11Pin = 13;

DHTesp dht;

const char *WiFiSSID = SECRET_WIFI_SSID;
const char *WiFiPass = SECRET_WIFI_PASS;

const uint32_t picturePeriod = 2000;
const uint32_t weatherPeriod = 2000;
const uint32_t logPeriod = 60000;

const uint32_t howLongBeforeRestartIfNotConnecting = 300000;//restart esp32 if havent been able to connect to server for 5 minutes


const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.google.com";
const char* ntpServer3 = "time.windows.com";
const char* timeZone = "MST7MDT,M3.2.0,M11.1.0";//https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

StorageData storageData;


Net NetClient(SECRET_DEVICE_NAME, SECRET_ENCROKEY, SECRET_HOST_ADDRESS, SECRET_HOST_PORT);


void packetReceived(uint8_t* data, uint32_t dataLength){
    sensor_t * s;
    switch (data[0]){
        case 1:
            s = esp_camera_sensor_get();
            if (s) s->set_quality(s, data[1]);
            storageData.quality=data[1];
            commitStorage(storageData);
            break;
        case 2:
            s = esp_camera_sensor_get();
            if (s) s->set_framesize(s, (framesize_t)data[1]);
            storageData.frame_size=(framesize_t)data[1];
            commitStorage(storageData);
            break;
    }
}

void onConnected(){
    Serial.println("NetClient Connected");
    NetClient.sendString("\xFF\x01Qual(8best-63worst):byte:1,FrameSize(3|5|7):byte:2");
}

void onDisconnected(){
    Serial.println("NetClient disconnected");
}

void WiFiSetup(bool doRandomMAC){
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);

    uint8_t newMac[6]={0,0,0,0,0,0};
    String newHostName=SECRET_DEVICE_NAME;
    if (doRandomMAC){
        esp_fill_random(newMac+1, 5);
        for (int i=0;i<6;i++){
            newHostName+=String(newMac[i], 16);
        }
    }
    WiFi.setHostname(newHostName.c_str());

    WiFi.mode(WIFI_STA);
    if (doRandomMAC) esp_wifi_set_mac(WIFI_IF_STA, newMac);
    WiFi.setMinSecurity(WIFI_AUTH_OPEN);
    WiFi.setSleep(WIFI_PS_NONE);

    WiFi.begin(WiFiSSID, WiFiPass);
    Serial.println("WiFiSetup:");
    Serial.print("    Mac Address:");
    Serial.println(WiFi.macAddress());
    Serial.print("    Hostname:");
    Serial.println(newHostName);
}

void setup(){
    //Setup serial comm
    Serial.begin(115200);
    Serial.println("Initializing...");

    //Setup DHT11
    dht.setup(dht11Pin, DHTesp::DHT22);

    //Setup time
    configTime(0, 0, ntpServer1, ntpServer2, ntpServer3);
    setenv("TZ", timeZone, 1);
    tzset();

    //Setup non volatile storage
    StorageData defaultStorage;
    defaultStorage.frame_size=FRAMESIZE_HVGA;
    defaultStorage.quality=16;
    initStorage(&defaultStorage, storageData);
    Serial.print("Framesize:");
    Serial.println(storageData.frame_size);
    Serial.print("Quality:");
    Serial.println(storageData.quality);
    
    //Setup camera
    cameraSetup(storageData.frame_size, storageData.quality);

    //Setup IO

    //Setup WiFi
    WiFiSetup(false);

    //Setup NetClient
    NetClient.setPacketReceivedCallback(&packetReceived);
    NetClient.setOnConnected(&onConnected);
    NetClient.setOnDisconnected(&onDisconnected);
}


class StateMachine {
    public:
        StateMachine(uint8_t max){
            maxStates=max;
        }

        uint8_t maxStates=0;
        uint8_t state=0;


        void next(){
            state++;
            if (state>=maxStates){
                state=0;
            }
        }
};

void loop(){
    static StateMachine sendState(2);

    static uint32_t lastConnectTime=0;
    static uint8_t failReconnects=0;

    static uint32_t lastPictureSendTime=0;
    static uint32_t lastWeatherSendTime=0;
    static uint32_t lastLogTime=0;
    static uint32_t lastReadyTime=0;

    uint32_t currentTime = millis();

    if (WiFi.status() != WL_CONNECTED){//Reconnect to WiFi
        if (isTimeToExecute(lastConnectTime, 2000)){
            Serial.println("Waiting for autoreconnect...");
            failReconnects++;
            if (failReconnects>30){
                Serial.println("Autoreconnect failed, generating new MAC and retrying...");
                failReconnects=0;
                WiFiSetup(true);
            }
        }
    }else{
        failReconnects=0;
        if (NetClient.loop()){
            lastReadyTime=currentTime;

            if (isTimeToExecute(lastPictureSendTime, picturePeriod)){
                CAMERA_CAPTURE capture;
                if (cameraCapture(capture)){
                    NetClient.sendBinary(capture.jpgBuff, capture.jpgBuffLen);
                    cameraCaptureCleanup(capture);
                }else{
                    Serial.println("failed to capture ");
                }
            }
            if (isTimeToExecute(lastWeatherSendTime, weatherPeriod)){
                float humidity = dht.getHumidity();
                float temperature = dht.getTemperature()*1.8f+32.0f;
                NetClient.sendString(String("humidity=")+String(humidity, 1));
                NetClient.sendString(String("temperature=")+String(temperature, 1));
                Serial.println(String("humidity=")+String(humidity, 1));
                Serial.println(String("temperature=")+String(temperature, 1));
            }
            if (isTimeToExecute(lastLogTime, logPeriod)){
                NetClient.sendString("log");
                Serial.println("Logging");
            }
        }
    }

    if (currentTime-lastReadyTime > howLongBeforeRestartIfNotConnecting || lastReadyTime>currentTime){//Crude edge case handling, if overflow, just restart
        ESP.restart();
    }
}