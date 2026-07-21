/**
 ******************************************************************************
 * @file    mqtt_client.c
 * @author  Eshen Wang
 * @version V1.0.0
 * @date    16-Nov-2015
 * @brief   MiCO application demonstrate a MQTT client.
 ******************************************************************************
 * @attention
 *
 * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
 * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
 * TIME. AS A RESULT, MXCHIP Inc. SHALL NOT BE HELD LIABLE FOR ANY
 * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
 * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
 * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
 *
 * <h2><center>&copy; COPYRIGHT 2014 MXCHIP Inc.</center></h2>
 ******************************************************************************
 */
#include "http_server/web_log.h"

#include "main.h"
#include "mico.h"
#include "MQTTClient.h"
#include "user_gpio.h"
#include "user_power.h"
#include "user_mqtt_client.h"

typedef struct {
    char topic[MAX_MQTT_TOPIC_SIZE];
    char qos;
    char retained;

    char data[MAX_MQTT_DATA_SIZE];
    uint32_t datalen;
} mqtt_recv_msg_t, *p_mqtt_recv_msg_t, mqtt_send_msg_t, *p_mqtt_send_msg_t;

static void MqttClientThread(mico_thread_arg_t arg);

static void MessageArrived(MessageData *md);

static OSStatus
MqttMsgPublish(Client *c, const char *topic, char qos, char retained, const unsigned char *msg,
               uint32_t msg_len);

OSStatus UserRecvHandler(void *arg);

void ProcessHaCmd(char *cmd);

bool isconnect = false;
mico_queue_t mqtt_msg_send_queue = NULL;

Client c;  // mqtt client object
Network n;  // socket network for mqtt client
volatile bool mqtt_thread_should_exit = false;
static volatile bool mqtt_thread_running = false;
static bool mqtt_worker_initialized = false;

static mico_worker_thread_t mqtt_client_worker_thread; /* Worker thread to manage send/recv events */
//static mico_timed_event_t mqtt_client_send_event;

char topic_state[MAX_MQTT_TOPIC_SIZE];
char topic_set[MAX_MQTT_TOPIC_SIZE];  // 婵烇絽娴傞崰妤呭极閸忚偐鈻旈弶鐐存緲閳诲繘鏌ｉ～顒€濮€妞ゆ梹娲樺鍕炊閳轰緡妫楅柣搴ゎ潐濮樸劌鈻撻幋鐐碘枖濠电姵鍑归弳?

mico_timer_t timer_handle;
static char timer_status = 0;

static bool mqtt_cmd_starts_with(const char *cmd, const char *prefix)
{
    size_t len = strlen(prefix);
    return strncmp(cmd, prefix, len) == 0 && (cmd[len] == 0 || cmd[len] == ' ');
}

void UserMqttTimerFunc(void *arg) {
    LinkStatusTypeDef LinkStatus = { 0 };

    UNUSED_PARAMETER(arg);
    micoWlanGetLinkStatus(&LinkStatus);
    if (LinkStatus.is_connected != 1) {
        mico_stop_timer(&timer_handle);
        return;
    }
    if (mico_rtos_is_queue_empty(&mqtt_msg_send_queue)) {

        switch (timer_status) {
            case 0:
                UserMqttHassAutoLed();
                UserMqttHassAutoTotalSocket();
                UserMqttHassAutoChildLock();
                UserMqttHassAutoRebootButton();
                UserMqttHassAutoSoftRebootButton();
                break;
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
                UserMqttHassAuto(timer_status);
                break;
            case 7:
                UserMqttHassAutoPower();
                break;
            default:
                mico_stop_timer(&timer_handle);
                break;
        }
        timer_status++;
    }
}

OSStatus UserMqttDeInit(void) {
    mqtt_thread_should_exit = true;
    isconnect = false;
    return kNoErr;
}

void clear_mqtt_msg_send_queue(void) {
if(mqtt_msg_send_queue == NULL){
return;
}
    void *msg = NULL;
    while (mico_rtos_is_queue_empty(&mqtt_msg_send_queue) == false) {
        if (mico_rtos_pop_from_queue(&mqtt_msg_send_queue, &msg, 0) == kNoErr) {
            if (msg) free(msg);  // 闂備焦褰冮敃銉╁棘娴ｇ硶妲堥柛顐ゅ枍缁辨牠鏌涢幇顒佸櫣闁宦板姂閺佸秴鐣濋崟顏嗙礆闂佺绻愮粔鐢垫兜閻樺樊鐓?
        }
    }
}

