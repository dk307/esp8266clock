#include "WiFiManager.h"
#include "configManager.h"
#include "logging.h"
#include "operations.h"

WifiManager WifiManager::instance;

void WifiManager::begin()
{
    rfcName = config::instance.data.hostName;
    rfcName.trim();

    if (rfcName.isEmpty())
    {
        rfcName = F("ESP-") + String(ESP.getChipId(), HEX);
    }

    rfcName = getRFC952Hostname(rfcName);

    LOG_INFO(F("RFC name is ") << rfcName);

    WiFi.persistent(false);
    WiFi.onStationModeDisconnected(std::bind(&WifiManager::onDisconnect, this, std::placeholders::_1));
    WiFi.setAutoReconnect(true);

    if (!connectSavedWifi())
    {
        // captive portal
        startCaptivePortal();
    }
}

void WifiManager::onDisconnect(const WiFiEventStationModeDisconnected& info)
{
    LOG_INFO(F("WiFi STA disconnected with reason:")<< info.reason);
    checkConnection = true;
}

// Upgraded default waitForConnectResult function to incorporate WL_NO_SSID_AVAIL, fixes issue #122
int8_t WifiManager::waitForConnectResult(unsigned long timeoutLength)
{
    // 1 and 3 have STA enabled
    if ((wifi_get_opmode() & 1) == 0)
    {
        return WL_DISCONNECTED;
    }
    using esp8266::polledTimeout::oneShot;
    oneShot timeout(timeoutLength); // number of milliseconds to wait before returning timeout error
    while (!timeout)
    {
        yield();
        if (WiFi.status() != WL_DISCONNECTED && WiFi.status() != WL_NO_SSID_AVAIL)
        {
            return WiFi.status();
        }
    }
    return -1; // -1 indicates timeout
}

void WifiManager::disconnect(bool disconnectWifi)
{
    WiFi.disconnect(disconnectWifi);
}

// function to forget current WiFi details and start a captive portal
void WifiManager::forget()
{
    disconnect(false);
    startCaptivePortal();

    LOG_INFO(F("Requested to forget WiFi. Started Captive portal."));
}

// function to request a connection to new WiFi credentials
void WifiManager::setNewWifi(const String &newSSID, const String &newPass)
{
    ssid = newSSID;
    pass = newPass;
    reconnect = true;
}

// function to connect to new WiFi credentials
void WifiManager::connectNewWifi(const String &newSSID, const String &newPass)
{
    WiFi.setHostname(rfcName.c_str());

    // fix for auto connect racing issue
    if (!(WiFi.status() == WL_CONNECTED && (WiFi.SSID() == newSSID)))
    {
        // trying to fix connection in progress hanging
        ETS_UART_INTR_DISABLE();
        wifi_station_disconnect();
        ETS_UART_INTR_ENABLE();

        // store old data in case new network is wrong
        const String oldSSID = WiFi.SSID();
        const String oldPSK = WiFi.psk();

        WiFi.begin(newSSID.c_str(), newPass.c_str(), 0, NULL, true);
        delay(2000);

        if (WiFi.waitForConnectResult(timeout) != WL_CONNECTED)
        {
            LOG_ERROR(F("New connection unsuccessful"));
            if (!inCaptivePortal)
            {
                WiFi.begin(oldSSID, oldPSK, 0, NULL, true);
                if (WiFi.waitForConnectResult(timeout) != WL_CONNECTED)
                {
                    LOG_ERROR(F("Reconnection failed too"));
                    startCaptivePortal();
                }
                else
                {
                    LOG_INFO(F("Reconnection successful") << WiFi.localIP());
                    LOG_INFO(F("Connected to new WiFi details with IP: ") << WiFi.localIP());
                    WiFi.setHostname(rfcName.c_str());
                    WiFi.setAutoReconnect(true);
                    WiFi.persistent(true);
                }
            }
        }
        else
        {
            if (inCaptivePortal)
            {
                stopCaptivePortal();
            }

            LOG_INFO(F("New connection successful") << WiFi.localIP());
        }
    }
}

