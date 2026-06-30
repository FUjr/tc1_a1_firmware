#include "user_wifi.h"

#include "main.h"
#include "mico_socket.h"
#include "user_gpio.h"
#include "http_server/web_log.h"
#include "mqtt_server/user_mqtt_client.h"

char wifi_status = WIFI_STATE_NOCONNECT;

mico_timer_t wifi_led_timer;
IpStatus ip_status = { 0, ZZ_AP_LOCAL_IP, ZZ_AP_LOCAL_IP, ZZ_AP_NET_MASK };
static volatile bool wifi_reconnect_thread_running = false;
static volatile bool wifi_startup_ap_window_open = true;

#define WIFI_STARTUP_AP_DELAY_SECONDS 300
#define WIFI_RECONNECT_INTERVAL_SECONDS 10

static bool WifiHasSavedConfig(void)
{
    return sys_config && sys_config->micoSystemConfig.ssid[0] != 0;
}

static void WifiStartStationFromSavedConfig(void)
{
    network_InitTypeDef_st wNetConfig;

    memset(&wNetConfig, 0, sizeof(network_InitTypeDef_st));
    wNetConfig.wifi_mode = Station;
    snprintf(wNetConfig.wifi_ssid, sizeof(wNetConfig.wifi_ssid), "%s",
             sys_config->micoSystemConfig.ssid);
    snprintf((char*)wNetConfig.wifi_key, sizeof(wNetConfig.wifi_key), "%s",
             sys_config->micoSystemConfig.user_key);
    wNetConfig.dhcpMode = DHCP_Client;
    wNetConfig.wifi_retry_interval = 6000;
    micoWlanStart(&wNetConfig);
}

//wifi宸茶繛鎺ヨ幏鍙栧埌IP鍦板潃鍥炶皟
static void WifiGetIpCallback(IPStatusTypedef *pnet, void * arg)
{
    strcpy(ip_status.ip, pnet->ip);
    strcpy(ip_status.gateway, pnet->gate);
    strcpy(ip_status.mask, pnet->mask);

    wifi_log("got IP:%s", pnet->ip);
    wifi_status = WIFI_STATE_CONNECTED;
    wifi_startup_ap_window_open = false;
    //UserFunctionCmdReceived(1,"{\"cmd\":\"device report\"}");
}

//wifi杩炴帴鐘舵€佹敼鍙樺洖璋?
static void WifiStatusCallback(WiFiEvent status, void* arg)
{
    if (status == NOTIFY_STATION_UP) //wifi杩炴帴鎴愬姛
    {
        //user_config->last_wifi_status = status;
        sys_config->micoSystemConfig.reserved = status;
        mico_system_context_update(sys_config);

        OSStatus status = micoWlanSuspendSoftAP(); //鍏抽棴AP
        if (status != kNoErr)
        {
            wifi_log("close ap error[%d]", status);
        }

        ip_status.mode = 1;
        wifi_startup_ap_window_open = false;
        //wifi_status = WIFI_STATE_CONNECTED;
    }
    else if (status == NOTIFY_STATION_DOWN) //wifi鏂紑
    {
        //涓嶄繚瀛楴OTIFY_STATION_DOWN鍒癴lash锛岀‘淇濋噸鍚悗浠嶄細灏濊瘯杩炴帴WiFi
        //sys_config->micoSystemConfig.reserved = status;
        //mico_system_context_update(sys_config);

        wifi_status = WIFI_STATE_NOCONNECT;
        if (!mico_rtos_is_timer_running(&wifi_led_timer))
        {
            mico_rtos_start_timer(&wifi_led_timer);
        }

        //鍚姩WiFi鑷姩閲嶈繛绾跨▼
        WifiStartReconnect(false);
    }
    else if (status == NOTIFY_AP_UP)
    {
        ip_status.mode = 0;
    }
}