/* Application entrance */
OSStatus UserMqttInit(void) {
    OSStatus err = kNoErr;

    mico_rtos_suspend_all_thread();
    if (mqtt_thread_running) {
        mico_rtos_resume_all_thread();
        return kNoErr;
    }
    mqtt_thread_running = true;
    mqtt_thread_should_exit = false;
    mico_rtos_resume_all_thread();

    sprintf(topic_set, MQTT_CLIENT_SUB_TOPIC1, str_mac);  // 婵炶揪缍€濞夋洟寮ˉ鐭–闂侀潻闄勫妯侯焽閸愨晜濯存繝濞惧亾閻犳劗鍠撻幏瀣箚瑜滃Σ鐢告煛瀹ュ懏绌块柣锔藉灩缁?
    sprintf(topic_state, MQTT_CLIENT_PUB_TOPIC, str_mac);
    //TODO size:0x800
    int mqtt_thread_stack_size = 0x2000;
    uint32_t mqtt_lib_version = MQTTClientLibVersion();mqtt_log(
            "MQTT client version: [%ld.%ld.%ld]",
            0xFF & (mqtt_lib_version >> 16), 0xFF & (mqtt_lib_version >> 8),
            0xFF & mqtt_lib_version);

    if (mqtt_msg_send_queue == NULL) {
        err = mico_rtos_init_queue(&mqtt_msg_send_queue, "mqtt_msg_send_queue",
                                   sizeof(p_mqtt_send_msg_t),
                                   MAX_MQTT_SEND_QUEUE_SIZE);
        require_noerr_action(err, exit,
                             mqtt_log("ERROR: create mqtt msg send queue err=%d.", err));
    }

    /* The receive worker must exist before the MQTT thread can dispatch to it. */
    if (!mqtt_worker_initialized) {
        err = mico_rtos_create_worker_thread(&mqtt_client_worker_thread,
                                             MICO_APPLICATION_PRIORITY, 0x800, 5);
        require_noerr_string(err, exit, "ERROR: Unable to start the mqtt client worker thread.");
        mqtt_worker_initialized = true;
    }

    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "mqtt_client",
                                  (mico_thread_function_t) MqttClientThread,
                                  mqtt_thread_stack_size, 0);
    if (err != kNoErr) goto exit;

    exit:
    if (kNoErr != err) {
        mico_rtos_suspend_all_thread();
        mqtt_thread_running = false;
        mico_rtos_resume_all_thread();
    }
    if (kNoErr != err && !mqtt_worker_initialized && mqtt_msg_send_queue != NULL) {
        clear_mqtt_msg_send_queue();
        mico_rtos_deinit_queue(&mqtt_msg_send_queue);
        mqtt_msg_send_queue = NULL;
    }
    if (kNoErr != err)mqtt_log("ERROR2, app thread exit err: %d kNoErr[%d]", err, kNoErr);
    return err;
}

static OSStatus UserMqttClientRelease(Client *c, Network *n) {
    OSStatus err = kNoErr;

    if (c == NULL || n == NULL) return kParamErr;

    if (c->isconnected) {
        MQTTDisconnect(c);
        c->isconnected = 0;
    }

    if (n->disconnect) {
        n->disconnect(n);
    }

    if (MQTT_SUCCESS != MQTTClientDeinit(c)) {
        mqtt_log("MQTTClientDeinit failed!");
        err = kDeletedErr;
    }
    memset(n, 0, sizeof(Network));

    return err;
}

// publish msg to mqtt server
static OSStatus MqttMsgPublish(Client *c, const char *topic, char qos, char retained,
                               const unsigned char *msg,
                               uint32_t msg_len) {
    OSStatus err = kUnknownErr;
    int ret = 0;
    MQTTMessage publishData = MQTTMessage_publishData_initializer;

    require(topic && msg_len && msg, exit);

    // upload data qos0
    publishData.qos = (enum QoS) qos;
    publishData.retained = retained;
    publishData.payload = (void *) msg;
    publishData.payloadlen = msg_len;

    ret = MQTTPublish(c, topic, &publishData);

    if (MQTT_SUCCESS == ret) {
        err = kNoErr;
    } else if (MQTT_SOCKET_ERR == ret) {
        err = kConnectionErr;
    } else {
        err = kUnknownErr;
    }

    exit:
    return err;
}

void registerMqttEvents(void) {
if(timer_status !=0){
    mico_stop_timer(&timer_handle);
    }
    timer_status = 0;
    mico_start_timer(&timer_handle);
}