// function to start the captive portal
void WifiManager::startCaptivePortal()
{
    LOG_INFO(F("Opened a captive portal with AP ") << rfcName);

    WiFi.persistent(false);
    // disconnect sta, start ap
    WiFi.disconnect(); //  this alone is not enough to stop the autoconnecter
    WiFi.mode(WIFI_AP);
    WiFi.persistent(true);

    WiFi.softAP(rfcName);

    dnsServer = new DNSServer();

    /* Setup the DNS server redirecting all the domains to the apIP */
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(53, F("*"), WiFi.softAPIP());

    captivePortalStart = millis();
    inCaptivePortal = true;
}

// function to stop the captive portal
void WifiManager::stopCaptivePortal()
{
    WiFi.mode(WIFI_STA);
    delete dnsServer;

    inCaptivePortal = false;
    callChangeListeners();
}

// return captive portal state
bool WifiManager::isCaptivePortal()
{
    return inCaptivePortal;
}

// return current SSID
IPAddress WifiManager::LocalIP()
{
    return WiFi.localIP();
}

String WifiManager::SSID()
{
    return WiFi.SSID();
}

int8_t WifiManager::RSSI()
{
    return WiFi.RSSI();
}

bool WifiManager::connectSavedWifi()
{
    LOG_INFO(F("RFC name is ") << rfcName);

    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);

    if (!WiFi.SSID().isEmpty())
    {
        // trying to fix connection in progress hanging
        ETS_UART_INTR_DISABLE();
        wifi_station_disconnect();
        ETS_UART_INTR_ENABLE();
        WiFi.begin();
    }

    if (waitForConnectResult(timeout) == WL_CONNECTED)
    {
        // connected
        LOG_INFO(F("Connected to stored WiFi details with IP: ") << WiFi.localIP());
        WiFi.setHostname(rfcName.c_str());
        WiFi.setAutoReconnect(true);
        WiFi.persistent(true);
        return true;
    }
    else
    {
        return false;
    }
}

// captive portal loop
void WifiManager::loop()
{
    const uint8_t maxConnectionRetries = 10;
    const uint32_t connectionRetryInterval = 30 * 1000;

    if (inCaptivePortal)
    {
        // captive portal loop
        dnsServer->processNextRequest();

        // only wait for 5 min in portal and then reboot
        if ((millis() - captivePortalStart) > 5 * 60 * 1000)
        { // 5mins
            operations::instance.reboot();
        }
    }

    if (reconnect)
    {
        connectNewWifi(ssid, pass);
        reconnect = false;
    }

    if (!inCaptivePortal)
    {
        // check every connection_retry_interval
        const auto now = millis();
        if ((now - reconnectLastRetry >= connectionRetryInterval) || checkConnection)
        {
            checkConnection = false;
            if (!WiFi.isConnected())
            {
                if (reconnectRetries <= maxConnectionRetries)
                {
                    LOG_INFO(F("Disconnected from wifi, connection retry no") << reconnectRetries);
                    if (!connectSavedWifi())
                    {
                        LOG_INFO(F("Connection to saved wifi failed for retry no:") << reconnectRetries);
                    }
                    else
                    {
                        LOG_INFO(F("Connection to saved wifi succeeded for retry no:") << reconnectRetries);
                    }
                    reconnectRetries++;
                    reconnectLastRetry = millis(); // get the time again to account for time taken to connect to wifi
                }
                else
                {
                    startCaptivePortal();
                }
            }
            else
            {
                // valid connection for connection_retry_interval
                if ((now - reconnectLastRetry >= connectionRetryInterval) && reconnectRetries)
                {
                    LOG_INFO(F("Wifi connection is stable now"));
                    reconnectRetries = 0;
                }
            }
        }
    }
}

String WifiManager::getRFC952Hostname(const String &name)
{
    const int MaxLength = 24;
    String rfc952Hostname;
    rfc952Hostname.reserve(MaxLength);

    for (auto &&c : name)
    {
        if (isalnum(c) || c == '-')
        {
            rfc952Hostname += c;
        }
        if (rfc952Hostname.length() >= MaxLength)
        {
            break;
        }
    }

    // remove last -
    size_t i = rfc952Hostname.length() - 1;
    while (rfc952Hostname[i] == '-' && i > 0)
    {
        i--;
    }

    return rfc952Hostname.substring(0, i);
}