bool scaned = false;
char* wifi_ret = NULL;
//wifi鎵弿缁撴灉鍥炶皟
void WifiScanCallback(ScanResult_adv* scan_ret, void* arg)
{
    int count = (int)scan_ret->ApNum;
    wifi_log("wifi_scan_callback ApNum[%d]", count);

    int i = 0;
    int result_size = sizeof(char)*count * (32 + 2) + 50;
    int ssids_size = sizeof(char)*count * 35 + 1;
    int secs_size = sizeof(char)*count * 2 + 1;
    if (wifi_ret)
    {
        free(wifi_ret);
        wifi_ret = NULL;
    }
    wifi_ret = malloc(result_size);
    char* ssids = malloc(ssids_size);
    char* secs = malloc(secs_size);
    if (wifi_ret == NULL || ssids == NULL || secs == NULL)
    {
        if (wifi_ret) free(wifi_ret);
        if (ssids) free(ssids);
        if (secs) free(secs);
        wifi_ret = NULL;
        scaned = false;
        wifi_log("wifi_scan_callback malloc failed");
        return;
    }
    ssids[0] = 0;
    secs[0] = 0;
    char* tmp1 = ssids;
    char* tmp2 = secs;
    int valid_count = 0;
    for (; i < count; i++)
    {
        /*
        ApInfo* ap = (ApInfo*)&scan_ret->ApList[i];
        uint8_t* mac = (uint8_t*)ap->bssid;
        wifi_log("wifi_scan_callback ssid[%16s] bssid[%02X-%02X-%02X-%02X-%02X-%02X] security[%d]",
            ap->ssid, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ap->security);
        */
        char* ssid = scan_ret->ApList[i].ssid;
        //鎺掗櫎闅愯棌鐨剋ifi鍜孲SID甯?鎴?鐨勬垜wifi
        if (ssid[0] == 0 || strstr(ssid, "'") || strstr(ssid, "\"")) continue;
        sprintf(tmp1, "'%s',", ssid);
        tmp1 += (strlen(ssid) + 3);
        sprintf(tmp2, "%d,", scan_ret->ApList[i].security%10);
        tmp2 += 2;
        valid_count++;
    }
    if (valid_count > 0)
    {
        *(--tmp1) = 0;
        *(--tmp2) = 0;
    }

    sprintf(wifi_ret, WIFI_SCAN_RESULT_JSON, 1, ssids, secs);

    scaned = true;
    free(ssids);
    free(secs);
}


//100ms瀹氭椂鍣ㄥ洖璋?
static void WifiLedTimerCallback(void* arg)
{
    static unsigned int num = 0;
    num++;

    switch (wifi_status)
    {
        case WIFI_STATE_FAIL:
            wifi_log("wifi connect fail");
            UserLedSet(0);
            mico_rtos_stop_timer(&wifi_led_timer);
            break;
        case WIFI_STATE_NOCONNECT:
            //wifi_connect_sys_config();
            break;
        case WIFI_STATE_CONNECTING:
            num = 0;
            UserLedSet(-1);
            break;
        case WIFI_STATE_CONNECTED:
            if (!(MQTT_SERVER[0] < 0x20 || MQTT_SERVER[0] > 0x7f || MQTT_SERVER_PORT < 1)){
                UserMqttInit();
            }
            UserLedSet(0);
            mico_rtos_stop_timer(&wifi_led_timer);
            if (RelayOut()&&user_config->power_led_enabled)
                UserLedSet(1);
            else
                UserLedSet(0);
            break;
    }
}

static void WifiReconnectThread(mico_thread_arg_t arg)
{
    bool allow_startup_ap = (bool)arg;
    int elapsed = 0;
    bool ap_started = false;

    while (1)
    {
        if (wifi_status == WIFI_STATE_CONNECTED) break;

        if (!WifiHasSavedConfig())
        {
            wifi_log("WiFi reconnect: no saved SSID, stop retry");
            break;
        }

        wifi_status = WIFI_STATE_CONNECTING;
        wifi_log("WiFi reconnect attempt, elapsed=%ds", elapsed);
        WifiStartStationFromSavedConfig();

        mico_thread_sleep(WIFI_RECONNECT_INTERVAL_SECONDS);
        elapsed += WIFI_RECONNECT_INTERVAL_SECONDS;

        if (allow_startup_ap && wifi_startup_ap_window_open && !ap_started
            && elapsed >= WIFI_STARTUP_AP_DELAY_SECONDS
            && wifi_status != WIFI_STATE_CONNECTED)
        {
            wifi_log("WiFi startup reconnect timeout, start AP without clearing WiFi config");
            ApInit(true);
            ap_started = true;
            wifi_startup_ap_window_open = false;
        }
    }

    wifi_log("WiFi reconnect thread exit, status=%d", wifi_status);
    wifi_reconnect_thread_running = false;
    mico_rtos_delete_thread(NULL);
}

void WifiStartReconnect(bool allow_startup_ap)
{
    if (!WifiHasSavedConfig())
    {
        wifi_log("WiFi reconnect skipped: no saved SSID");
        return;
    }
    if (wifi_reconnect_thread_running)
    {
        wifi_log("WiFi reconnect thread already running");
        return;
    }

    wifi_log("Starting WiFi reconnect thread...");
    wifi_reconnect_thread_running = true;
    if (mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "wifi_reconn",
                                (mico_thread_function_t)WifiReconnectThread,
                                0x800, (mico_thread_arg_t)allow_startup_ap) != kNoErr)
    {
        wifi_reconnect_thread_running = false;
        wifi_log("WiFi reconnect thread create failed");
    }
}

