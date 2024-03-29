#ifndef WIFI_H
#define WIFI_H

#include <Arduino.h>
#include <DNSServer.h>
#include <memory>
#include <ESP8266WiFi.h>

#include "changeCallback.h"

class WifiManager : public changeCallBack
{
public:
    void begin();
    void loop();
    void forget();
    bool isCaptivePortal();
    void setNewWifi(const String &newSSID, const String &newPass);
    const String &getAPForCaptiveMode() const
    {
        return rfcName;
    }

    static IPAddress LocalIP();
    static String SSID();
    static int8_t RSSI();
    void disconnect(bool disconnectWifi);
    static WifiManager instance;

private:
    WifiManager() {}
    DNSServer *dnsServer{nullptr};
    String ssid;
    String pass;

    bool reconnect = false;
    bool inCaptivePortal = false;
    uint64_t captivePortalStart = 0;
    String rfcName;
    const unsigned long timeout = 60000;

    uint8_t reconnectRetries{0};
    uint32_t reconnectLastRetry{0};
    bool checkConnection{false};

    void startCaptivePortal();
    void stopCaptivePortal();
    void connectNewWifi(const String &newSSID, const String &newPass);
    int8_t waitForConnectResult(unsigned long timeoutLength);
    bool connectSavedWifi();
    void onDisconnect(const WiFiEventStationModeDisconnected&);

    static String getRFC952Hostname(const String &name);
};
#endif
