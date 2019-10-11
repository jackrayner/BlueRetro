#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_bt.h>
#include <nvs_flash.h>
#include "zephyr/atomic.h"
#include "adapter.h"
#include "bt_host.h"
#include "bt_hci.h"
#include "bt_l2cap.h"
#include "bt_hidp_wii.h"
#include "util.h"


#define H4_TRACE /* Display packet dump that can be parsed by wireshark/text2pcap */

#define BT_TX 0
#define BT_RX 1

#define BT_MAX_RETRY 3

typedef void (*bt_cmd_func_t)(void *param);

enum {
    BT_CMD_PARAM_BDADDR,
    BT_CMD_PARAM_HANDLE,
    BT_CMD_PARAM_DEV
};

struct bt_hci_cmd_param {
    bt_cmd_func_t cmd;
    uint32_t param;
};

struct bt_name_type {
    char name[249];
    int8_t type;
};

struct bt_wii_ext_type {
    uint8_t ext_type[6];
    int8_t type;
};

enum {
    /* BT CTRL flags */
    BT_CTRL_READY,
};

enum {
    /* BT device connection flags */
    BT_DEV_DEVICE_FOUND,
    BT_DEV_PAGE,
};

struct bt_hci_pkt bt_hci_pkt_tmp;
static uint32_t bt_pkt_retry = 0;
static uint32_t bt_config_state = 0;
static RingbufHandle_t txq_hdl;
static struct bt_dev bt_dev[7] = {0};
static uint8_t local_bdaddr[6];
static atomic_t bt_flags = 0;
#if 0
static uint16_t min_lx = 0xFFFF;
static uint16_t min_ly = 0xFFFF;
static uint16_t min_rx = 0xFFFF;
static uint16_t min_ry = 0xFFFF;
static uint16_t max_lx = 0;
static uint16_t max_ly = 0;
static uint16_t max_rx = 0;
static uint16_t max_ry = 0;
#endif

const uint8_t led_dev_id_map[] = {
    0x1, 0x2, 0x4, 0x8, 0x3, 0x6, 0xC
};

static const struct bt_name_type bt_name_type[] = {
    {"Nintendo RVL-CNT-01-UC", WIIU_PRO},
    {"Nintendo RVL-CNT-01-TR", WII_CORE},
    {"Nintendo RVL-CNT-01", WII_CORE},
    {"Pro Controller", SWITCH_PRO}
};

static const struct bt_wii_ext_type bt_wii_ext_type[] = {
    {{0x00, 0x00, 0xA4, 0x20, 0x00, 0x00}, WII_NUNCHUCK},
    {{0x00, 0x00, 0xA4, 0x20, 0x01, 0x01}, WII_CLASSIC},
    {{0x01, 0x00, 0xA4, 0x20, 0x01, 0x01}, WII_CLASSIC}, /* Classic Pro */
    {{0x00, 0x00, 0xA4, 0x20, 0x01, 0x20}, WIIU_PRO}
};

#ifdef H4_TRACE
static void bt_h4_trace(uint8_t *data, uint16_t len, uint8_t dir);
#endif /* H4_TRACE */
static void bt_ctrl_rcv_pkt_ready(void);
static int bt_host_rcv_pkt(uint8_t *data, uint16_t len);
static void bt_hci_event_handler(uint8_t *data, uint16_t len);
static void bt_acl_handler(uint8_t *data, uint16_t len);

static esp_vhci_host_callback_t vhci_host_cb = {
    bt_ctrl_rcv_pkt_ready,
    bt_host_rcv_pkt
};

#ifdef H4_TRACE
static void bt_h4_trace(uint8_t *data, uint16_t len, uint8_t dir) {
    uint8_t col;
    uint16_t byte, line;
    uint16_t line_max = len/16;

    if (len % 16)
        line_max++;

    if (dir)
        printf("I ");
    else
        printf("O ");

    for (byte = 0, line = 0; line < line_max; line++) {
        printf("%06X", byte);
        for (col = 0; col < 16 && byte < len; col++, byte++) {
            printf(" %02X", data[byte]);
        }
        printf("\n");
    }
}
#endif /* H4_TRACE */

