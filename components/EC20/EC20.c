#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include <cJSON.h>

#include "driver/uart.h"
#include "soc/uart_periph.h"
#include "driver/gpio.h"
#include "Smartconfig.h"
#include "Led.h"
#include "Json_parse.h"
#include "Http.h"

#include "EC20.h"

#define UART1_TXD (GPIO_NUM_21)
#define UART1_RXD (GPIO_NUM_22)

static const char *TAG = "EC20";
TaskHandle_t EC20_Task_Handle;
TaskHandle_t EC20_M_Task_Handle;
TaskHandle_t Uart1_Task_Handle;
TaskHandle_t EC20_Mqtt_Handle;

char *mqtt_recv;
char *EC20_RECV;

#define EC20_SW 25
#define BUF_SIZE 1024

#define EX_UART_NUM UART_NUM_1

QueueHandle_t EC_uart_queue;
QueueHandle_t EC_at_queue;
QueueHandle_t EC_ota_queue;
QueueHandle_t EC_mqtt_queue;

char ICCID[24] = {0};
uint8_t EC_RECV_MODE = EC_NORMAL;

uint16_t file_len = 0;

extern char topic_s[100];
extern char topic_p[100];
extern bool MQTT_E_STA;

void uart_event_task(void *pvParameters);
void EC20_Task(void *arg);
void EC20_M_Task(void *arg);
void EC20_Mqtt_Init_Task(void *arg);

uint8_t EC20_Moudle_Init(void);

//重启EC20网络初始化任务
void Res_EC20_Task(void)
{
    xEventGroupClearBits(Net_sta_group, CONNECTED_BIT);
    xEventGroupClearBits(Net_sta_group, ACTIVED_BIT);
    if ((xEventGroupGetBits(Net_sta_group) & EC20_Task_BIT) != EC20_Task_BIT)
    {
        xTaskCreate(EC20_Task, "EC20_Task", 3072, NULL, 9, &EC20_Task_Handle);
    }
}

//重启EC20 mqtt 初始化任务
void Res_EC20_Mqtt_Task(void)
{
    if ((xEventGroupGetBits(Net_sta_group) & EC20_M_INIT_BIT) != EC20_M_INIT_BIT)
    {
        xTaskCreate(EC20_Mqtt_Init_Task, "EC_M_Init", 2048, NULL, 6, &EC20_Mqtt_Handle);
    }
}

//开启EC20
void EC20_Start(void)
{
    if ((xEventGroupGetBits(Net_sta_group) & Uart1_Task_BIT) != Uart1_Task_BIT)
    {
        xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 20, &Uart1_Task_Handle);
    }

    if ((xEventGroupGetBits(Net_sta_group) & EC20_M_TASK_BIT) != EC20_M_TASK_BIT)
    {
        xTaskCreate(EC20_M_Task, "EC20_M_Task", 3072, NULL, 8, &EC20_M_Task_Handle);
    }

    Res_EC20_Task();
}

//关闭EC20
void EC20_Stop(void)
{
    if ((xEventGroupGetBits(Net_sta_group) & Uart1_Task_BIT) == Uart1_Task_BIT)
    {
        vTaskDelete(Uart1_Task_Handle);
        xEventGroupClearBits(Net_sta_group, Uart1_Task_BIT);
        free(EC20_RECV);
    }

    if ((xEventGroupGetBits(Net_sta_group) & EC20_M_TASK_BIT) == EC20_M_TASK_BIT)
    {
        vTaskDelete(EC20_M_Task_Handle);
        xEventGroupClearBits(Net_sta_group, EC20_M_TASK_BIT);
        free(mqtt_recv);
    }
}

void EC20_Power_On(void)
{
    //开机
    gpio_set_level(EC20_SW, 1); //
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    gpio_set_level(EC20_SW, 0); //
    vTaskDelay(15000 / portTICK_PERIOD_MS);
    // AT_Cmd_Send(NULL, "RDY", 10000, 1);
}

void EC20_Rest(void)
{
    EC20_Power_On();
    if (AT_Cmd_Send("AT\r\n", "OK", 200, 5) == NULL)
    {
        EC20_Power_On();
    }
    else
    {
        Net_ErrCode = NO_ARK;
    }
}