void MqttClientThread(mico_thread_arg_t arg) {
    OSStatus err = kUnknownErr;

    int rc = -1;
    fd_set readfds;
    bool timer_initialized = false;

    ssl_opts ssl_settings;
    MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;

    p_mqtt_send_msg_t p_send_msg = NULL;
    int msg_send_event_fd = -1;
    bool no_mqtt_msg_exchange = true;

    UNUSED_PARAMETER(arg);

    mqtt_log("MQTT client thread started...");

    memset(&c, 0, sizeof(c));
    memset(&n, 0, sizeof(n));

    /* create msg send queue event fd */
    msg_send_event_fd = mico_create_event_fd(mqtt_msg_send_queue);
    require_action(msg_send_event_fd >= 0, exit,
                   mqtt_log("ERROR: create msg send queue event fd failed!!!"));

    err = mico_init_timer(&timer_handle, 150, UserMqttTimerFunc, NULL);
    require_noerr_string(err, exit, "ERROR: Unable to initialize MQTT timer.");
    timer_initialized = true;

    MQTT_start:
    clear_mqtt_msg_send_queue();
    UserMqttClientRelease(&c, &n);

    isconnect = false;
    /* 1. create network connection */
    ssl_settings.ssl_enable = false;
    LinkStatusTypeDef LinkStatus = { 0 };
    while (!mqtt_thread_should_exit) {
        isconnect = false;
        mico_rtos_thread_sleep(3);
        if (MQTT_SERVER[0] < 0x20 || MQTT_SERVER[0] > 0x7f || MQTT_SERVER_PORT < 1)
            continue;  //闂備礁鎼悧婊勭椤忓牆鍌ㄩ柕鍫濇川绾鹃箖鏌ｉ姀銏ｅ姌tt闂備礁鎼悧鍡欑矓鐎涙ɑ鍙忛柣鏃傚帶闂傤垶鏌曟繝蹇涙妞は佸啠妲堥柟鎯х－鍟哥紓浣虹帛閻╊垶鐛幒妤佹櫢?

        micoWlanGetLinkStatus(&LinkStatus);
        if (LinkStatus.is_connected != 1) { mqtt_log(
                    "ERROR:WIFI not connect, waiting 3s for connecting and then connecting MQTT ");
            mico_rtos_thread_sleep(3);
            continue;
        }

        rc = NewNetwork(&n, MQTT_SERVER, MQTT_SERVER_PORT, ssl_settings);
        if (rc == MQTT_SUCCESS) break;

        //mqtt_log("ERROR: MQTT network connect err=%d, reconnect after 3s...", rc);
    }
    if (mqtt_thread_should_exit) goto exit;
    mqtt_log("MQTT network connect success!");

    /* 2. init mqtt client */
    //c.heartbeat_retry_max = 2;
    rc = MQTTClientInit(&c, &n, MQTT_CMD_TIMEOUT);
    require_noerr_string(rc, MQTT_reconnect, "ERROR: MQTT client init err.");

    mqtt_log("MQTT client init success!");

    /* 3. create mqtt client connection */
    connectData.willFlag = 0;
    connectData.MQTTVersion = 4;  // 3: 3.1, 4: v3.1.1
    connectData.clientID.cstring = str_mac;
    connectData.username.cstring = user_config->mqtt_user;
    connectData.password.cstring = user_config->mqtt_password;
    connectData.keepAliveInterval = MQTT_CLIENT_KEEPALIVE;
    connectData.cleansession = 1;

    rc = MQTTConnect(&c, &connectData);
    require_noerr_string(rc, MQTT_reconnect, "ERROR: MQTT client connect err.");

    mqtt_log("MQTT client connect success, result: %d ", rc);

    UserLedSet(RelayOut() && user_config->power_led_enabled);

    /* 4. mqtt client subscribe - 闂佺粯绮嶅妯猴耿椤忓嫅鎺曠疀鎼淬劌娈濋柣鐘辩劍閸庢娊藝椤掍礁顕辨慨妯诲墯閸氬倿姊婚崘銊﹀殌闁搞倖绮岄蹇旀綇閳哄倻鏆犳繛鎴炴尭椤兘銆?*/
    rc = MQTTSubscribe(&c, topic_set, QOS0, MessageArrived);
    require_noerr_string(rc, MQTT_reconnect, "ERROR: MQTT client subscribe err.");mqtt_log(
            "MQTT client subscribe success! recv_topic=[%s].", topic_set);
    /* 4.1 mark mqtt connected */
    isconnect = true;

    int i = 0;
    for (; i < SOCKET_NUM; i++) {
        UserMqttSendSocketState(i);
    }

    UserMqttSendLedState();
    UserMqttSendTotalSocketState();
    UserMqttSendChildLockState();

    registerMqttEvents();
    /* 5. client loop for recv msg && keepalive */
    while (!mqtt_thread_should_exit) {
        int max_fd;
        struct timeval t = { MQTT_YIELD_TMIE / 1000,
                             (MQTT_YIELD_TMIE % 1000) * 1000 };

        isconnect = true;
        no_mqtt_msg_exchange = true;
        FD_ZERO(&readfds);
        FD_SET(c.ipstack->my_socket, &readfds);
        FD_SET(msg_send_event_fd, &readfds);
        max_fd = c.ipstack->my_socket > msg_send_event_fd
                 ? c.ipstack->my_socket : msg_send_event_fd;
        select(max_fd + 1, &readfds, NULL, NULL, &t);

        /* recv msg from server */
        if (FD_ISSET(c.ipstack->my_socket, &readfds)) {
            rc = MQTTYield(&c, (int) MQTT_YIELD_TMIE);
            require_noerr(rc, MQTT_reconnect);
            no_mqtt_msg_exchange = false;
        }

        /* recv msg from user worker thread to be sent to server */
        if (FD_ISSET(msg_send_event_fd, &readfds)) {
            while (mico_rtos_is_queue_empty(&mqtt_msg_send_queue) == false) {
                // get msg from send queue
                mico_rtos_pop_from_queue(&mqtt_msg_send_queue, &p_send_msg, 0);
                require_string(p_send_msg, exit, "Wrong data point");

                // send message to server
                err = MqttMsgPublish(&c, p_send_msg->topic, p_send_msg->qos, p_send_msg->retained,
                                     (const unsigned char *) p_send_msg->data,
                                     p_send_msg->datalen);
                free(p_send_msg);
                p_send_msg = NULL;
                require_noerr_string(err, MQTT_reconnect, "ERROR: MQTT publish data err");

                //mqtt_log("MQTT publish data success! send_topic=[%s], msg=[%ld].", p_send_msg->topic, p_send_msg->datalen);
                no_mqtt_msg_exchange = false;
            }
        }

        /* if no msg exchange, we need to check ping msg to keep alive. */
        if (no_mqtt_msg_exchange) {
            rc = keepalive(&c);
            require_noerr_string(rc, MQTT_reconnect, "ERROR: keepalive err");
        }
    }

    MQTT_reconnect:

    if (mqtt_thread_should_exit) goto exit;

mqtt_log("Disconnect MQTT client, and reconnect after 5s, reason: mqtt_rc = %d, err = %d", rc, err);

    timer_status = 100;
    clear_mqtt_msg_send_queue();
    UserMqttClientRelease(&c, &n);
    isconnect = false;
    UserLedSet(-1);
    mico_rtos_thread_msleep(100);
    UserLedSet(-1);
    mico_rtos_thread_sleep(5);
    if (mqtt_thread_should_exit) goto exit;
    goto MQTT_start;

exit:
    isconnect = false;
    mqtt_log("EXIT: MQTT client exit with err = %d.", err);
    if (p_send_msg) {
        free(p_send_msg);
        p_send_msg = NULL;
    }
    if (timer_initialized) {
        mico_stop_timer(&timer_handle);
        mico_deinit_timer(&timer_handle);
    }
    if (msg_send_event_fd >= 0) {
        mico_delete_event_fd(msg_send_event_fd);
    }
    clear_mqtt_msg_send_queue();
    UserMqttClientRelease(&c, &n);
    mico_rtos_suspend_all_thread();
    mqtt_thread_running = false;
    mico_rtos_resume_all_thread();
    mico_rtos_delete_thread(NULL); // 闂佺厧顨庢禍婊堝垂?
    return;
}