static int32_t bt_get_new_dev(struct bt_dev **device) {
    for (uint32_t i = 0; i < 7; i++) {
        if (!atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND)) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

static int32_t bt_get_active_dev(struct bt_dev **device) {
    for (uint32_t i = 0; i < 7; i++) {
        if (atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND)) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

static int32_t bt_get_dev_from_bdaddr(bt_addr_t *bdaddr, struct bt_dev **device) {
    for (uint32_t i = 0; i < 7; i++) {
        if (memcmp((void *)bdaddr, bt_dev[i].remote_bdaddr, 6) == 0) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

static int32_t bt_get_dev_from_handle(uint16_t handle, struct bt_dev **device) {
    for (uint32_t i = 0; i < 7; i++) {
        if (bt_acl_handle(handle) == bt_dev[i].acl_handle) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

static int32_t bt_get_dev_from_scid(uint16_t scid, struct bt_dev **device) {
    *device = &bt_dev[(scid & 0xF)];
    return (scid & 0xF);
}

static int32_t bt_get_type_from_name(const uint8_t* name) {
    for (uint32_t i = 0; i < sizeof(bt_name_type)/sizeof(*bt_name_type); i++) {
        if (memcmp(name, bt_name_type[i].name, strlen(bt_name_type[i].name)) == 0) {
            return bt_name_type[i].type;
        }
    }
    return -1;
}

static void bt_host_reset_dev(struct bt_dev *device) {
    memset((void *)device, 0, sizeof(*device));
}

static int32_t bt_get_type_from_wii_ext(const uint8_t* ext_type) {
    for (uint32_t i = 0; i < sizeof(bt_wii_ext_type)/sizeof(*bt_wii_ext_type); i++) {
        if (memcmp(ext_type, bt_wii_ext_type[i].ext_type, sizeof(bt_wii_ext_type[0].ext_type)) == 0) {
            return bt_wii_ext_type[i].type;
        }
    }
    return -1;
}

static int32_t wii_wiiu_ctrl(int8_t type) {
    switch (type) {
        case WII_CORE:
        case WII_NUNCHUCK:
        case WII_CLASSIC:
        case WIIU_PRO:
            return 1;
        default:
            return 0;
    }
}

static struct bt_hci_cp_set_event_filter clr_evt_filter = {
    .filter_type = BT_BREDR_FILTER_TYPE_CLEAR
};

static struct bt_hci_cp_set_event_filter inquiry_evt_filter = {
    .filter_type = BT_BREDR_FILTER_TYPE_INQUIRY,
    .condition_type = BT_BDEDR_COND_TYPE_CLASS,
    .inquiry_class.dev_class = {0x00, 0x05, 0x00},
    .inquiry_class.dev_class_mask = {0x00, 0x1F, 0x00}
};

static struct bt_hci_cp_set_event_filter conn_evt_filter = {
    .filter_type = BT_BREDR_FILTER_TYPE_CONN,
    .condition_type = BT_BDEDR_COND_TYPE_CLASS,
    .conn_class.dev_class = {0x00, 0x05, 0x00},
    .conn_class.dev_class_mask = {0x00, 0x1F, 0x00},
    .conn_class.auto_accept_flag =  BT_BREDR_AUTO_OFF
};

struct bt_hci_cmd_cp {
    bt_cmd_func_t cmd;
    void *cp;
};

static struct bt_hci_cmd_cp bt_hci_config[] =
{
    {bt_hci_cmd_reset, NULL},
    {bt_hci_cmd_read_local_features, NULL},
    {bt_hci_cmd_read_local_version_info, NULL},
    {bt_hci_cmd_read_bd_addr, NULL},
    {bt_hci_cmd_read_buffer_size, NULL},
    {bt_hci_cmd_read_class_of_device, NULL},
    {bt_hci_cmd_read_local_name, NULL},
    {bt_hci_cmd_read_voice_setting, NULL},
    {bt_hci_cmd_read_num_supported_iac, NULL},
    {bt_hci_cmd_read_current_iac_lap, NULL},
    {bt_hci_cmd_set_event_filter, &clr_evt_filter},
    {bt_hci_cmd_write_conn_accept_timeout, NULL},
    {bt_hci_cmd_read_supported_commands, NULL},
    {bt_hci_cmd_write_ssp_mode, NULL},
    {bt_hci_cmd_write_inquiry_mode, NULL},
    {bt_hci_cmd_read_inquiry_rsp_tx_pwr_lvl, NULL},
    {bt_hci_cmd_read_local_ext_features, NULL},
    {bt_hci_cmd_read_stored_link_key, NULL},
    {bt_hci_cmd_read_page_scan_activity, NULL},
    {bt_hci_cmd_read_page_scan_type, NULL},
    {bt_hci_cmd_write_le_host_supp, NULL},
    {bt_hci_cmd_delete_stored_link_key, NULL},
    {bt_hci_cmd_write_class_of_device, NULL},
    {bt_hci_cmd_write_local_name, NULL},
    {bt_hci_cmd_set_event_filter, &inquiry_evt_filter},
    {bt_hci_cmd_set_event_filter, &conn_evt_filter},
    {bt_hci_cmd_write_auth_enable, NULL},
    {bt_hci_cmd_set_event_mask, NULL},
    {bt_hci_cmd_write_page_scan_activity, NULL},
    {bt_hci_cmd_write_inquiry_scan_activity, NULL},
    {bt_hci_cmd_write_page_scan_type, NULL},
    {bt_hci_cmd_write_page_scan_timeout, NULL},
    {bt_hci_cmd_write_hold_mode_act, NULL},
    {bt_hci_cmd_write_scan_enable, NULL},
    {bt_hci_cmd_write_default_link_policy, NULL},
    {bt_hci_cmd_inquiry, NULL}
};

static void bt_host_config_q_cmd(void) {
    if (bt_config_state < ARRAY_SIZE(bt_hci_config)) {
        bt_hci_config[bt_config_state].cmd(bt_hci_config[bt_config_state].cp);
    }
}

static struct bt_hci_cmd_param bt_dev_tx_conn[] =
{
    {bt_hci_cmd_connect, BT_CMD_PARAM_BDADDR},
    {bt_hci_cmd_remote_name_request, BT_CMD_PARAM_BDADDR},
    {bt_hci_cmd_read_remote_features, BT_CMD_PARAM_HANDLE},
    {bt_hci_cmd_read_remote_ext_features, BT_CMD_PARAM_HANDLE},
    {bt_hci_cmd_auth_requested, BT_CMD_PARAM_HANDLE},
    {bt_hci_cmd_set_conn_encrypt, BT_CMD_PARAM_HANDLE},
    {bt_l2cap_cmd_sdp_conn_req, BT_CMD_PARAM_DEV},
    {bt_l2cap_cmd_hid_ctrl_conn_req, BT_CMD_PARAM_DEV},
    {bt_l2cap_cmd_hid_intr_conn_req, BT_CMD_PARAM_DEV},
};

static void bt_host_dev_tx_conn_q_cmd(struct bt_dev *device) {
    if (device->conn_state < ARRAY_SIZE(bt_dev_tx_conn)) {
        switch (bt_dev_tx_conn[device->conn_state].param) {
            case BT_CMD_PARAM_BDADDR:
                bt_dev_tx_conn[device->conn_state].cmd((void *)device->remote_bdaddr);
                break;
            case BT_CMD_PARAM_HANDLE:
                bt_dev_tx_conn[device->conn_state].cmd((void *)&device->acl_handle);
                break;
            case BT_CMD_PARAM_DEV:
                bt_dev_tx_conn[device->conn_state].cmd((void *)device);
                break;
        }
    }
}

static struct bt_hci_cmd_param bt_dev_rx_conn[] =
{
    {bt_hci_cmd_accept_conn_req, BT_CMD_PARAM_BDADDR},
};

static void bt_host_dev_rx_conn_q_cmd(struct bt_dev *device) {
    if (device->conn_state < ARRAY_SIZE(bt_dev_rx_conn)) {
        switch (bt_dev_rx_conn[device->conn_state].param) {
            case BT_CMD_PARAM_BDADDR:
                bt_dev_rx_conn[device->conn_state].cmd((void *)device->remote_bdaddr);
                break;
            case BT_CMD_PARAM_HANDLE:
                bt_dev_rx_conn[device->conn_state].cmd((void *)&device->acl_handle);
                break;
            case BT_CMD_PARAM_DEV:
                bt_dev_rx_conn[device->conn_state].cmd((void *)device);
                break;
        }
    }
}

static void bt_host_dev_conn_q_cmd(struct bt_dev *device) {
    if (atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
        bt_host_dev_rx_conn_q_cmd(device);
    }
    else {
        bt_host_dev_tx_conn_q_cmd(device);
    }
}

static struct bt_hidp_cmd (*bt_hipd_conf[])[8] =
{
    &bt_hipd_wii_conf, /* WII_CORE */
    &bt_hipd_wii_conf, /* WII_NUNCHUCK */
    &bt_hipd_wii_conf, /* WII_CLASSIC */
    &bt_hipd_wii_conf, /* WIIU_PRO */
    NULL, /* SWITCH_PRO */
    NULL, /* PS3_DS3 */
    NULL, /* PS4_DS4 */
    NULL, /* XB1_S */
    NULL, /* HID_PAD */
    NULL, /* HID_KB */
    NULL, /* HID_MOUSE */
};

static void bt_host_dev_hid_q_cmd(struct bt_dev *device) {
    if ((*bt_hipd_conf[device->type])[device->hid_state].cmd) {
        (*bt_hipd_conf[device->type])[device->hid_state].cmd((void *)device, (*bt_hipd_conf[device->type])[device->hid_state].report);
    }
}

static void bt_hci_event_handler(uint8_t *data, uint16_t len) {
    struct bt_dev *device = NULL;
    struct bt_hci_pkt *bt_hci_evt_pkt = (struct bt_hci_pkt *)data;

    switch (bt_hci_evt_pkt->evt_hdr.evt) {
        case BT_HCI_EVT_INQUIRY_COMPLETE:
            printf("# BT_HCI_EVT_INQUIRY_COMPLETE\n");
            if (bt_get_active_dev(&device) == BT_NONE) {
                bt_hci_cmd_inquiry(NULL);
            }
            break;
        case BT_HCI_EVT_INQUIRY_RESULT:
        case BT_HCI_EVT_INQUIRY_RESULT_WITH_RSSI:
        case BT_HCI_EVT_EXTENDED_INQUIRY_RESULT:
        {
            struct bt_hci_evt_inquiry_result *inquiry_result = (struct bt_hci_evt_inquiry_result *)bt_hci_evt_pkt->evt_data;
            printf("# BT_HCI_EVT_INQUIRY_RESULT\n");
            printf("# Number of responce: %d\n", inquiry_result->num_reports);
            for (uint8_t i = 1; i <= inquiry_result->num_reports; i++) {
                bt_get_dev_from_bdaddr((bt_addr_t *)((uint8_t *)&inquiry_result + 1 + 6*(i - 1)), &device);
                if (device == NULL) {
                    int32_t bt_dev_id = bt_get_new_dev(&device);
                    if (device) {
                        memcpy(device->remote_bdaddr, (uint8_t *)inquiry_result + 1 + 6*(i - 1), sizeof(device->remote_bdaddr));
                        device->id = bt_dev_id;
                        device->sdp_chan.scid = bt_dev_id | 0x0070;
                        device->ctrl_chan.scid = bt_dev_id | 0x0080;
                        device->intr_chan.scid = bt_dev_id | 0x0090;
                        atomic_set_bit(&device->flags, BT_DEV_DEVICE_FOUND);
                        bt_hci_cmd_inquiry_cancel(NULL);

                        bt_host_dev_conn_q_cmd(device);
                    }
                }
                printf("# dev: %d Found bdaddr: %02X:%02X:%02X:%02X:%02X:%02X\n", device->id,
                    device->remote_bdaddr[5], device->remote_bdaddr[4], device->remote_bdaddr[3],
                    device->remote_bdaddr[2], device->remote_bdaddr[1], device->remote_bdaddr[0]);
                break; /* Only support one result for now */
            }
            break;
        }
        case BT_HCI_EVT_CONN_COMPLETE:
        {
            struct bt_hci_evt_conn_complete *conn_complete = (struct bt_hci_evt_conn_complete *)bt_hci_evt_pkt->evt_data;
            printf("# BT_HCI_EVT_CONN_COMPLETE\n");
            bt_get_dev_from_bdaddr(&conn_complete->bdaddr, &device);
            if (device) {
                if (conn_complete->status) {
                    device->pkt_retry++;
                    printf("# dev: %d error: 0x%02X\n", device->id, conn_complete->status);
                    if (device->pkt_retry < BT_MAX_RETRY) {
                        bt_host_dev_conn_q_cmd(device);
                    }
                    else {
                        bt_host_reset_dev(device);
                        if (bt_get_active_dev(&device) == BT_NONE) {
                            bt_hci_cmd_inquiry(NULL);
                        }
                    }
                }
                else {
                    device->acl_handle = conn_complete->handle;
                    device->pkt_retry = 0;
                    device->conn_state++;
                    printf("# dev: %d acl_handle: 0x%04X\n", device->id, device->acl_handle);
                    bt_host_dev_conn_q_cmd(device);
                }
            }
            else {
                printf("# dev NULL!\n");
            }
            break;
        }
#ifdef WIP
        case BT_HCI_EVT_CONN_REQUEST:
            printf("# BT_HCI_EVT_CONN_REQUEST\n");
            bt_get_dev_from_bdaddr(&bt_hci_evt_packet->evt_data.conn_request.bdaddr, &device);
            if (device == NULL) {
                int32_t bt_dev_id = bt_get_new_dev(&device);
                if (device) {
                    memcpy(device->remote_bdaddr.val, bt_hci_evt_packet->evt_data.conn_request.bdaddr.val,
                        sizeof(device->remote_bdaddr));
                    device->id = bt_dev_id;
                    device->ctrl_chan.scid = bt_dev_id | 0x0080;
                    device->intr_chan.scid = bt_dev_id | 0x0090;
                    atomic_set_bit(&device->flags, BT_DEV_DEVICE_FOUND);
                    atomic_set_bit(&device->flags, BT_DEV_PAGE);
                    xTaskCreatePinnedToCore(&bt_dev_task, "bt_dev_task", 2048, device, 5, device->xHandle, 0);
                }
            }
            printf("# Page dev: %d bdaddr: %02X:%02X:%02X:%02X:%02X:%02X\n", device->id,
                device->remote_bdaddr.val[5], device->remote_bdaddr.val[4], device->remote_bdaddr.val[3],
                device->remote_bdaddr.val[2], device->remote_bdaddr.val[1], device->remote_bdaddr.val[0]);
            break;
        case BT_HCI_EVT_DISCONN_COMPLETE:
            printf("# BT_HCI_EVT_DISCONN_COMPLETE\n");
            bt_get_dev_from_handle(bt_hci_evt_packet->evt_data.disconn_complete.handle, &device);
            memset(&bt_dev[device->id], 0, sizeof(bt_dev[0]));
            if (bt_get_active_dev(&device) == -1 && xHandle == NULL) {
                printf("# No paired device left, restart inquiry\n");
                xTaskCreatePinnedToCore(&bt_task, "bt_task", 2048, NULL, 5, &xHandle, 0);
            }
            break;
        case BT_HCI_EVT_AUTH_COMPLETE:
            printf("# BT_HCI_EVT_AUTH_COMPLETE\n");
            bt_get_dev_from_handle(bt_hci_evt_packet->evt_data.auth_complete.handle, &device);
            if (bt_hci_evt_packet->evt_data.auth_complete.status) {
                printf("# dev: %d error: 0x%02X\n", device->id,
                    bt_hci_evt_packet->evt_data.auth_complete.status);
            }
            else {
                printf("# dev: %d Pairing done\n", device->id);
                atomic_set_bit(&device->flags, BT_DEV_AUTHENTICATED);
                atomic_clear_bit(&device->flags, BT_DEV_AUTHENTICATING);
            }
            break;
#endif
        case BT_HCI_EVT_REMOTE_NAME_REQ_COMPLETE:
        {
            struct bt_hci_evt_remote_name_req_complete *remote_name_req_complete = (struct bt_hci_evt_remote_name_req_complete *)bt_hci_evt_pkt->evt_data;
            printf("# BT_HCI_EVT_REMOTE_NAME_REQ_COMPLETE:\n");
            bt_get_dev_from_bdaddr(&remote_name_req_complete->bdaddr, &device);
            if (device) {
                if (remote_name_req_complete->status) {
                    device->pkt_retry++;
                    printf("# dev: %d error: 0x%02X\n", device->id, remote_name_req_complete->status);
                    if (device->pkt_retry < BT_MAX_RETRY) {
                        bt_host_dev_conn_q_cmd(device);
                    }
                    else {
                        bt_host_reset_dev(device);
                        if (bt_get_active_dev(&device) == BT_NONE) {
                            bt_hci_cmd_inquiry(NULL);
                        }
                    }
                }
                else {
                    device->type = bt_get_type_from_name(remote_name_req_complete->name);
                    printf("# dev: %d type: %d %s\n", device->id, device->type, remote_name_req_complete->name);
                    device->pkt_retry = 0;
                    device->conn_state++;
                    bt_host_dev_conn_q_cmd(device);
                }
            }
            else {
                printf("# dev NULL!\n");
            }
            break;
        }
#ifdef WIP
        case BT_HCI_EVT_ENCRYPT_CHANGE:
            printf("# BT_HCI_EVT_ENCRYPT_CHANGE\n");
            bt_get_dev_from_handle(bt_hci_evt_packet->evt_data.encrypt_change.handle, &device);
            if (bt_hci_evt_packet->evt_data.encrypt_change.status) {
                printf("# dev: %d error: 0x%02X\n", device->id ? device->id : -1,
                    bt_hci_evt_packet->evt_data.encrypt_change.status);
            }
            else if (device) {
                atomic_set_bit(&device->flags, BT_DEV_ENCRYPT_SET);
                atomic_clear_bit(&device->flags, BT_DEV_PENDING);
            }
            break;
        case BT_HCI_EVT_REMOTE_FEATURES:
            printf("# BT_HCI_EVT_REMOTE_FEATURES\n");
            bt_get_dev_from_handle(bt_hci_evt_packet->evt_data.remote_features.handle, &device);
            if (bt_hci_evt_packet->evt_data.remote_features.status) {
                printf("# dev: %d error: 0x%02X\n", device->id ? device->id : -1,
                    bt_hci_evt_packet->evt_data.remote_features.status);
            }
            else if (device) {
                if (!(bt_hci_evt_packet->evt_data.remote_features.features[8] & 0x80)) {
                    atomic_set_bit(&device->flags, BT_DEV_EXT_FEATURES_READ);
                }
                if (!atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                    atomic_clear_bit(&device->flags, BT_DEV_PENDING);
                }
            }
            break;
#endif
        case BT_HCI_EVT_CMD_COMPLETE:
        {
            struct bt_hci_evt_cmd_complete *cmd_complete = (struct bt_hci_evt_cmd_complete *)bt_hci_evt_pkt->evt_data;
            uint8_t status = bt_hci_evt_pkt->evt_data[sizeof(*cmd_complete)];
            printf("# BT_HCI_EVT_CMD_COMPLETE\n");
            if (status != BT_HCI_ERR_SUCCESS && status != BT_HCI_ERR_UNKNOWN_CMD) {
                printf("# opcode: 0x%04X error: 0x%02X retry: %d\n", cmd_complete->opcode, status, bt_pkt_retry);
                switch (cmd_complete->opcode) {
                    case BT_HCI_OP_READ_BD_ADDR:
                    case BT_HCI_OP_RESET:
                    case BT_HCI_OP_READ_LOCAL_FEATURES:
                    case BT_HCI_OP_READ_LOCAL_VERSION_INFO:
                    case BT_HCI_OP_READ_BUFFER_SIZE:
                    case BT_HCI_OP_READ_CLASS_OF_DEVICE:
                    case BT_HCI_OP_READ_LOCAL_NAME:
                    case BT_HCI_OP_READ_VOICE_SETTING:
                    case BT_HCI_OP_READ_NUM_SUPPORTED_IAC:
                    case BT_HCI_OP_READ_CURRENT_IAC_LAP:
                    case BT_HCI_OP_SET_EVENT_FILTER:
                    case BT_HCI_OP_WRITE_CONN_ACCEPT_TIMEOUT:
                    case BT_HCI_OP_READ_SUPPORTED_COMMANDS:
                    case BT_HCI_OP_WRITE_SSP_MODE:
                    case BT_HCI_OP_WRITE_INQUIRY_MODE:
                    case BT_HCI_OP_READ_INQUIRY_RSP_TX_PWR_LVL:
                    case BT_HCI_OP_READ_LOCAL_EXT_FEATURES:
                    case BT_HCI_OP_READ_STORED_LINK_KEY:
                    case BT_HCI_OP_READ_PAGE_SCAN_ACTIVITY:
                    case BT_HCI_OP_READ_PAGE_SCAN_TYPE:
                    case BT_HCI_OP_LE_WRITE_LE_HOST_SUPP:
                    case BT_HCI_OP_DELETE_STORED_LINK_KEY:
                    case BT_HCI_OP_WRITE_CLASS_OF_DEVICE:
                    case BT_HCI_OP_WRITE_LOCAL_NAME:
                    case BT_HCI_OP_WRITE_AUTH_ENABLE:
                    case BT_HCI_OP_SET_EVENT_MASK:
                    case BT_HCI_OP_WRITE_PAGE_SCAN_ACTIVITY:
                    case BT_HCI_OP_WRITE_INQUIRY_SCAN_ACTIVITY:
                    case BT_HCI_OP_WRITE_PAGE_SCAN_TYPE:
                    case BT_HCI_OP_WRITE_PAGE_TIMEOUT:
                    case BT_HCI_OP_WRITE_HOLD_MODE_ACT:
                    case BT_HCI_OP_WRITE_SCAN_ENABLE:
                    case BT_HCI_OP_WRITE_DEFAULT_LINK_POLICY:
                        bt_pkt_retry++;
                        if (bt_pkt_retry > BT_MAX_RETRY) {
                            bt_pkt_retry = 0;
                            bt_config_state = 0;
                            bt_hci_cmd_reset(NULL);
                        }
                        else {
                            bt_host_config_q_cmd();
                        }
                        break;
                }
            }
            else {
                switch (cmd_complete->opcode) {
                    case BT_HCI_OP_READ_BD_ADDR:
                    {
                        struct bt_hci_rp_read_bd_addr *read_bd_addr = (struct bt_hci_rp_read_bd_addr *)&bt_hci_evt_pkt->evt_data[sizeof(*cmd_complete)];
                        memcpy((void *)local_bdaddr, (void *)&read_bd_addr->bdaddr, sizeof(local_bdaddr));
                        printf("# local_bdaddr: %02X:%02X:%02X:%02X:%02X:%02X\n",
                            local_bdaddr[5], local_bdaddr[4], local_bdaddr[3],
                            local_bdaddr[2], local_bdaddr[1], local_bdaddr[0]);
                    }
                        /* Fall-through */
                    case BT_HCI_OP_RESET:
                    case BT_HCI_OP_READ_LOCAL_FEATURES:
                    case BT_HCI_OP_READ_LOCAL_VERSION_INFO:
                    case BT_HCI_OP_READ_BUFFER_SIZE:
                    case BT_HCI_OP_READ_CLASS_OF_DEVICE:
                    case BT_HCI_OP_READ_LOCAL_NAME:
                    case BT_HCI_OP_READ_VOICE_SETTING:
                    case BT_HCI_OP_READ_NUM_SUPPORTED_IAC:
                    case BT_HCI_OP_READ_CURRENT_IAC_LAP:
                    case BT_HCI_OP_SET_EVENT_FILTER:
                    case BT_HCI_OP_WRITE_CONN_ACCEPT_TIMEOUT:
                    case BT_HCI_OP_READ_SUPPORTED_COMMANDS:
                    case BT_HCI_OP_WRITE_SSP_MODE:
                    case BT_HCI_OP_WRITE_INQUIRY_MODE:
                    case BT_HCI_OP_READ_INQUIRY_RSP_TX_PWR_LVL:
                    case BT_HCI_OP_READ_LOCAL_EXT_FEATURES:
                    case BT_HCI_OP_READ_STORED_LINK_KEY:
                    case BT_HCI_OP_READ_PAGE_SCAN_ACTIVITY:
                    case BT_HCI_OP_READ_PAGE_SCAN_TYPE:
                    case BT_HCI_OP_LE_WRITE_LE_HOST_SUPP:
                    case BT_HCI_OP_DELETE_STORED_LINK_KEY:
                    case BT_HCI_OP_WRITE_CLASS_OF_DEVICE:
                    case BT_HCI_OP_WRITE_LOCAL_NAME:
                    case BT_HCI_OP_WRITE_AUTH_ENABLE:
                    case BT_HCI_OP_SET_EVENT_MASK:
                    case BT_HCI_OP_WRITE_PAGE_SCAN_ACTIVITY:
                    case BT_HCI_OP_WRITE_INQUIRY_SCAN_ACTIVITY:
                    case BT_HCI_OP_WRITE_PAGE_SCAN_TYPE:
                    case BT_HCI_OP_WRITE_PAGE_TIMEOUT:
                    case BT_HCI_OP_WRITE_HOLD_MODE_ACT:
                    case BT_HCI_OP_WRITE_SCAN_ENABLE:
                    case BT_HCI_OP_WRITE_DEFAULT_LINK_POLICY:
                        bt_pkt_retry = 0;
                        bt_config_state++;
                        bt_host_config_q_cmd();
                        break;
                }
            }
            break;
        }
#ifdef WIP
        case BT_HCI_EVT_CMD_STATUS:
            printf("# BT_HCI_EVT_CMD_STATUS\n");
            if (bt_hci_evt_packet->evt_data.cmd_status.status) {
                printf("# opcode: 0x%04X error: 0x%02X\n",
                    bt_hci_evt_packet->evt_data.cmd_status.opcode,
                    bt_hci_evt_packet->evt_data.cmd_status.status);
            }
            else {
                switch (bt_hci_evt_packet->evt_data.cmd_status.opcode) {
                    case BT_HCI_OP_INQUIRY:
                        atomic_set_bit(&bt_flags, BT_CTRL_INQUIRY);
                        break;
                    case BT_HCI_OP_REMOTE_NAME_REQUEST:
                        bt_get_dev_from_pending_flag(&device);
                        if (device) {
                            atomic_set_bit(&device->flags, BT_DEV_NAME_READ);
                            if (atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                                atomic_clear_bit(&device->flags, BT_DEV_PENDING);
                            }
                        }
                        break;
                    case BT_HCI_OP_READ_REMOTE_FEATURES:
                        bt_get_dev_from_pending_flag(&device);
                        if (device) {
                            atomic_set_bit(&device->flags, BT_DEV_FEATURES_READ);
                            if (atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                                atomic_clear_bit(&device->flags, BT_DEV_PENDING);
                            }
                        }
                        break;
                    case BT_HCI_OP_READ_REMOTE_EXT_FEATURES:
                        bt_get_dev_from_pending_flag(&device);
                        if (device) {
                            atomic_set_bit(&device->flags, BT_DEV_EXT_FEATURES_READ);
                            if (atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                                atomic_clear_bit(&device->flags, BT_DEV_PENDING);
                            }
                        }
                        break;
                    case BT_HCI_OP_AUTH_REQUESTED:
                        bt_get_dev_from_pending_flag(&device);
                        if (device) {
                            atomic_set_bit(&device->flags, BT_DEV_AUTHENTICATING);
                            atomic_clear_bit(&device->flags, BT_DEV_PENDING);
                        }
                        break;
                }
                atomic_clear_bit(&bt_flags, BT_CTRL_PENDING);
            }
            break;
        case BT_HCI_EVT_PIN_CODE_REQ:
            printf("# BT_HCI_EVT_PIN_CODE_REQ\n");
            bt_get_dev_from_bdaddr(&bt_hci_evt_packet->evt_data.pin_code_req.bdaddr, &device);
            atomic_set_bit(&device->flags, BT_DEV_PIN_CODE_REQ);
            break;
        case BT_HCI_EVT_LINK_KEY_REQ:
            printf("# BT_HCI_EVT_LINK_KEY_REQ\n");
            bt_get_dev_from_bdaddr(&bt_hci_evt_packet->evt_data.link_key_req.bdaddr, &device);
            atomic_set_bit(&device->flags, BT_DEV_LINK_KEY_REQ);
            break;
        case BT_HCI_EVT_LINK_KEY_NOTIFY:
            printf("# BT_HCI_EVT_LINK_KEY_NOTIFY\n");
            break;
        case BT_HCI_EVT_REMOTE_EXT_FEATURES:
            printf("# BT_HCI_EVT_REMOTE_EXT_FEATURES\n");
            bt_get_dev_from_handle(bt_hci_evt_packet->evt_data.remote_ext_features.handle, &device);
            if (bt_hci_evt_packet->evt_data.remote_ext_features.status) {
                printf("# dev: %d error: 0x%02X\n", device->id ? device->id : -1,
                    bt_hci_evt_packet->evt_data.remote_ext_features.status);
            }
            else if (device) {
                if (!atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                    atomic_clear_bit(&device->flags, BT_DEV_PENDING);
                }
            }
            break;
        case BT_HCI_EVT_IO_CAPA_REQ:
            printf("# BT_HCI_EVT_IO_CAPA_REQ\n");
            bt_get_dev_from_bdaddr(&bt_hci_evt_packet->evt_data.io_capa_req.bdaddr, &device);
            if (device) {
                 atomic_set_bit(&device->flags, BT_DEV_IO_CAP_REQ);
            }
            break;
        case BT_HCI_EVT_USER_CONFIRM_REQ:
            printf("# BT_HCI_EVT_USER_CONFIRM_REQ\n");
            bt_get_dev_from_bdaddr(&bt_hci_evt_packet->evt_data.user_confirm_req.bdaddr, &device);
            if (device) {
                 atomic_set_bit(&device->flags, BT_DEV_USER_CONFIRM_REQ);
            }
            break;
#endif
    }
}

static void bt_acl_handler(uint8_t *data, uint16_t len) {
    struct bt_dev *device = NULL;
    struct bt_hci_acl_packet *bt_hci_acl_packet = (struct bt_hci_acl_packet *)data;

#ifdef WIP
    switch (bt_hci_acl_packet->pl.sig_hdr.code) {
        case BT_L2CAP_CONN_REQ:
            printf("# BT_L2CAP_CONN_REQ\n");
            bt_get_dev_from_handle(bt_hci_acl_packet->acl_hdr.handle, &device);
            device->l2cap_ident = bt_hci_acl_packet->pl.sig_hdr.ident;
            if (atomic_test_bit(&device->flags, BT_DEV_HID_CTRL_CONNECTED)) {
                device->intr_chan.dcid = bt_hci_acl_packet->pl.l2cap_data.conn_req.scid;
            }
            else {
                device->ctrl_chan.dcid = bt_hci_acl_packet->pl.l2cap_data.conn_req.scid;
            }
            atomic_set_bit(&device->flags, BT_DEV_L2CAP_CONN_REQ);
            break;
        case BT_L2CAP_CONN_RSP:
            printf("# BT_L2CAP_CONN_RSP\n");
            bt_get_dev_from_scid(bt_hci_acl_packet->pl.l2cap_data.conn_rsp.scid, &device);
            if (bt_hci_acl_packet->pl.l2cap_data.conn_rsp.result == BT_L2CAP_BR_SUCCESS
                && bt_hci_acl_packet->pl.l2cap_data.conn_rsp.status == BT_L2CAP_CS_NO_INFO) {
                device->l2cap_ident = bt_hci_acl_packet->pl.sig_hdr.ident;
                if (bt_hci_acl_packet->pl.l2cap_data.conn_rsp.scid == device->ctrl_chan.scid) {
                    device->ctrl_chan.dcid = bt_hci_acl_packet->pl.l2cap_data.conn_rsp.dcid;
                }
                else {
                    device->intr_chan.dcid = bt_hci_acl_packet->pl.l2cap_data.conn_rsp.dcid;
                }
                atomic_set_bit(&device->flags, BT_DEV_L2CAP_CONNECTED);
                atomic_clear_bit(&bt_flags, BT_CTRL_PENDING);
                atomic_clear_bit(&device->flags, BT_DEV_PENDING);
            }
            break;
        case BT_L2CAP_CONF_REQ:
            printf("# BT_L2CAP_CONF_REQ\n");
            bt_get_dev_from_scid(bt_hci_acl_packet->pl.l2cap_data.conf_req.dcid, &device);
            device->l2cap_ident = bt_hci_acl_packet->pl.sig_hdr.ident;
            atomic_set_bit(&device->flags, BT_DEV_L2CAP_RCONF_REQ);
            break;
        case BT_L2CAP_CONF_RSP:
            printf("# BT_L2CAP_CONF_RSP\n");
            bt_get_dev_from_scid(bt_hci_acl_packet->pl.l2cap_data.conf_rsp.scid, &device);
            device->l2cap_ident = bt_hci_acl_packet->pl.sig_hdr.ident;
            atomic_set_bit(&device->flags, BT_DEV_L2CAP_LCONF_DONE);
            atomic_clear_bit(&bt_flags, BT_CTRL_PENDING);
            atomic_clear_bit(&device->flags, BT_DEV_PENDING);
            break;
        case BT_L2CAP_DISCONN_REQ:
            printf("# BT_L2CAP_DISCONN_REQ\n");
            break;
        case BT_L2CAP_DISCONN_RSP:
            printf("# BT_L2CAP_DISCONN_RSP\n");
            bt_get_dev_from_handle(bt_hci_acl_packet->acl_hdr.handle, &device);
            atomic_clear_bit(&bt_flags, BT_CTRL_PENDING);
            atomic_clear_bit(&device->flags, BT_DEV_PENDING);
            break;
        case BT_HIDP_DATA_IN:
            bt_get_dev_from_scid(bt_hci_acl_packet->l2cap_hdr.cid, &device);
            switch (bt_hci_acl_packet->pl.hidp.hidp_hdr.protocol) {
                case BT_HIDP_WII_STATUS:
                    printf("# BT_HIDP_WII_STATUS\n");
                    atomic_set_bit(&device->flags, BT_DEV_WII_STATUS_RX);
                    atomic_clear_bit(&device->flags, BT_DEV_WII_EXT_CONF_PENDING);
                    atomic_clear_bit(&device->flags, BT_DEV_WII_EXT_CONF_DONE);
                    atomic_clear_bit(&device->flags, BT_DEV_WII_REP_MODE_SET);
                    if ((bt_hci_acl_packet->pl.hidp.hidp_data.wii_status.flags & BT_HIDP_WII_FLAGS_EXT_CONN) && device->type == WII_CORE) {
                        printf("# dev: %d Skip ext id ext: %d type: %d\n", device->id,
                            (bt_hci_acl_packet->pl.hidp.hidp_data.wii_status.flags & BT_HIDP_WII_FLAGS_EXT_CONN), device->type);
                        atomic_clear_bit(&device->flags, BT_DEV_WII_EXT_ID_READ);
                    }
                    else {
                        atomic_set_bit(&device->flags, BT_DEV_WII_EXT_ID_READ);
                    }
                    if (device->xHandle == NULL) {
                        printf("# dev: %d respawn config thread\n", device->id);
                        xTaskCreatePinnedToCore(&bt_dev_task, "bt_dev_task", 2048, device, 5, device->xHandle, 0);
                    }
                    break;
                case BT_HIDP_WII_RD_DATA:
                    printf("# BT_HIDP_WII_RD_DATA\n");
                    device->type = bt_get_type_from_wii_ext(bt_hci_acl_packet->pl.hidp.hidp_data.wii_rd_data.data);
                    printf("# dev: %d wii ext: %d\n", device->id, device->type);
                    atomic_clear_bit(&device->flags, BT_DEV_PENDING);
                    break;
                case BT_HIDP_WII_ACK:
                    printf("# BT_HIDP_WII_ACK\n");
                    switch(bt_hci_acl_packet->pl.hidp.hidp_data.wii_ack.report) {
                        case BT_HIDP_WII_WR_MEM:
                            atomic_clear_bit(&device->flags, BT_DEV_PENDING);
                            break;
                    }
                    if (bt_hci_acl_packet->pl.hidp.hidp_data.wii_ack.err) {
                        printf("# dev: %d ack err: 0x%02X\n", device->id, bt_hci_acl_packet->pl.hidp.hidp_data.wii_ack.err);
                    }
                    break;
                case BT_HIDP_WII_CORE_ACC_EXT:
                {
#if 0
                    struct wiiu_pro_map *wiiu_pro = (struct wiiu_pro_map *)&bt_hci_acl_packet->pl.hidp.hidp_data.wii_core_acc_ext.ext;
                    device->report_cnt++;
                    input.format = device->type;
                    if (atomic_test_bit(&bt_flags, BT_CTRL_READY) && atomic_test_bit(&input.flags, BTIO_UPDATE_CTRL)) {
                        bt_hid_cmd_wii_set_led(device->acl_handle, device->intr_chan.dcid, input.leds_rumble);
                        atomic_clear_bit(&input.flags, BTIO_UPDATE_CTRL);
                    }
                    memcpy(&input.io.wiiu_pro, wiiu_pro, sizeof(*wiiu_pro));
                    translate_status(sd_config, &input, output);
                    min_lx = min(min_lx, wiiu_pro->axes[0]);
                    min_ly = min(min_ly, wiiu_pro->axes[2]);
                    min_rx = min(min_rx, wiiu_pro->axes[1]);
                    min_ry = min(min_ry, wiiu_pro->axes[3]);
                    max_lx = max(max_lx, wiiu_pro->axes[0]);
                    max_ly = max(max_ly, wiiu_pro->axes[2]);
                    max_rx = max(max_rx, wiiu_pro->axes[1]);
                    max_ry = max(max_ry, wiiu_pro->axes[3]);
                    printf("JG2019 MIN LX 0x%04X LY 0x%04X RX 0x%04X RY 0x%04X\n", min_lx, min_ly, min_rx, min_ry);
                    printf("JG2019 MAX LX 0x%04X LY 0x%04X RX 0x%04X RY 0x%04X\n", max_lx, max_ly, max_rx, max_ry);
                    printf("JG2019 %04X %04X %04X %04X\n", wiiu_pro->axes[0] - 0x800, wiiu_pro->axes[2] - 0x800, wiiu_pro->axes[1] - 0x800, wiiu_pro->axes[3] - 0x800);
#endif
                    break;
                }
            }
            break;
    }
#endif
}

/*
 * @brief: BT controller callback function, used to notify the upper layer that
 *         controller is ready to receive command
 */
static void bt_ctrl_rcv_pkt_ready(void) {
    atomic_set_bit(&bt_flags, BT_CTRL_READY);
}

/*
 * @brief: BT controller callback function, to transfer data packet to upper
 *         controller is ready to receive command
 */
static int bt_host_rcv_pkt(uint8_t *data, uint16_t len) {
#ifdef H4_TRACE
    bt_h4_trace(data, len, BT_RX);
#endif /* H4_TRACE */

    switch(data[0]) {
        case BT_HCI_H4_TYPE_ACL:
            bt_acl_handler(data, len);
            break;
        case BT_HCI_H4_TYPE_EVT:
            bt_hci_event_handler(data, len);
            break;
        default:
            printf("# %s unsupported packet type: 0x%02X\n", __FUNCTION__, data[0]);
            break;
    }

    return 0;
}

#ifdef WIP
static void bt_dev_task(void *param) {
    struct bt_dev *device = (struct bt_dev *)param;

    while (1) {
        if (atomic_test_bit(&bt_flags, BT_CTRL_READY)) {
            if (!atomic_test_bit(&bt_flags, BT_CTRL_PENDING)) {
                if (!atomic_test_bit(&device->flags, BT_DEV_PENDING)) {
                    if (atomic_test_bit(&device->flags, BT_DEV_CONNECTED)) {
                        if (atomic_test_bit(&device->flags, BT_DEV_AUTHENTICATED) || atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                            if (!wii_wiiu_ctrl(device->type) && !atomic_test_bit(&device->flags, BT_DEV_ENCRYPT_SET)) {
                                atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                bt_hci_cmd_set_conn_encrypt(device->acl_handle);
                            }
                            else if (!atomic_test_bit(&device->flags, BT_DEV_SDP_CONNECTED) && !atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                                if (!atomic_test_bit(&device->flags, BT_DEV_L2CAP_CONNECTED)) {
                                    device->l2cap_ident = 0x00;
                                    atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                    bt_l2cap_cmd_conn_req(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, BT_L2CAP_PSM_SDP, device->ctrl_chan.scid);
                                }
                                else if (atomic_test_bit(&device->flags, BT_DEV_L2CAP_RCONF_REQ)) {
                                    bt_l2cap_cmd_conf_rsp(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, device->ctrl_chan.dcid);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_RCONF_REQ);
                                    atomic_set_bit(&device->flags, BT_DEV_L2CAP_RCONF_DONE);
                                }
                                else if (!atomic_test_bit(&device->flags, BT_DEV_L2CAP_LCONF_DONE)) {
                                    device->l2cap_ident++;
                                    atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                    bt_l2cap_cmd_conf_req(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, device->ctrl_chan.dcid);
                                }
                                else if (atomic_test_bit(&device->flags, BT_DEV_L2CAP_RCONF_DONE)) {
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_CONN_REQ);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_CONNECTED);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_LCONF_DONE);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_RCONF_DONE);
                                    atomic_set_bit(&device->flags, BT_DEV_SDP_CONNECTED);
                                    atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                    bt_l2cap_cmd_disconn_req(device->acl_handle, BT_L2CAP_CID_BR_SIG,  device->l2cap_ident, device->ctrl_chan.dcid, device->ctrl_chan.scid);
                                }
                            }
                            else if (!atomic_test_bit(&device->flags, BT_DEV_HID_CTRL_CONNECTED)) {
                                if (!atomic_test_bit(&device->flags, BT_DEV_L2CAP_CONNECTED)) {
                                    if (atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                                        if (atomic_test_bit(&device->flags, BT_DEV_L2CAP_CONN_REQ)) {
                                            bt_l2cap_cmd_conn_rsp(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, device->ctrl_chan.scid, device->ctrl_chan.dcid, 0);
                                            atomic_set_bit(&device->flags, BT_DEV_L2CAP_CONNECTED);
                                        }
                                    }
                                    else {
                                        device->l2cap_ident = 0x00;
                                        atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                        bt_l2cap_cmd_conn_req(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, BT_L2CAP_PSM_HID_CTRL, device->ctrl_chan.scid);
                                    }
                                }
                                else if (atomic_test_bit(&device->flags, BT_DEV_L2CAP_RCONF_REQ)) {
                                    bt_l2cap_cmd_conf_rsp(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, device->ctrl_chan.dcid);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_RCONF_REQ);
                                    atomic_set_bit(&device->flags, BT_DEV_L2CAP_RCONF_DONE);
                                }
                                else if (!atomic_test_bit(&device->flags, BT_DEV_L2CAP_LCONF_DONE)) {
                                    device->l2cap_ident++;
                                    atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                    bt_l2cap_cmd_conf_req(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, device->ctrl_chan.dcid);
                                }
                                else if (atomic_test_bit(&device->flags, BT_DEV_L2CAP_RCONF_DONE)) {
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_CONN_REQ);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_CONNECTED);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_LCONF_DONE);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_RCONF_DONE);
                                    atomic_set_bit(&device->flags, BT_DEV_HID_CTRL_CONNECTED);
                                }
                            }
                            else if (!atomic_test_bit(&device->flags, BT_DEV_HID_INTR_CONNECTED)) {
                                if (!atomic_test_bit(&device->flags, BT_DEV_L2CAP_CONNECTED)) {
                                    if (atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                                        if (atomic_test_bit(&device->flags, BT_DEV_L2CAP_CONN_REQ)) {
                                            bt_l2cap_cmd_conn_rsp(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, device->intr_chan.scid, device->intr_chan.dcid, 0);
                                            atomic_set_bit(&device->flags, BT_DEV_L2CAP_CONNECTED);
                                        }
                                    }
                                    else {
                                        device->l2cap_ident++;
                                        atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                        bt_l2cap_cmd_conn_req(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, BT_L2CAP_PSM_HID_INTR, device->intr_chan.scid);
                                    }
                                }
                                else if (atomic_test_bit(&device->flags, BT_DEV_L2CAP_RCONF_REQ)) {
                                    bt_l2cap_cmd_conf_rsp(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, device->intr_chan.dcid);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_RCONF_REQ);
                                    atomic_set_bit(&device->flags, BT_DEV_L2CAP_RCONF_DONE);
                                }
                                else if (!atomic_test_bit(&device->flags, BT_DEV_L2CAP_LCONF_DONE)) {
                                    device->l2cap_ident++;
                                    atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                    bt_l2cap_cmd_conf_req(device->acl_handle, BT_L2CAP_CID_BR_SIG, device->l2cap_ident, device->intr_chan.dcid);
                                }
                                else if (atomic_test_bit(&device->flags, BT_DEV_L2CAP_RCONF_DONE)) {
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_CONNECTED);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_LCONF_DONE);
                                    atomic_clear_bit(&device->flags, BT_DEV_L2CAP_RCONF_DONE);
                                    atomic_set_bit(&device->flags, BT_DEV_HID_INTR_CONNECTED);
                                }
                            }
                            else if (wii_wiiu_ctrl(device->type)) {
                                /* HID report config */
                                if (atomic_test_bit(&device->flags, BT_DEV_WII_STATUS_RX)) {
                                    if (atomic_test_bit(&device->flags, BT_DEV_WII_EXT_ID_READ)) {
                                        bt_hid_cmd_wii_set_rep_mode(device->acl_handle, device->intr_chan.dcid, 1, BT_HIDP_WII_CORE_ACC_EXT);
                                        atomic_set_bit(&device->flags, BT_DEV_WII_REP_MODE_SET);
                                        device->xHandle = NULL;
                                        vTaskDelete(NULL);
                                    }
                                    else if (!atomic_test_bit(&device->flags, BT_DEV_WII_EXT_CONF_PENDING)) {
                                        struct bt_hidp_wii_wr_mem wii_wr_mem =
                                        {
                                            BT_HIDP_WII_REG,
                                            {0xA4, 0x00, 0xF0},
                                            0x01,
                                            {0x55}
                                        };
                                        atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                        bt_hid_cmd_wii_write(device->acl_handle, device->intr_chan.dcid, &wii_wr_mem);
                                        atomic_set_bit(&device->flags, BT_DEV_WII_EXT_CONF_PENDING);
                                    }
                                    else if (!atomic_test_bit(&device->flags, BT_DEV_WII_EXT_CONF_DONE)) {
                                        struct bt_hidp_wii_wr_mem wii_wr_mem =
                                        {
                                            BT_HIDP_WII_REG,
                                            {0xA4, 0x00, 0xFB},
                                            0x01,
                                            {0x00}
                                        };
                                        atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                        bt_hid_cmd_wii_write(device->acl_handle, device->intr_chan.dcid, &wii_wr_mem);
                                        atomic_set_bit(&device->flags, BT_DEV_WII_EXT_CONF_DONE);
                                    }
                                    else {
                                        struct bt_hidp_wii_rd_mem wii_rd_mem =
                                        {
                                            BT_HIDP_WII_REG,
                                            {0xA4, 0x00, 0xFA},
                                            0x0600,
                                        };
                                        atomic_set_bit(&device->flags, BT_DEV_PENDING);
                                        bt_hid_cmd_wii_read(device->acl_handle, device->intr_chan.dcid, &wii_rd_mem);
                                        atomic_set_bit(&device->flags, BT_DEV_WII_EXT_ID_READ);
                                    }
                                }
                                else if (!atomic_test_bit(&device->flags, BT_DEV_WII_LED_SET)) {
                                    bt_hid_cmd_wii_set_led(device->acl_handle, device->intr_chan.dcid, led_dev_id_map[device->id] << 4);
                                    atomic_set_bit(&device->flags, BT_DEV_WII_LED_SET);
                                }
                                else {
                                    bt_hid_cmd_wii_set_rep_mode(device->acl_handle, device->intr_chan.dcid, 1, BT_HIDP_WII_CORE_ACC_EXT);
                                    atomic_set_bit(&device->flags, BT_DEV_WII_REP_MODE_SET);
                                    device->xHandle = NULL;
                                    vTaskDelete(NULL);
                                }
                            }
                            else if (device->type == SWITCH_PRO) {
                                if (!atomic_test_bit(&device->flags, BT_DEV_WII_LED_SET)) {
                                    bt_hid_cmd_wii_set_led(device->acl_handle, device->intr_chan.dcid, led_dev_id_map[device->id] << 4);
                                    atomic_set_bit(&device->flags, BT_DEV_WII_LED_SET);
                                }
                            }
                        }
                        else if (!atomic_test_bit(&device->flags, BT_DEV_FEATURES_READ)) {
                            atomic_set_bit(&device->flags, BT_DEV_PENDING);
                            bt_hci_cmd_read_remote_features(device->acl_handle);
                        }
                        else if (!atomic_test_bit(&device->flags, BT_DEV_EXT_FEATURES_READ)) {
                            atomic_set_bit(&device->flags, BT_DEV_PENDING);
                            bt_hci_cmd_read_remote_ext_features(device->acl_handle);
                        }
                        else if (!atomic_test_bit(&device->flags, BT_DEV_NAME_READ)) {
                            atomic_set_bit(&device->flags, BT_DEV_PENDING);
                            bt_hci_cmd_remote_name_request(device->remote_bdaddr);
                        }
                        else if (!atomic_test_bit(&device->flags, BT_DEV_AUTHENTICATING)) {
                            atomic_set_bit(&device->flags, BT_DEV_PENDING);
                            bt_hci_cmd_auth_requested(device->acl_handle);
                        }
                        else if (atomic_test_bit(&device->flags, BT_DEV_LINK_KEY_REQ)) {
                            atomic_set_bit(&device->flags, BT_DEV_PENDING);
                            bt_hci_cmd_link_key_neg_reply(device->remote_bdaddr);
                        }
                        else if (atomic_test_bit(&device->flags, BT_DEV_PIN_CODE_REQ)) {
                            atomic_set_bit(&device->flags, BT_DEV_PENDING);
                            //if (device->type == WII_CORE) {
                            //    bt_hci_cmd_pin_code_reply(device->remote_bdaddr, 6, device->remote_bdaddr.val);
                            //}
                            //else {
                                bt_hci_cmd_pin_code_reply(device->remote_bdaddr, 6, local_bdaddr.val);
                            //}
                        }
                        else if (atomic_test_bit(&device->flags, BT_DEV_IO_CAP_REQ)) {
                            atomic_set_bit(&device->flags, BT_DEV_PENDING);
                            bt_hci_cmd_io_capability_reply(&device->remote_bdaddr);
                        }
                        else if (atomic_test_bit(&device->flags, BT_DEV_USER_CONFIRM_REQ)) {
                            atomic_set_bit(&device->flags, BT_DEV_PENDING);
                            bt_hci_cmd_user_confirm_reply(&device->remote_bdaddr);
                        }
                    }
                    else if (atomic_test_bit(&bt_flags, BT_CTRL_INQUIRY)) {
                        bt_hci_cmd_inquiry_cancel();
                    }
                    else {
                        atomic_set_bit(&device->flags, BT_DEV_PENDING);
                        if (atomic_test_bit(&device->flags, BT_DEV_PAGE)) {
                            vTaskDelay(100 / portTICK_PERIOD_MS);
                            bt_hci_cmd_accept_conn_req(&device->remote_bdaddr);
                        }
                        else {
                            bt_hci_cmd_connect(&device->remote_bdaddr);
                        }
                    }
                }
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
#endif