void uart_event_task(void *pvParameters)
{
    xEventGroupSetBits(Net_sta_group, Uart1_Task_BIT);
    uart_event_t event;
    uint16_t all_read_len = 0;
    int header_len = 0;
    EC20_RECV = (char *)malloc(BUF_SIZE);
    char *ret_chr;

    for (;;)
    {
        //Waiting for UART event.
        if (xQueueReceive(EC_uart_queue, (void *)&event, (portTickType)portMAX_DELAY))
        {
            switch (event.type)
            {
            case UART_DATA:
                // ESP_LOGI(TAG, "[UART DATA]: %d", event.size);
                if (event.size > 1)
                {
                    if (all_read_len + event.size >= BUF_SIZE)
                    {
                        ESP_LOGE(TAG, "read len flow");
                        uart_flush(EX_UART_NUM);
                        all_read_len = 0;
                        memset(EC20_RECV, 0, BUF_SIZE);
                        xQueueReset(EC_uart_queue);
                        continue; //此处需返回循环，否则会导致循环错误
                    }
                    uart_read_bytes(EX_UART_NUM, (uint8_t *)EC20_RECV + all_read_len, event.size, portMAX_DELAY);
                    all_read_len += event.size;
                    EC20_RECV[all_read_len] = 0; //去掉字符串结束符，防止字符串拼接不成功
                    //分接收模式处理，为了区别OTA 读取文件差异
                    switch (EC_RECV_MODE)
                    {
                    case EC_NORMAL:
                        if (s_rstrstr(EC20_RECV, all_read_len, 3, "\r\n") != NULL)
                        {
                            xQueueOverwrite(EC_at_queue, (void *)EC20_RECV);
                            if (strstr(EC20_RECV, "+QMTRECV:") != NULL)
                            {
                                // ESP_LOGI("MQTT", "%s\n", EC20_RECV);
                                if (net_mode == NET_4G)
                                {
                                    xQueueOverwrite(EC_mqtt_queue, (void *)EC20_RECV);
                                }
                            }
                            else if (strstr(EC20_RECV, "+QMTSTAT:") != NULL)
                            {
                                ESP_LOGE("MQTT", "%s\n", EC20_RECV);
                                Res_EC20_Task();
                            }
                            else if (strstr(EC20_RECV, "+QMTPING:") != NULL)
                            {
                                ESP_LOGE("MQTT", "%s\n", EC20_RECV);
                                Res_EC20_Task();
                            }
                            // else if (strstr(EC20_RECV, "+CME ERROR:") != NULL)
                            // {
                            //     ESP_LOGE("MQTT", "%s\n", EC20_RECV);
                            //     Res_EC20_Task();
                            // }
                            // else
                            // {
                            //     xQueueOverwrite(EC_at_queue, (void *)EC20_RECV);
                            // }
                            all_read_len = 0;
                            memset(EC20_RECV, 0, BUF_SIZE);
                            uart_flush(EX_UART_NUM);
                        }
                        // else if (strstr((char *)EC20_RECV, ">") != NULL)
                        else if (EC20_RECV[all_read_len - 2] == '>')
                        {
                            xQueueOverwrite(EC_at_queue, (void *)EC20_RECV);
                            // ESP_LOGI("MQTT", "%s\n", EC20_RECV);
                            all_read_len = 0;
                            memset(EC20_RECV, 0, BUF_SIZE);
                            uart_flush(EX_UART_NUM);
                        }
                        break;

                    case EC_OTA:
                        ret_chr = s_rstrstr(EC20_RECV, all_read_len, 30, "\r\n\r\nOK\r\n");
                        if (ret_chr != NULL)
                        {
                            // ESP_LOGI("EC_OTA", "all_read_len=%d\n", all_read_len);
                            ret_chr = s_strstr((const char *)EC20_RECV, 7, NULL, "+QIRD:");
                            if (ret_chr != NULL)
                            {
                                file_len = (uint16_t)strtoul(ret_chr, 0, 10);
                                ret_chr = s_strstr(ret_chr, 7, NULL, "\r\n");
                                header_len = (int)(ret_chr - EC20_RECV);
                                // ESP_LOGI("EC_OTA", "file_len=%d,header_len=%d\n", file_len, header_len);
                                if ((all_read_len - header_len - 8) >= file_len)
                                {
                                    // ESP_LOGI("EC_OTA", "OK");
                                    xQueueOverwrite(EC_at_queue, (void *)ret_chr);
                                    all_read_len = 0;
                                    memset(EC20_RECV, 0, BUF_SIZE);
                                    uart_flush(EX_UART_NUM);
                                }
                            }
                            // rst_val = mid(rst_val, "+QIRD: ", "\r\n", num_buff);
                        }

                    default:
                        break;
                    }
                }

                break;

            case UART_FIFO_OVF:
                ESP_LOGI(TAG, "hw fifo overflow");
                uart_flush_input(EX_UART_NUM);
                xQueueReset(EC_uart_queue);
                break;

            case UART_BUFFER_FULL:
                ESP_LOGI(TAG, "ring buffer full");
                uart_flush_input(EX_UART_NUM);
                xQueueReset(EC_uart_queue);
                break;

            //Others
            default:
                ESP_LOGI(TAG, "uart type: %d", event.type);
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

void EC20_Init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_RTS,
        .source_clk = UART_SCLK_APB,
    };
    //Install UART driver, and get the queue.
    uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, 0, 2, &EC_uart_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);
    uart_set_pin(EX_UART_NUM, UART1_TXD, UART1_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // //uart2 switch io
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1 << EC20_SW);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    // EC20_at_Binary = xSemaphoreCreateBinary();
    EC_at_queue = xQueueCreate(1, BUF_SIZE);
    EC_mqtt_queue = xQueueCreate(1, BUF_SIZE);

    if (net_mode == NET_4G)
    {
        EC20_Start();
    }
}

void EC20_Task(void *arg)
{
    xEventGroupSetBits(Net_sta_group, EC20_Task_BIT);
    uint8_t ret;
    while (net_mode == NET_4G)
    {
        ESP_LOGI(TAG, "EC20_Task START");

        MQTT_E_STA = false;
        Net_sta_flag = false;
        xEventGroupClearBits(Net_sta_group, CONNECTED_BIT);
        xEventGroupClearBits(Net_sta_group, ACTIVED_BIT);
        Start_Active();
        ret = EC20_Moudle_Init();
        if (ret == 0)
        {
            continue;
        }
        ret = EC20_Http_CFG();
        if (ret == 0)
        {
            continue;
        }

        xEventGroupSetBits(Net_sta_group, CONNECTED_BIT);
        Res_EC20_Mqtt_Task();

        if (ret == 1)
        {
            break;
        }

        // vTaskSuspend(NULL);
    }
    ESP_LOGI(TAG, "EC20_Task DELETE");
    xEventGroupClearBits(Net_sta_group, EC20_Task_BIT);
    vTaskDelete(NULL);
}

/**********************************************************/
void EC20_M_Task(void *arg)
{
    xEventGroupSetBits(Net_sta_group, EC20_M_TASK_BIT);
    mqtt_recv = (char *)malloc(1024);
    while (1)
    {
        memset(mqtt_recv, 0, 1024);
        if (xQueueReceive(EC_mqtt_queue, (void *)mqtt_recv, -1) == pdPASS)
        {
            parse_objects_mqtt(mqtt_recv, true);
        }
    }
}

/*******************************************************************************
//Check AT Command Respon result，
// 
*******************************************************************************/
char *AT_Cmd_Send(char *cmd_buf, char *check_buff, uint32_t time_out, uint8_t try_num)
{
    xSemaphoreTake(EC20_muxtex, -1);
    char *rst_val = NULL;
    uint8_t i, j;

    uint8_t *recv_buf = (uint8_t *)malloc(1024);

    for (i = 0; i < try_num; i++)
    {
        if (cmd_buf != NULL)
        {
            uart_write_bytes(EX_UART_NUM, cmd_buf, strlen(cmd_buf));
        }

        for (j = 0; j < 10; j++)
        {
            if (xQueueReceive(EC_at_queue, (void *)recv_buf, time_out / portTICK_RATE_MS) == pdPASS)
            {
                rst_val = strstr((char *)recv_buf, check_buff); //
                if (rst_val != NULL)
                {
                    break;
                }
            }
            else //未等到数据
            {

                break;
            }
        }

        if (rst_val != NULL)
        {
            break;
        }
    }

    // vTaskDelay(100 / portTICK_PERIOD_MS);
    free(recv_buf);
    xSemaphoreGive(EC20_muxtex);

    return rst_val; //
}

uint8_t EC20_Net_Check(void)
{
    uint8_t ret = 0;
    for (uint8_t i = 0; i < 15; i++)
    {
        // ESP_LOGI(TAG, "Net_Check");
        if (AT_Cmd_Send("AT+QIACT?\r\n", "+QIACT: 1,1", 100, 1) != NULL)
        {
            ret = 1;
            break;
        }
        if (AT_Cmd_Send("AT+QIACT=1\r\n", "OK", 1000, 1) == NULL)
        {
            ESP_LOGE(TAG, "EC20_Http_CFG %d", __LINE__);
        }
    }

    // ESP_LOGI(TAG, "Net_Check: %d", ret);
    if (ret == 0) //重启
    {
        Res_EC20_Task();
    }

    return ret;
}

//EC20 init
uint8_t EC20_Moudle_Init(void)
{
    char *ret;
    char *cmd_buf = (char *)malloc(120);

    EC20_Rest();

    ret = AT_Cmd_Send("AT\r\n", "OK", 1000, 5);
    if (ret == NULL)
    {
        Net_ErrCode = NO_ARK;
        ESP_LOGE(TAG, "%d", __LINE__);
        return 0;
    }

    ret = AT_Cmd_Send("ATE0\r\n", "OK", 1000, 5); //回显
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "ATE0  ");
        return 0;
    }

    ret = AT_Cmd_Send("AT+IPR=115200\r\n", "OK", 1000, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Init %d", __LINE__);
        goto end;
    }

    ret = AT_Cmd_Send("AT+CPIN?\r\n", "READY", 1000, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Init %d", __LINE__);
        Net_ErrCode = CPIN_ERR;
        goto end;
    }

    ret = AT_Cmd_Send("AT+QCCID\r\n", "+QCCID:", 1000, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Init %d", __LINE__);
        Net_ErrCode = CPIN_ERR;
        goto end;
    }
    else
    {
        memcpy(ICCID, ret + 8, 20);
        ESP_LOGI(TAG, "ICCID=%s", ICCID);
    }

    sprintf(cmd_buf, "AT+QICSGP=1,1,\"%s\",\"%s\",\"%s\",1\r\n", SIM_APN, SIM_USER, SIM_PWD);
    ret = AT_Cmd_Send(cmd_buf, "OK", 1000, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Init %d", __LINE__);
        Net_ErrCode = QICSGP_ERR;
        goto end;
    }

    ret = AT_Cmd_Send("AT+CGATT=1\r\n", "OK", 1000, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Init %d", __LINE__);
        Net_ErrCode = CGATT_ERR;
        goto end;
    }

    ret = AT_Cmd_Send("AT+CGATT?\r\n", "+CGATT: 1", 1000, 10);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Init %d", __LINE__);
        Net_ErrCode = CGATT_ERR;
        goto end;
    }

    ret = AT_Cmd_Send("AT+QIACT?\r\n", "+QIACT: 1,1", 1000, 5);
    if (ret != NULL)
    {
        // ESP_LOGI(TAG, "EC20_Http_CFG %d", __LINE__);
        goto end;
    }

    ret = AT_Cmd_Send("AT+QIACT=1\r\n", "OK", 1000, 10);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Http_CFG %d", __LINE__);
        Net_ErrCode = QIACT_ERR;
        goto end;
    }

    ret = AT_Cmd_Send("AT+QIACT?\r\n", "+QIACT: 1,1", 1000, 5);
    if (ret != NULL)
    {
        // ESP_LOGI(TAG, "EC20_Http_CFG %d", __LINE__);
        goto end;
    }