// callback, msg received from mqtt server
static void MessageArrived(MessageData *md) {
    OSStatus err = kUnknownErr;
    p_mqtt_recv_msg_t p_recv_msg = NULL;
    MQTTMessage *message = md->message;

    p_recv_msg = (p_mqtt_recv_msg_t) calloc(1, sizeof(mqtt_recv_msg_t));
    require_action(p_recv_msg, exit, err = kNoMemoryErr);

    p_recv_msg->datalen = message->payloadlen >= MAX_MQTT_DATA_SIZE ?
                           MAX_MQTT_DATA_SIZE - 1 : message->payloadlen;
    p_recv_msg->qos = (char) (message->qos);
    p_recv_msg->retained = message->retained;
    int topic_len = md->topicName->lenstring.len >= MAX_MQTT_TOPIC_SIZE ?
                    MAX_MQTT_TOPIC_SIZE - 1 : md->topicName->lenstring.len;
    memcpy(p_recv_msg->topic, md->topicName->lenstring.data, topic_len);
    p_recv_msg->topic[topic_len] = 0;
    memcpy(p_recv_msg->data, message->payload, p_recv_msg->datalen);
    p_recv_msg->data[p_recv_msg->datalen] = 0;

    mqtt_log("MessageArrived topic[%s] data[%s]", p_recv_msg->topic, p_recv_msg->data);
    err = mico_rtos_send_asynchronous_event(&mqtt_client_worker_thread, UserRecvHandler,
                                            p_recv_msg);
    require_noerr(err, exit);

    exit:
    if (err != kNoErr) { mqtt_log("ERROR: Recv data err = %d", err);
        if (p_recv_msg) free(p_recv_msg);
    }
    return;
}