static void bt_tx_ringbuf_task(void *param) {
    size_t packet_len;
    uint8_t *packet;

    while(1) {
        if (atomic_test_bit(&bt_flags, BT_CTRL_READY)) {
            packet = (uint8_t *)xRingbufferReceive(txq_hdl, &packet_len, 0);
            if (packet) {
#ifdef H4_TRACE
                bt_h4_trace(packet, packet_len, BT_TX);
#endif /* H4_TRACE */
                atomic_clear_bit(&bt_flags, BT_CTRL_READY);
                esp_vhci_host_send_packet(packet, packet_len);
                vRingbufferReturnItem(txq_hdl, (void *)packet);
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

int32_t bt_host_init(void) {
    /* Initialize NVS — it is used to store PHY calibration data */
    int32_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        printf("Bluetooth controller initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        printf("Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_vhci_host_register_callback(&vhci_host_cb);

    bt_ctrl_rcv_pkt_ready();

    txq_hdl = xRingbufferCreate(256*8, RINGBUF_TYPE_NOSPLIT);
    if (txq_hdl == NULL) {
        printf("Failed to create ring buffer\n");
        return ret;
    }

    xTaskCreatePinnedToCore(&bt_tx_ringbuf_task, "bt_tx_ringbuf_task", 2048, NULL, 5, NULL, 0);

    bt_host_config_q_cmd();

    return ret;
}

int32_t bt_host_txq_add(uint8_t *packet, uint32_t packet_len) {
    UBaseType_t ret = xRingbufferSend(txq_hdl, (void *)packet, packet_len, 0);
    if (ret != pdTRUE) {
        printf("# %s txq full!\n", __FUNCTION__);
    }
    return (ret == pdTRUE ? 0 : -1);
}