end:
    // free(active_url);
    free(cmd_buf);
    if (ret == NULL)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

//EC20 MQTT INIT
void EC20_Mqtt_Init_Task(void *arg)
{
    xEventGroupSetBits(Net_sta_group, EC20_M_INIT_BIT);
    uint8_t ret;
    while (1)
    {
        xEventGroupWaitBits(Net_sta_group, ACTIVED_BIT | MQTT_INITED_BIT, false, true, -1); //等待激活
        if (net_mode != NET_4G)
        {
            MQTT_E_STA = false;
            break;
        }

        xSemaphoreTake(xMutex_Http_Send, -1);
        ret = EC20_MQTT_INIT();
        xSemaphoreGive(xMutex_Http_Send);
        if (ret == 0)
        {
            Res_EC20_Task();
        }
        else
        {
            MQTT_E_STA = true;
            break;
        }
    }
    xEventGroupClearBits(Net_sta_group, EC20_M_INIT_BIT);
    vTaskDelete(NULL);
}

uint8_t EC20_Http_CFG(void)
{
    char *ret;

    ret = AT_Cmd_Send("AT+QHTTPCFG=\"contextid\",1\r\n", "OK", 1000, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Http_CFG %d", __LINE__);
        goto end;
    }

    ret = AT_Cmd_Send("AT+QHTTPCFG=\"responseheader\",0\r\n", "OK", 1000, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Http_CFG %d", __LINE__);
        goto end;
    }

    // ret = AT_Cmd_Send("AT+QHTTPCFG=\"closewaittime\",0\r\n", "OK", 100, 1);
    // if (ret == NULL)
    // {
    //     ESP_LOGE(TAG, "EC20_Http_CFG %d", __LINE__);
    //     // goto end; //兼容EC200T
    // }

    // ret = AT_Cmd_Send("AT+QIACT?\r\n", "+QIACT: 1,1", 100, 5);
    // if (ret != NULL)
    // {
    //     // ESP_LOGI(TAG, "EC20_Http_CFG %d", __LINE__);
    //     goto end;
    // }

    // ret = AT_Cmd_Send("AT+QIACT=1\r\n", "OK", 100, 5);
    // if (ret == NULL)
    // {
    //     ESP_LOGE(TAG, "EC20_Http_CFG %d", __LINE__);
    //     Net_ErrCode = QIACT_ERR;
    //     goto end;
    // }

    // ret = AT_Cmd_Send("AT+QIACT?\r\n", "+QIACT: 1,1", 100, 5);
    // if (ret != NULL)
    // {
    //     // ESP_LOGI(TAG, "EC20_Http_CFG %d", __LINE__);
    //     Net_ErrCode = QIACT_ERR;
    //     goto end;
    // }

end:
    // free(active_url);

    if (ret == NULL)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

uint8_t EC20_Active(char *active_url, char *recv_buf)
{
    char *ret;
    char *cmd_buf;
    uint8_t active_len;
    cmd_buf = (char *)malloc(24);
    memset(cmd_buf, 0, 24);
    active_len = strlen(active_url) - 2; //不包含换行符
    sprintf(cmd_buf, "AT+QHTTPURL=%d,60\r\n", active_len);
    ret = AT_Cmd_Send(cmd_buf, "CONNECT", 1000, 1);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Active %d", __LINE__);
        goto end;
    }
    ret = AT_Cmd_Send(active_url, "OK", 60000, 1);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Active %d", __LINE__);
        goto end;
    }

    ret = AT_Cmd_Send("AT+QHTTPGET=60\r\n", "+QHTTPGET: 0,200", 6000, 1);
    if (ret == NULL)
    {
        // Res_EC20_Task();
        ESP_LOGE(TAG, "EC20_Active %d", __LINE__);
        goto end;
    }

    ret = AT_Cmd_Send("AT+QHTTPREAD=60\r\n", "CONNECT", 6000, 1);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Active %d", __LINE__);
        goto end;
    }
    memcpy(recv_buf, ret, BUF_SIZE);