/* Application process MQTT received data */
OSStatus UserRecvHandler(void *arg) {
    OSStatus err = kUnknownErr;
    p_mqtt_recv_msg_t p_recv_msg = arg;
    require(p_recv_msg, exit);

    mqtt_log("user get data success! from_topic=[%s], msg=[%ld].", p_recv_msg->topic,
             p_recv_msg->datalen);
    //UserFunctionCmdReceived(0, p_recv_msg->data);

    ProcessHaCmd(p_recv_msg->data);

    free(p_recv_msg);

    exit:
    return err;
}

void ProcessHaCmd(char *cmd) {
    mqtt_log("ProcessHaCmd[%s]", cmd);
    char mac[20] = {0};

    if (mqtt_cmd_starts_with(cmd, "set socket")) {
        int i, on;
        if (sscanf(cmd, "set socket %19s %d %d", mac, &i, &on) != 3) return;
        if (strcmp(mac, str_mac)) return;mqtt_log("set socket[%d] on[%d]", i, on);
        UserRelaySet(i, on);
        UserMqttSendSocketState(i);
        UserMqttSendTotalSocketState();
        mico_system_context_update(sys_config);
    } else if (mqtt_cmd_starts_with(cmd, "set led")) {
        int on;
        if (sscanf(cmd, "set led %19s %d", mac, &on) != 2) return;
        if (strcmp(mac, str_mac)) return;mqtt_log("set led on[%d]", on);
        user_config->power_led_enabled = on;
        if (RelayOut() && user_config->power_led_enabled) {
            UserLedSet(1);
        } else {
            UserLedSet(0);
        }
        UserMqttSendLedState();
        mico_system_context_update(sys_config);
    } else if (mqtt_cmd_starts_with(cmd, "set total_socket")) {
        int on;
        if (sscanf(cmd, "set total_socket %19s %d", mac, &on) != 2) return;
        if (strcmp(mac, str_mac)) return;mqtt_log("set total_socket on[%d]", on);
        UserRelaySetAll(on);
        int i = 0;
        for (i = 0; i < SOCKET_NUM; i++) {
            UserRelaySet(i, user_config->socket_status[i]);
            UserMqttSendSocketState(i);
        }
        UserMqttSendTotalSocketState();
    }else if (mqtt_cmd_starts_with(cmd, "set childLock")) {
        int on;
        if (sscanf(cmd, "set childLock %19s %d", mac, &on) != 2) return;
        if (strcmp(mac, str_mac)) return;mqtt_log("set childLock on[%d]", on);
        user_config->user[0] = on;
        childLockEnabled = on;
        UserMqttSendChildLockState();
        mico_system_context_update(sys_config);
    }else if (mqtt_cmd_starts_with(cmd, "reboot")) {
        if (sscanf(cmd, "reboot %19s", mac) != 1) return;
        if (strcmp(mac, str_mac)) return;
        MicoSystemReboot();  // 缂備焦鏌ㄩ鍛暤閸℃稒鐓傜€广儱鎳忛崕娆撴偣娴ｇ懓鍔ゆい?
    }else if (mqtt_cmd_starts_with(cmd, "soft_reboot")) {
        if (sscanf(cmd, "soft_reboot %19s", mac) != 1) return;
        if (strcmp(mac, str_mac)) return;
        UserSoftReboot();  // 闁哄鍎愰崹鍫曞闯閹间礁瑙︽い鏍ㄧ箥閸熷骸顭?
    }
}