void WifiConnect(char* wifi_ssid, char* wifi_key)
{
    wifi_log("WifiConnect wifi_ssid[%s] wifi_key[******]", wifi_ssid);
    //wifi閰嶇疆鍒濆鍖?
    //淇濆瓨wifi鍙婂瘑鐮佸埌Flash
    snprintf(sys_config->micoSystemConfig.ssid, sizeof(sys_config->micoSystemConfig.ssid), "%s", wifi_ssid);
    snprintf(sys_config->micoSystemConfig.user_key, sizeof(sys_config->micoSystemConfig.user_key), "%s", wifi_key);
    sys_config->micoSystemConfig.user_keyLength = strlen(wifi_key);
    mico_system_context_update(sys_config);
    wifi_status = WIFI_STATE_NOCONNECT;
    WifiStartStationFromSavedConfig();
}

void WifiInit(void)
{
    //wifi鐘舵€佷笅led闂儊瀹氭椂鍣ㄥ垵濮嬪寲
    mico_rtos_init_timer(&wifi_led_timer, 100, (void*)WifiLedTimerCallback, NULL);
    //wifi宸茶繛鎺ヨ幏鍙栧埌IP鍦板潃 鍥炶皟
    mico_system_notify_register(mico_notify_DHCP_COMPLETED, (void*)WifiGetIpCallback, NULL);
    //wifi杩炴帴鐘舵€佹敼鍙樺洖璋?
    mico_system_notify_register(mico_notify_WIFI_STATUS_CHANGED, (void*)WifiStatusCallback, NULL);
    //wifi鎵弿缁撴灉鍥炶皟
    mico_system_notify_register(mico_notify_WIFI_SCAN_ADV_COMPLETED, (void*)WifiScanCallback, NULL);

    //sntp_init();
    //鍚姩瀹氭椂鍣ㄥ紑濮嬭繘琛寃ifi杩炴帴
    if (!mico_rtos_is_timer_running(&wifi_led_timer)) mico_rtos_start_timer(&wifi_led_timer);
}

void WifiStartOnBoot(void)
{
    LinkStatusTypeDef LinkStatus;

    wifi_startup_ap_window_open = true;
    micoWlanGetLinkStatus(&LinkStatus);
    if (LinkStatus.is_connected == 1)
    {
        wifi_status = WIFI_STATE_CONNECTED;
        wifi_startup_ap_window_open = false;
        ip_status.mode = 1;
        return;
    }

    if (WifiHasSavedConfig())
    {
        WifiStartReconnect(true);
    }
    else
    {
        wifi_startup_ap_window_open = false;
        ApInit(true);
    }
}

void ApConfig(char* name, char* key)
{
    strncpy(user_config->ap_name, name, 32);
    strncpy(user_config->ap_key, key, 32);
    user_config->ap_name[31] = 0;
    user_config->ap_key[31] = 0;
    wifi_log("ApConfig ap_name[%s] ap_key[******]", user_config->ap_name);
    wifi_startup_ap_window_open = false;
    micoWlanSuspendStation();
    ApInit(false);
    mico_system_context_update(sys_config);
}

void ApInit(bool use_defaul)
{
    if (use_defaul)
    {
        sprintf(user_config->ap_name, ZZ_AP_NAME, str_mac + 6);
        sprintf(user_config->ap_key, "%s", ZZ_AP_KEY);
        wifi_log("ApInit use_defaul[true] key[]");
    }

    network_InitTypeDef_st wNetConfig;
    memset(&wNetConfig, 0x0, sizeof(network_InitTypeDef_st));
    strcpy((char *)wNetConfig.wifi_ssid, user_config->ap_name);
    strcpy((char *)wNetConfig.wifi_key, user_config->ap_key);
    wNetConfig.wifi_mode = Soft_AP;
    wNetConfig.dhcpMode = DHCP_Server;
    wNetConfig.wifi_retry_interval = 100;
    strcpy((char *)wNetConfig.local_ip_addr, ZZ_AP_LOCAL_IP);
    strcpy((char *)wNetConfig.net_mask, ZZ_AP_NET_MASK);
    strcpy((char *)wNetConfig.dnsServer_ip_addr, ZZ_AP_DNS_SERVER);
    micoWlanStart(&wNetConfig);

    wifi_log("ApInit ssid[%s] key[******]", wNetConfig.wifi_ssid);
}