end:
    free(cmd_buf);
    if (ret == NULL)
    {
        // Res_EC20_Task();
        return 0;
    }
    else
    {
        return 1;
    }
}

uint8_t EC20_Send_Post_Data(char *post_buf, bool end_flag)
{
    uart_write_bytes(EX_UART_NUM, post_buf, strlen(post_buf));
    if (end_flag == true)
    {
        if (AT_Cmd_Send("\r\n", "+QHTTPPOST: 0,200", 6000, 1) == NULL)
        {
            ESP_LOGE(TAG, "EC20_Post %d", __LINE__);
            // Res_EC20_Task();
            return 0;
        }
    }
    return 1;
}

uint8_t EC20_Read_Post_Data(char *recv_buff, uint16_t buff_size)
{
    char *rst_val;
    rst_val = AT_Cmd_Send("AT+QHTTPREAD=60\r\n", "CONNECT", 1000, 1);
    if (rst_val == NULL)
    {
        ESP_LOGE(TAG, "EC20_read %d", __LINE__);
        // Res_EC20_Task();
        return 0;
    }
    memcpy(recv_buff, rst_val, buff_size);
    return 1;
}

#define CMD_LEN 150
uint8_t EC20_MQTT_INIT(void)
{
    char *ret;
    char *cmd_buf;
    cmd_buf = (char *)malloc(CMD_LEN);

    ret = AT_Cmd_Send("AT+QMTOPEN?\r\n", "+QMTOPEN: 0,", 1000, 5);
    if (ret != NULL)
    {
        ESP_LOGI(TAG, "EC20_MQTT already open");
        goto end;
    }

    ret = AT_Cmd_Send("AT+QMTCFG=\"version\",0,3\r\n", "OK", 1000, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_MQTT %d", __LINE__);
        goto end;
    }

    ret = AT_Cmd_Send("AT+QMTCFG=\"recv/mode\",0,0,0\r\n", "OK", 1000, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_MQTT %d", __LINE__);
        goto end;
    }

    ret = AT_Cmd_Send("AT+QMTCFG=\"keepalive\",0,120\r\n", "OK", 1000, 1);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_MQTT %d", __LINE__);
        goto end;
    }

    memset(cmd_buf, 0, CMD_LEN);
    sprintf(cmd_buf, "AT+QMTOPEN=0,\"%s\",%s\r\n", MQTT_SERVER, MQTT_PORT);
    ret = AT_Cmd_Send(cmd_buf, "+QMTOPEN: 0,0", 6000, 10);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_MQTT %d", __LINE__);
        goto end;
    }

    memset(cmd_buf, 0, CMD_LEN);
    sprintf(cmd_buf, "AT+QMTCONN=0,\"%s\",\"c_id=%s\",\"api_key=%s\"\r\n", USER_ID, ChannelId, ApiKey);
    ret = AT_Cmd_Send(cmd_buf, "+QMTCONN: 0,0,0", 6000, 1);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_MQTT %d", __LINE__);
        goto end;
    }

    memset(cmd_buf, 0, CMD_LEN);
    sprintf(cmd_buf, "AT+QMTSUB=0,1,\"%s\",0\r\n", topic_s);
    ret = AT_Cmd_Send(cmd_buf, "+QMTSUB: ", 6000, 1);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_MQTT %d", __LINE__);
        goto end;
    }