OSStatus UserMqttSendTopic(char *topic, char *arg, char retained) {
    OSStatus err = kUnknownErr;
    p_mqtt_send_msg_t p_send_msg = NULL;
    if(mqtt_msg_send_queue == NULL|| !isconnect){
    return err;
    }

//  mqtt_log("======App prepare to send ![%d]======", MicoGetMemoryInfo()->free_memory);

    /* Send queue is full, pop the oldest */
    if (mico_rtos_is_queue_full(&mqtt_msg_send_queue) == true) {
        mico_rtos_pop_from_queue(&mqtt_msg_send_queue, &p_send_msg, 0);
        free(p_send_msg);
        p_send_msg = NULL;
    }

    /* Push the latest data into send queue*/
    p_send_msg = (p_mqtt_send_msg_t) calloc(1, sizeof(mqtt_send_msg_t));
    require_action(p_send_msg, exit, err = kNoMemoryErr);

    p_send_msg->qos = 0;
    p_send_msg->retained = retained;
    p_send_msg->datalen = strlen(arg) >= MAX_MQTT_DATA_SIZE ?
                           MAX_MQTT_DATA_SIZE - 1 : strlen(arg);
    memcpy(p_send_msg->data, arg, p_send_msg->datalen);
    p_send_msg->data[p_send_msg->datalen] = 0;
    strncpy(p_send_msg->topic, topic, MAX_MQTT_TOPIC_SIZE - 1);
    p_send_msg->topic[MAX_MQTT_TOPIC_SIZE - 1] = 0;

    err = mico_rtos_push_to_queue(&mqtt_msg_send_queue, &p_send_msg, 0);
    require_noerr(err, exit);

    //mqtt_log("Push user msg into send queue success!");

    exit:
    if (err != kNoErr && p_send_msg) free(p_send_msg);
    return err;
}

/* Application collect data and seng them to MQTT send queue */
OSStatus UserMqttSend(char *arg) {
    return UserMqttSendTopic(topic_state, arg, 0);
}

//闂備礁鎼ú銈夋偤閵娾晛钃熷┑鍌滈兘闁诲孩顔栭崳顖炲箯閻戣姤鐓曢煫鍥ㄦ尵閿涘秴鈹戦鑺ュ€愭鐐╁亾闂?
OSStatus UserMqttSendSocketState(char socket_id) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/socket_%d/state", str_mac, (int) socket_id);
        sprintf(send_buf, "set socket %s %d %d", str_mac, socket_id,
                (int) user_config->socket_status[(int) socket_id]);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

OSStatus UserMqttSendTotalSocketState(void) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/total_socket/state", str_mac);
        sprintf(send_buf, "set total_socket %s %d", str_mac, RelayOut() ? 1 : 0);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

OSStatus UserMqttSendLedState(void) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/led/state", str_mac);
        sprintf(send_buf, "set led %s %d", str_mac, (int) user_config->power_led_enabled);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

OSStatus UserMqttSendChildLockState(void) {
    char *send_buf = malloc(64);
    char *topic_buf = malloc(64);
    OSStatus oss_status = kUnknownErr;
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/childLock/state", str_mac);
        sprintf(send_buf, "set childLock %s %d", str_mac, childLockEnabled);
        oss_status = UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);

    return oss_status;
}