end:
    // free(active_url);
    free(cmd_buf);
    if (ret == NULL)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

//data_buff 需要包含 \r\n
uint8_t EC20_MQTT_PUB(char *data_buff)
{
    char *ret;
    char *cmd_buf;
    cmd_buf = (char *)malloc(CMD_LEN);
    memset(cmd_buf, 0, CMD_LEN);
    // ESP_LOGI(TAG, "MQTT LEN:%d,\n%s", strlen(data_buff), data_buff);
    sprintf(cmd_buf, "AT+QMTPUBEX=0,0,0,0,\"%s\",%d\r\n", topic_p, strlen(data_buff) - 2);
    ret = AT_Cmd_Send(cmd_buf, ">", 1000, 1);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_MQTT %d", __LINE__);
        goto end;
    }

    ret = AT_Cmd_Send(data_buff, "+QMTPUBEX: 0,0,0", 1000, 1);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_MQTT %d", __LINE__);
        goto end;
    }

end:
    // free(active_url);
    free(cmd_buf);
    if (ret == NULL)
    {
        // Res_EC20_Task();
        return 0;
    }
    else
    {
        return 1;
    }
}

//获取信号值
uint8_t EC20_Get_Rssi(float *Rssi_val)
{
    xSemaphoreTake(xMutex_Http_Send, -1);
    char *ret_val;
    char *temp_buf;
    char *InpString;
    temp_buf = (char *)malloc(15);

    ret_val = AT_Cmd_Send("AT+CSQ\r\n", "+CSQ", 1000, 1);
    if (ret_val == NULL)
    {
        ESP_LOGE(TAG, "%d", __LINE__);
        goto end;
    }

    memcpy(temp_buf, ret_val, 15);
    InpString = strtok(temp_buf, ":"); //+CSQ: 24,5
    InpString = strtok(NULL, ",");
    *Rssi_val = ((uint8_t)strtoul(InpString, 0, 10)) * 2 - 113; //GPRS Signal Quality

end:
    free(temp_buf);
    xSemaphoreGive(xMutex_Http_Send);
    return *Rssi_val;
}

bool End_EC_TCP_OTA(void)
{
    char *ret;
    Res_EC20_Mqtt_Task();
    ret = AT_Cmd_Send("AT+QICLOSE=0\r\n", "OK", 1000, 1);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, " %d", __LINE__);
        return false;
    }
    return true;
}

//tcp 模式OTA
bool Start_EC20_TCP_OTA(void)
{
    char *cmd_buf = (char *)malloc(256);
    // char *get_buf = (char *)malloc(256);
    char *host_buf = (char *)malloc(128);
    char *rst_val = NULL;
    // uint8_t *recv_buf = (uint8_t *)malloc(BUF_SIZE);

    //获取HOST
    memset(host_buf, 0, 128);
    if (mid(mqtt_json_s.mqtt_ota_url, "://", "/", host_buf) == NULL)
    {
        ESP_LOGE(TAG, "%d", __LINE__);
        goto end;
    }
    ESP_LOGI(TAG, "host:%s,len:%d", host_buf, strlen(host_buf));

    //关闭MQTT
    rst_val = AT_Cmd_Send("AT+QMTCLOSE=0\r\n", "+QMTCLOSE:", 1000, 1);
    if (rst_val == NULL)
    {
        ESP_LOGE(TAG, "%d", __LINE__);
        goto end;
    }

    //建立TCP连接
    sprintf(cmd_buf, "AT+QIOPEN=1,0,\"TCP\",\"%s\",80,0,0\r\n", host_buf);
    rst_val = AT_Cmd_Send(cmd_buf, "+QIOPEN: 0,0", 5000, 1);
    if (rst_val == NULL)
    {
        ESP_LOGE(TAG, " %d", __LINE__);
        goto end;
    }

    sprintf(cmd_buf, "AT+QISEND=0,%d\r\n", (strlen(mqtt_json_s.mqtt_ota_url) + strlen(host_buf) + 13));
    rst_val = AT_Cmd_Send(cmd_buf, ">", 1000, 1);
    if (rst_val == NULL)
    {
        ESP_LOGE(TAG, "%d", __LINE__);
        goto end;
    }

    sprintf(cmd_buf, "GET %s\r\nHost:%s\r\n", mqtt_json_s.mqtt_ota_url, host_buf);
    rst_val = AT_Cmd_Send(cmd_buf, "+QIURC: \"recv\",0", 5000, 1);
    if (rst_val == NULL)
    {
        ESP_LOGE(TAG, "%d", __LINE__);
        goto end;
    }

end:
    free(cmd_buf);
    free(host_buf);
    if (rst_val == NULL)
    {
        return false;
    }
    else
    {
        return true;
    }
}