//hass mqtt闂備胶鍘ч〃搴㈢濠婂嫭鍙忛柍鍝勬噹閻鏌熺€电孝缂佺姵鐩弻鈩冩媴閸濆嫷鏆悗瑙勬尫缁€渚€顢氶敐澶嬫櫢濞寸姴顑呯粈鍌炴煕閹邦厼绲荤紒鍙夋そ濮婃椽顢曢浣割伓
void UserMqttHassAuto(char socket_id) {
    socket_id--;
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/socket_%d/config", str_mac, socket_id);
        sprintf(send_buf,
                "{\"name\":\"%s\","
                "\"uniq_id\":\"tc1_%s_s%d\","
                "\"object_id\":\"tc1_%s_s%d\","
                "\"stat_t\":\"homeassistant/switch/%s/socket_%d/state\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_on\":\"set socket %s %d 1\","
                "\"pl_off\":\"set socket %s %d 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                user_config->socket_names[(int)socket_id], str_mac, socket_id,str_mac, socket_id, str_mac, socket_id,
                str_mac,  // 濠电儑缍€椤曆勬叏閻愬灚濯奸柟顖嗗本校MAC闂侀潻闄勫妯侯焽閸愵喖绀嗛柡澶嬪閸ゆ帒霉閻橆喖鈧挾鈧潧鏈敍鎰熺涵鍛箑
                str_mac,
                socket_id, str_mac, socket_id, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

void UserMqttHassAutoRebootButton(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        // 闂備焦褰冪粔鎾箚鎼淬劌绠板鑸靛姈鐏忥箓姊洪弶璺ㄐら柣?
        sprintf(topic_buf, "homeassistant/button/%s/reboot/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"Reboot Device\","
                "\"uniq_id\":\"tc1_%s_reboot\","
                "\"object_id\":\"tc1_%s_reboot\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_prs\":\"reboot %s\","
                "\"device_class\":\"restart\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac, str_mac, sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);
}

void UserMqttHassAutoSoftRebootButton(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        // 闁哄鍎愰崹鍫曞闯閹间礁瑙︽い鏍ㄧ☉閻﹀姊虹粵瀣珝闁告ǜ鍊楃槐?
        sprintf(topic_buf, "homeassistant/button/%s/soft_reboot/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"Soft Reboot\","
                "\"uniq_id\":\"tc1_%s_soft_reboot\","
                "\"object_id\":\"tc1_%s_soft_reboot\","
                "\"cmd_t\":\"device/ztc1/%s/set\","
                "\"pl_prs\":\"soft_reboot %s\","
                "\"device_class\":\"restart\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac, str_mac, sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);
}

void UserMqttHassAutoLed(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/led/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"LED Indicator\","
                "\"uniq_id\":\"tc1_%s_led\","
                "\"object_id\":\"tc1_%s_led\","
                "\"stat_t\":\"homeassistant/switch/%s/led/state\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_on\":\"set led %s 1\","
                "\"pl_off\":\"set led %s 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac,str_mac, str_mac, str_mac, str_mac,str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

void UserMqttHassAutoChildLock(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/childLock/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"Child Lock\","
                "\"uniq_id\":\"tc1_%s_child_lock\","
                "\"object_id\":\"tc1_%s_child_lock\","
                "\"stat_t\":\"homeassistant/switch/%s/childLock/state\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_on\":\"set childLock %s 1\","
                "\"pl_off\":\"set childLock %s 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac,str_mac, str_mac, str_mac, str_mac,str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

void UserMqttHassAutoTotalSocket(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = (char *) malloc(600);
    topic_buf = (char *) malloc(64);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/switch/%s/total_socket/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"Master Switch\","
                "\"uniq_id\":\"tc1_%s_total_socket\","
                "\"object_id\":\"tc1_%s_total_socket\","
                "\"stat_t\":\"homeassistant/switch/%s/total_socket/state\","
                "\"cmd_t\":\"device/ztc1/%s/set\","  // 婵烇絽娴傞崰妤呭极婵傜宸濋柟瀛樺笚婵垹鈽夐幘鎰佺吋妞わ綀濮ょ粋鎺楀Ψ閵夈儳鍩嶉梻浣规緲缁夌兘顢欓弴鐔风窞闁搞儜鍕婵炴垶鎸撮崑鎾绘煛瀹ュ懏绌块柣?
                "\"pl_on\":\"set total_socket %s 1\","
                "\"pl_off\":\"set total_socket %s 0\","
                "\"device_class\":\"outlet\","
                "\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac, str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf)
        free(send_buf);
    if (topic_buf)
        free(topic_buf);
}

//hass mqtt闂備胶鍘ч〃搴㈢濠婂嫭鍙忛柍鍝勬噹閻鏌熺€电孝缂佺姵鐩弻鈩冩媴閸濆嫷鏆悗瑙勬尫缁舵艾鐣峰┑瀣亜闁告繂瀚慨鐢告⒑閸涘﹦鎳勯柛銊ユ健閺佹捇寮妶鍡楊伓
void UserMqttHassAutoPower(void) {
    char *send_buf = NULL;
    char *topic_buf = NULL;
    send_buf = malloc(600);
    topic_buf = malloc(128);
    if (send_buf != NULL && topic_buf != NULL) {
        sprintf(topic_buf, "homeassistant/sensor/%s/power/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"Power\","
                "\"uniq_id\":\"tc1_%s_p\","
                "\"object_id\":\"tc1_%s_p\","
                "\"state_topic\":\"homeassistant/sensor/%s/power/state\","
                "\"unit_of_measurement\":\"W\","
                "\"icon\":\"mdi:gauge\","
                "\"value_template\":\"{{ value_json.power }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
        sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumption/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"Energy Total\","
                "\"uniq_id\":\"tc1_%s_pc\","
                "\"object_id\":\"tc1_%s_pc\","
                "\"state_topic\":\"homeassistant/sensor/%s/powerConsumption/state\","
                "\"unit_of_measurement\":\"kWh\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumption }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);


        sprintf(topic_buf, "homeassistant/sensor/%s/startupTime/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"Uptime\","
                "\"uniq_id\":\"tc1_%s_sut\","
                "\"object_id\":\"tc1_%s_sut\","
                "\"state_topic\":\"homeassistant/sensor/%s/startupTime/state\","
                "\"icon\":\"mdi:clock-time-three-outline\","
                "\"entity_category\":\"diagnostic\","
                "\"value_template\":\"{{ value_json.startupTime }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac, str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);

        sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionToday/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"Energy Today\","
                "\"uniq_id\":\"tc1_%s_pc_today\","
                "\"object_id\":\"tc1_%s_pc_today\","
                "\"state_topic\":\"homeassistant/sensor/%s/powerConsumptionToday/state\","
                "\"unit_of_measurement\":\"kWh\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumptionToday }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);

        sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionYesterday/config", str_mac);
        sprintf(send_buf,
                "{\"name\":\"Energy Yesterday\","
                "\"uniq_id\":\"tc1_%s_pc_yesterday\","
                "\"object_id\":\"tc1_%s_pc_yesterday\","
                "\"state_topic\":\"homeassistant/sensor/%s/powerConsumptionYesterday/state\","
                "\"unit_of_measurement\":\"kWh\","
                "\"icon\":\"mdi:fence-electric\","
                "\"value_template\":\"{{ value_json.powerConsumptionYesterday }}\",""\"device\":{"
                "\"identifiers\":[\"tc1_%s\"],"
                "\"name\":\"%s\","
                "\"model\":\"TC1\","
                "\"manufacturer\":\"PHICOMM\"}}",
                str_mac,str_mac, str_mac, str_mac,sys_config->micoSystemConfig.name);
        UserMqttSendTopic(topic_buf, send_buf, 1);
    }
    if (send_buf) free(send_buf);
    if (topic_buf) free(topic_buf);
}

char topic_buf[128] = {0};
char send_buf[128] = {0};

extern void UserMqttHassPower(void) {
    uint32_t today_count = p_count >= (uint32_t) user_config->p_count_1_day_ago
                           ? p_count - (uint32_t) user_config->p_count_1_day_ago : 0;
    uint32_t yesterday_count = user_config->p_count_1_day_ago >= user_config->p_count_2_days_ago
                               ? (uint32_t) (user_config->p_count_1_day_ago -
                                            user_config->p_count_2_days_ago) : 0;

    sprintf(topic_buf, "homeassistant/sensor/%s/power/state", str_mac);
    sprintf(send_buf, "{\"power\":\"%.3f\"}", real_time_power / 10);
    UserMqttSendTopic(topic_buf, send_buf, 0);

    sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumption/state", str_mac);
    sprintf(send_buf, "{\"powerConsumption\":\"%.3f\"}", PowerPulseCountToKwh(p_count));
    UserMqttSendTopic(topic_buf, send_buf, 0);

    //闁荤姳绶ょ槐鏇㈡偩閼姐倕瀵查柤濮愬€楅崺鐘诲级閳哄倸濮屾い銏″灴瀵噣宕奸弴鐕傜吹
    char up_time[16] = "00:00:00";
    mico_time_t past_ms = 0;
    mico_time_get_time(&past_ms);
    int past = past_ms / 1000;
    int d = past / 3600 / 24;
    int h = past / 3600 % 24;
    int m = past / 60 % 60;
    int s = past % 60;
    sprintf(up_time, "%d - %02d:%02d:%02d", d, h, m, s);

    sprintf(topic_buf, "homeassistant/sensor/%s/startupTime/state", str_mac);
    sprintf(send_buf, "{\"startupTime\":\"%s\"}", up_time);
    UserMqttSendTopic(topic_buf, send_buf, 0);

//    tc1_log("p_count %ld, p_count_1_day_ago %ld ,p_count_2_days_ago %ld, result %ld",p_count,user_config->p_count_1_day_ago,user_config->p_count_2_days_ago,((p_count-user_config->p_count_1_day_ago)<0?0:(p_count-user_config->p_count_1_day_ago)));
    sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionToday/state", str_mac);
    sprintf(send_buf, "{\"powerConsumptionToday\":\"%.3f\"}",
            PowerPulseCountToKwh(today_count));
    UserMqttSendTopic(topic_buf, send_buf, 0);

    sprintf(topic_buf, "homeassistant/sensor/%s/powerConsumptionYesterday/state", str_mac);
    sprintf(send_buf, "{\"powerConsumptionYesterday\":\"%.3f\"}",
            PowerPulseCountToKwh(yesterday_count));
    UserMqttSendTopic(topic_buf, send_buf, 0);
}

bool UserMqttIsConnect() {
    return isconnect;
}