//读取升级文件 tcp
uint16_t Read_TCP_OTA_File(char *file_buff)
{
    char *cmd_buf = (char *)malloc(120);
    uint8_t *recv_buf = (uint8_t *)malloc(BUF_SIZE);
    // char *rst_val = NULL;
    // char num_buff[5] = {0};
    uint16_t ret = 0;

    xSemaphoreTake(EC20_muxtex, -1);
    EC_RECV_MODE = EC_OTA;
    sprintf(cmd_buf, "AT+QIRD=0,%d\r\n", BUF_SIZE - 25);

    uart_flush(EX_UART_NUM);
    uart_write_bytes(EX_UART_NUM, cmd_buf, strlen(cmd_buf));

    if (xQueueReceive(EC_at_queue, (void *)recv_buf, 10000 / portTICK_RATE_MS) == pdPASS)
    {
        // rst_val = s_strstr((const char *)recv_buf, 7, NULL, "+QIRD:");
        // if (rst_val == NULL)
        // {
        //     ESP_LOGE(TAG, "EC20 %d", __LINE__);
        //     // ESP_LOGI(TAG, "%s", recv_buf);
        //     file_len = 0;
        //     goto end;
        // }
        // // rst_val = mid(rst_val, "+QIRD: ", "\r\n", num_buff);
        // file_len = (uint16_t)strtoul(rst_val, 0, 10);
        // // ESP_LOGI(TAG, "file_len:%d", file_len);
        // rst_val = s_strstr(rst_val, 7, NULL, "\r\n");
        memcpy(file_buff, recv_buf, file_len);
        ret = file_len;
        goto end;
    }
    else //未等到数据
    {
        ESP_LOGE(TAG, "LINE %d", __LINE__);
        ret = 0;
        goto end;
    }

end:
    EC_RECV_MODE = EC_NORMAL;
    xSemaphoreGive(EC20_muxtex);

    free(cmd_buf);
    free(recv_buf);
    return ret;
}

// 检查模块硬件
void Check_Module(void)
{
    char *ret;
    bool module_flag = false;
    bool simcard_flag = false;
    bool result_flag = true;
    float ec_rssi_val;

    if ((xEventGroupGetBits(Net_sta_group) & Uart1_Task_BIT) != Uart1_Task_BIT)
    {
        xTaskCreate(uart_event_task, "uart_event_task", 5120, NULL, 20, &Uart1_Task_Handle);
    }

    cJSON *root = cJSON_CreateObject();
    char *json_temp;

    ret = AT_Cmd_Send("AT\r\n", "OK", 100, 5); //
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "%d", __LINE__);
        EC20_Rest();
    }

    ret = AT_Cmd_Send("AT\r\n", "OK", 100, 5); //
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "%d", __LINE__);
        module_flag = false;
        result_flag = false;
        goto end;
    }
    module_flag = true;

    ret = AT_Cmd_Send("AT+CPIN?\r\n", "READY", 100, 5);
    if (ret == NULL)
    {
        ESP_LOGE(TAG, "EC20_Init %d", __LINE__);
        simcard_flag = false;
        result_flag = false;
        goto end;
    }
    simcard_flag = true;

    EC20_Get_Rssi(&ec_rssi_val);

end:

    if (net_mode != NET_4G)
    {
        EC20_Stop();
    }

    if (result_flag == true)
    {
        cJSON_AddStringToObject(root, "result", "OK");
    }
    else
    {
        cJSON_AddStringToObject(root, "result", "ERROR");
    }

    if (module_flag == true)
    {
        cJSON_AddStringToObject(root, "module", "OK");
    }
    else
    {
        cJSON_AddStringToObject(root, "module", "ERROR");
    }

    if (simcard_flag == true)
    {
        cJSON_AddStringToObject(root, "simcard", "OK");
        cJSON_AddNumberToObject(root, "RSSI", ec_rssi_val);
    }
    else
    {
        cJSON_AddStringToObject(root, "simcard", "ERROR");
    }

    json_temp = cJSON_PrintUnformatted(root);
    printf("%s\n", json_temp);
    cJSON_Delete(root); //delete pJson
    free(json_temp);
}