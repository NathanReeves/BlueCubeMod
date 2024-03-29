//
//  BlueCubeMod Firmware
//
//
//  Created by Nathan Reeves 2019
//
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_spi_flash.h"

#include "esp_log.h"
#include "esp_hidd_api.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_gap_bt_api.h"
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/rmt.h"
#include "driver/periph_ctrl.h"
#include "soc/rmt_reg.h"



#define LED_GPIO    25
#define PIN_SEL  (1ULL<<LED_GPIO)

//for reading GameCube controller values
#define RMT_TX_GPIO_NUM  23     // GameCube TX GPIO ----
#define RMT_RX_GPIO_NUM  18     // GameCube RX GPIO ----
#define RMT_TX_CHANNEL    2     /*!< RMT channel for transmitter */
#define RMT_RX_CHANNEL    3     /*!< RMT channel for receiver */
#define RMT_CLK_DIV      80    /*!< RMT counter clock divider */
#define RMT_TICK_10_US    (80000000/RMT_CLK_DIV/100000)   /*!< RMT counter value for 10 us.(Source clock is APB clock) */
#define rmt_item32_tIMEOUT_US  9500   /*!< RMT receiver timeout value(us) */

//Calibration
static int lxcalib = 0;
static int lycalib = 0;
static int cxcalib = 0;
static int cycalib = 0;
static int lcalib = 0;
static int rcalib = 0;
//Buttons and sticks
static uint8_t but1_send = 0;
static uint8_t but2_send = 0;
static uint8_t but3_send = 0;
static uint8_t lx_send = 0;
static uint8_t ly_send = 0;
static uint8_t cx_send = 0;
static uint8_t cy_send = 0;
static uint8_t lt_send = 0;
static uint8_t rt_send = 0;


//static esp_hidd_callbacks_t callbacks;
static esp_hidd_app_param_t app_param;
static esp_hidd_qos_param_t both_qos;

//RMT Transmitter Init - for reading GameCube controller
rmt_item32_t items[25];
rmt_config_t rmt_tx;

SemaphoreHandle_t xSemaphore;
bool connected = false;
int paired = 0;
TaskHandle_t SendingHandle = NULL;
TaskHandle_t BlinkHandle = NULL;
uint8_t timer = 0;
static void rmt_tx_init()
{

        rmt_tx.channel = RMT_TX_CHANNEL;
        rmt_tx.gpio_num = RMT_TX_GPIO_NUM;
        rmt_tx.mem_block_num = 1;
        rmt_tx.clk_div = RMT_CLK_DIV;
        rmt_tx.tx_config.loop_en = false;
        rmt_tx.tx_config.carrier_freq_hz = 24000000;
        rmt_tx.tx_config.carrier_level = 1;
        rmt_tx.tx_config.carrier_en = 0;
        rmt_tx.tx_config.idle_level = 1;
        rmt_tx.tx_config.idle_output_en = true;
        rmt_tx.rmt_mode = 0;
        rmt_config(&rmt_tx);
        rmt_driver_install(rmt_tx.channel, 0, 0);

        //Fill items[] with console->controller command: 0100 0000 0000 0011 0000 0010

        items[0].duration0 = 3;
        items[0].level0 = 0;
        items[0].duration1 = 1;
        items[0].level1 = 1;
        items[1].duration0 = 1;
        items[1].level0 = 0;
        items[1].duration1 = 3;
        items[1].level1 = 1;
        int j;
        for(j = 0; j < 12; j++) {
                items[j+2].duration0 = 3;
                items[j+2].level0 = 0;
                items[j+2].duration1 = 1;
                items[j+2].level1 = 1;
        }
        items[14].duration0 = 1;
        items[14].level0 = 0;
        items[14].duration1 = 3;
        items[14].level1 = 1;
        items[15].duration0 = 1;
        items[15].level0 = 0;
        items[15].duration1 = 3;
        items[15].level1 = 1;
        for(j = 0; j < 8; j++) {
                items[j+16].duration0 = 3;
                items[j+16].level0 = 0;
                items[j+16].duration1 = 1;
                items[j+16].level1 = 1;
        }
        items[24].duration0 = 1;
        items[24].level0 = 0;
        items[24].duration1 = 3;
        items[24].level1 = 1;

}

//RMT Receiver Init
rmt_config_t rmt_rx;
static void rmt_rx_init()
{
        rmt_rx.channel = RMT_RX_CHANNEL;
        rmt_rx.gpio_num = RMT_RX_GPIO_NUM;
        rmt_rx.clk_div = RMT_CLK_DIV;
        rmt_rx.mem_block_num = 4;
        rmt_rx.rmt_mode = RMT_MODE_RX;
        rmt_rx.rx_config.idle_threshold = rmt_item32_tIMEOUT_US / 10 * (RMT_TICK_10_US);
        rmt_config(&rmt_rx);
}

//Polls controller and formats response
//GameCube Controller Protocol: http://www.int03.co.uk/crema/hardware/gamecube/gc-control.html
static void get_buttons()
{
        ESP_LOGI("hi", "Hello world from core %d!\n", xPortGetCoreID() );
        //button init values
        uint8_t but1 = 0;
        uint8_t but2 = 0;
        uint8_t but3 = 0;
        uint8_t dpad = 0x08;//Released
        uint8_t lx = 0;
        uint8_t ly = 0;
        uint8_t cx = 0;
        uint8_t cy = 0;
        uint8_t lt = 0;
        uint8_t rt = 0;

        //Sample and find calibration value for sticks
        int calib_loop = 0;
        int xsum = 0;
        int ysum = 0;
        int cxsum = 0;
        int cysum = 0;
        int lsum = 0;
        int rsum = 0;
        while(calib_loop < 5)
        {
                lx = 0;
                ly = 0;
                cx = 0;
                cy = 0;
                lt = 0;
                rt = 0;
                rmt_write_items(rmt_tx.channel, items, 25, 0);
                rmt_rx_start(rmt_rx.channel, 1);

                vTaskDelay(10);

                rmt_item32_t* item = (rmt_item32_t*) (RMT_CHANNEL_MEM(rmt_rx.channel));
                if(item[33].duration0 == 1 && item[27].duration0 == 1 && item[26].duration0 == 3 && item[25].duration0 == 3)
                {

                        //LEFT STICK X
                        for(int x = 8; x > -1; x--)
                        {
                                if((item[x+41].duration0 == 1))
                                {
                                        lx += pow(2, 8-x-1);
                                }
                        }

                        //LEFT STICK Y
                        for(int x = 8; x > -1; x--)
                        {
                                if((item[x+49].duration0 == 1))
                                {
                                        ly += pow(2, 8-x-1);
                                }
                        }
                        //C STICK X
                        for(int x = 8; x > -1; x--)
                        {
                                if((item[x+57].duration0 == 1))
                                {
                                        cx += pow(2, 8-x-1);
                                }
                        }

                        //C STICK Y
                        for(int x = 8; x > -1; x--)
                        {
                                if((item[x+65].duration0 == 1))
                                {
                                        cy += pow(2, 8-x-1);
                                }
                        }

                        //R AN
                        for(int x = 8; x > -1; x--)
                        {
                                if((item[x+73].duration0 == 1))
                                {
                                        rt += pow(2, 8-x-1);
                                }
                        }

                        //L AN
                        for(int x = 8; x > -1; x--)
                        {
                                if((item[x+81].duration0 == 1))
                                {
                                        lt += pow(2, 8-x-1);
                                }
                        }

                        xsum += lx;
                        ysum += ly;
                        cxsum += cx;
                        cysum += cy;
                        lsum += lt;
                        rsum += rt;
                        calib_loop++;
                }

        }

        //Set Stick Calibration
        lxcalib = 127-(xsum/5);
        lycalib = 127-(ysum/5);
        cxcalib = 127-(cxsum/5);
        cycalib = 127-(cysum/5);
        lcalib = 127-(lsum/5);
        rcalib = 127-(rsum/5);


        while(1)
        {
                but1 = 0;
                but2 = 0;
                but3 = 0;
                dpad = 0x08;
                lx = 0;
                ly = 0;
                cx = 0;
                cy = 0;
                lt = 0;
                rt = 0;

                //Write command to controller
                rmt_write_items(rmt_tx.channel, items, 25, 0);
                rmt_rx_start(rmt_rx.channel, 1);

                vTaskDelay(6); //6ms between sample

                rmt_item32_t* item = (rmt_item32_t*) (RMT_CHANNEL_MEM(rmt_rx.channel));

                //Check first 3 bits and high bit at index 33
                if(item[33].duration0 == 1 && item[27].duration0 == 1 && item[26].duration0 == 3 && item[25].duration0 == 3)
                {

                        //Button report: first item is item[25]
                        //0 0 1 S Y X B A
                        //1 L R Z U D R L
                        //Joystick X (8bit)
                        //Joystick Y (8bit)
                        //C-Stick X (8bit)
                        //C-Stick Y (8bit)
                        //L Trigger Analog (8/4bit)
                        //R Trigger Analog (8/4bit)

                        if(item[32].duration0 == 1) but1 += 0x08; // A
                        if(item[31].duration0 == 1) but1 += 0x04; // B
                        if(item[30].duration0 == 1) but1 += 0x02; // X
                        if(item[29].duration0 == 1) but1 += 0x01; // Y

                        if(item[28].duration0 == 1) but2 += 0x02; // START/PLUS

                        //DPAD
                        if(item[40].duration0 == 1) but3 += 0x08; // L
                        if(item[39].duration0 == 1) but3 += 0x04; // R
                        if(item[38].duration0 == 1) but3 += 0x01; // D
                        if(item[37].duration0 == 1) but3 += 0x02; // U

                        if(item[35].duration0 == 1) but1 += 0x80; // ZR
                        if(item[34].duration0 == 1) but3 += 0x80; // ZL
                        //Buttons
                        if(item[36].duration0 == 1)
                        {
                                but1 += 0x40;// Z
                                // if(but3 == 0x80) { but3 += 0x40;}
                                if(but2 == 0x02) { but2 = 0x01; } // Minus =  Z + Start
                                if(but3 == 0x02) { but2 = 0x10; } // Home =  Z + Up
                        }

                        //LEFT STICK X
                        for(int x = 8; x > -1; x--)
                        {
                                if(item[x+41].duration0 == 1)
                                {
                                        lx += pow(2, 8-x-1);
                                }
                        }

                        //LEFT STICK Y
                        for(int x = 8; x > -1; x--)
                        {
                                if(item[x+49].duration0 == 1)
                                {
                                        ly += pow(2, 8-x-1);
                                }
                        }

                        //C STICK X
                        for(int x = 8; x > -1; x--)
                        {
                                if(item[x+57].duration0 == 1)
                                {
                                        cx += pow(2, 8-x-1);
                                }
                        }

                        //C STICK Y
                        for(int x = 8; x > -1; x--)
                        {
                                if(item[x+65].duration0 == 1)
                                {
                                        cy += pow(2, 8-x-1);
                                }
                        }

                        /// Analog triggers here --  Ignore for Switch :/
                        /*
                           //R AN
                           for(int x = 8; x > -1; x--)
                           {
                            if(item[x+73].duration0 == 1)
                            {
                                rt += pow(2, 8-x-1);
                            }
                           }

                           //L AN
                           for(int x = 8; x > -1; x--)
                           {
                            if(item[x+81].duration0 == 1)
                            {
                                lt += pow(2, 8-x-1);
                            }
                           }*/
                        /////
                        but1_send = but1;
                        but2_send = but2;
                        but3_send = but3;
                        lx_send = lx + lxcalib;
                        ly_send = ly + lycalib;
                        cx_send = cx + cxcalib;
                        cy_send = cy + cycalib;
                        lt_send = 0;//lt;//left trigger analog
                        rt_send = 0;//rt;//right trigger analog
                }else{
                        //log_info("GameCube controller read fail");
                }

        }
}

//Switch button report example //         batlvl       Buttons              Lstick           Rstick
//static uint8_t report30[] = {0x30, 0x00, 0x90,   0x00, 0x00, 0x00,   0x00, 0x00, 0x00,   0x00, 0x00, 0x00};
static uint8_t report30[] = {
        0x30,
        0x0,
        0x80,
        0,//but1
        0,//but2
        0,//but3
        0,//Ls
        0,//Ls
        0,//Ls
        0,//Rs
        0,//Rs
        0,//Rs
        0x08
};
static uint8_t emptyReport[] = {
        0x0,
        0x0
};

void send_buttons()
{
        // // test
        // if(but1_send == 0)
        //         but1_send = 0x02; // X
        // else
        //         but1_send = 0;

        xSemaphoreTake(xSemaphore, portMAX_DELAY);
        report30[1] = timer;
        //buttons
        report30[3] = but1_send;
        report30[4] = but2_send;
        report30[5] = but3_send;
        //encode left stick
        report30[6] = (lx_send << 4) & 0xF0;
        report30[7] = (lx_send & 0xF0) >> 4;
        report30[8] = ly_send;
        //encode right stick
        report30[9] = (cx_send << 4) & 0xF0;
        report30[10] = (cx_send & 0xF0) >> 4;
        report30[11] = cy_send;
        xSemaphoreGive(xSemaphore);
        timer+=1;
        if(timer == 255)
                timer = 0;

        if(!paired)
        {
                emptyReport[1] = timer;
                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(emptyReport), emptyReport);
                vTaskDelay(100);
        }
        else
        {
                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(report30), report30);
                vTaskDelay(15);
        }


}
const uint8_t hid_descriptor_gamecube[] = {
        0x05, 0x01,    // Usage Page (Generic Desktop Ctrls)
        0x09, 0x05,    // Usage (Game Pad)
        0xA1, 0x01,    // Collection (Application)
        //Padding
        0x95, 0x03,      //     REPORT_COUNT = 3
        0x75, 0x08,      //     REPORT_SIZE = 8
        0x81, 0x03,      //     INPUT = Cnst,Var,Abs
        //Sticks
        0x09, 0x30,    //   Usage (X)
        0x09, 0x31,    //   Usage (Y)
        0x09, 0x32,    //   Usage (Z)
        0x09, 0x35,    //   Usage (Rz)
        0x15, 0x00,    //   Logical Minimum (0)
        0x26, 0xFF, 0x00, //   Logical Maximum (255)
        0x75, 0x08,    //   Report Size (8)
        0x95, 0x04,    //   Report Count (4)
        0x81, 0x02,    //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
        //DPAD
        0x09, 0x39,    //   Usage (Hat switch)
        0x15, 0x00,    //   Logical Minimum (0)
        0x25, 0x07,    //   Logical Maximum (7)
        0x35, 0x00,    //   Physical Minimum (0)
        0x46, 0x3B, 0x01, //   Physical Maximum (315)
        0x65, 0x14,    //   Unit (System: English Rotation, Length: Centimeter)
        0x75, 0x04,    //   Report Size (4)
        0x95, 0x01,    //   Report Count (1)
        0x81, 0x42,    //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
        //Buttons
        0x65, 0x00,    //   Unit (None)
        0x05, 0x09,    //   Usage Page (Button)
        0x19, 0x01,    //   Usage Minimum (0x01)
        0x29, 0x0E,    //   Usage Maximum (0x0E)
        0x15, 0x00,    //   Logical Minimum (0)
        0x25, 0x01,    //   Logical Maximum (1)
        0x75, 0x01,    //   Report Size (1)
        0x95, 0x0E,    //   Report Count (14)
        0x81, 0x02,    //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
        //Padding
        0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
        0x09, 0x20,    //   Usage (0x20)
        0x75, 0x06,    //   Report Size (6)
        0x95, 0x01,    //   Report Count (1)
        0x15, 0x00,    //   Logical Minimum (0)
        0x25, 0x7F,    //   Logical Maximum (127)
        0x81, 0x02,    //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
        //Triggers
        0x05, 0x01,    //   Usage Page (Generic Desktop Ctrls)
        0x09, 0x33,    //   Usage (Rx)
        0x09, 0x34,    //   Usage (Ry)
        0x15, 0x00,    //   Logical Minimum (0)
        0x26, 0xFF, 0x00, //   Logical Maximum (255)
        0x75, 0x08,    //   Report Size (8)
        0x95, 0x02,    //   Report Count (2)
        0x81, 0x02,
        0xc0
};
int hid_descriptor_gc_len = sizeof(hid_descriptor_gamecube);
///Switch Replies
static uint8_t reply02[] = {0x21, 0x01, 0x40, 0x00, 0x00, 0x00, 0xe6, 0x27, 0x78, 0xab, 0xd7, 0x76, 0x00, 0x82, 0x02, 0x03, 0x48, 0x03, 0x02, 0xD8, 0xA0, 0x1D, 0x40, 0x15, 0x66, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                            , 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static uint8_t reply08[] = {0x21, 0x02, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x80, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                            , 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static uint8_t reply03[] = {0x21, 0x05, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                            , 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static uint8_t reply04[] = {0x21, 0x06, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x83, 0x04, 0x00, 0x6a, 0x01, 0xbb, 0x01, 0x93, 0x01, 0x95, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                            , 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t reply1060[] = {0x21, 0x03, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x90, 0x10, 0x00, 0x60, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                              , 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static uint8_t reply1050[] = { 0x21, 0x04, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x90, 0x10, 0x50, 0x60, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                               , 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static uint8_t reply1080[] = {0x21, 0x04, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x90, 0x10, 0x80, 0x60, 0x00, 0x00, 0x18, 0x5e, 0x01, 0x00, 0x00, 0xf1, 0x0f,
                              0x19, 0xd0, 0x4c, 0xae, 0x40, 0xe1,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                              0x00, 0x00};
static uint8_t reply1098[] = {0x21, 0x04, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x90, 0x10, 0x98, 0x60, 0x00, 0x00, 0x12, 0x19, 0xd0, 0x4c, 0xae, 0x40, 0xe1,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                              0x00, 0x00};
//User analog stick calib
static uint8_t reply1010[] = {0x21, 0x04, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x90, 0x10, 0x10, 0x80, 0x00, 0x00, 0x18, 0x00, 0x00};
static uint8_t reply103D[] = {0x21, 0x05, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x90, 0x10, 0x3D, 0x60, 0x00, 0x00, 0x19, 0xF0, 0x07, 0x7f, 0xF0, 0x07, 0x7f, 0xF0, 0x07, 0x7f, 0xF0, 0x07, 0x7f, 0xF0, 0x07, 0x7f, 0xF0, 0x07, 0x7f, 0xF0, 0x07, 0x7f, 0xF0, 0x07, 0x7f, 0x0f, 0x0f, 0x00, 0x00, 0x00, 0x00};
static uint8_t reply1020[] = {0x21, 0x04, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x90, 0x10, 0x20, 0x60, 0x00, 0x00, 0x18, 0x00, 0x00};
static uint8_t reply4001[] = {0x21, 0x04, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x80, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t reply4801[] = {0x21, 0x04, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x80, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t reply3001[] = {0x21, 0x04, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x80, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t reply3333[] = {0x21, 0x03, 0x8E, 0x84, 0x00, 0x12, 0x01, 0x18, 0x80, 0x01, 0x18, 0x80, 0x80, 0x80, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                              , 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };



// sending bluetooth values every 15ms
void send_task(void* pvParameters) {
        const char* TAG = "send_task";
        ESP_LOGI(TAG, "Sending hid reports on core %d\n", xPortGetCoreID() );
        while(1)
        {
                send_buttons();
        }
}

//LED blink
void startBlink()
{
        while(1) {
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(150);
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(150);
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(150);
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(1000);
        }
        vTaskDelete(NULL);
}

void set_bt_address()
{
        //store a random mac address in flash
        nvs_handle my_handle;
        esp_err_t err;
        uint8_t bt_addr[8];

        err = nvs_open("storage", NVS_READWRITE, &my_handle);
        if (err != ESP_OK) return err;

        size_t addr_size = 0;
        err = nvs_get_blob(my_handle, "mac_addr", NULL, &addr_size);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;

        if (addr_size > 0) {
                err = nvs_get_blob(my_handle, "mac_addr", bt_addr, &addr_size);
        }
        else
        {
                for(int i=0; i<8; i++)
                        bt_addr[i] = esp_random()%255;
                size_t addr_size = sizeof(bt_addr);
                err = nvs_set_blob(my_handle, "mac_addr", bt_addr, addr_size);
        }

        err = nvs_commit(my_handle);
        nvs_close(my_handle);
        esp_base_mac_addr_set(bt_addr);

        //put mac addr in switch pairing packet
        for(int z=0; z<6; z++)
                reply02[z+19] = bt_addr[z];
}
void print_bt_address() {
        const char* TAG = "bt_address";
        const uint8_t* bd_addr;

        bd_addr = esp_bt_dev_get_address();
        ESP_LOGI(TAG, "my bluetooth address is %02X:%02X:%02X:%02X:%02X:%02X",
                 bd_addr[0], bd_addr[1], bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5]);
}

#define SPP_TAG "tag"
static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
        switch(event) {
        case ESP_BT_GAP_DISC_RES_EVT:
                ESP_LOGI(SPP_TAG, "ESP_BT_GAP_DISC_RES_EVT");
                esp_log_buffer_hex(SPP_TAG, param->disc_res.bda, ESP_BD_ADDR_LEN);
                break;
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
                ESP_LOGI(SPP_TAG, "ESP_BT_GAP_DISC_STATE_CHANGED_EVT");
                break;
        case ESP_BT_GAP_RMT_SRVCS_EVT:
                ESP_LOGI(SPP_TAG, "ESP_BT_GAP_RMT_SRVCS_EVT");
                ESP_LOGI(SPP_TAG, "%d", param->rmt_srvcs.num_uuids);
                break;
        case ESP_BT_GAP_RMT_SRVC_REC_EVT:
                ESP_LOGI(SPP_TAG, "ESP_BT_GAP_RMT_SRVC_REC_EVT");
                break;
        case ESP_BT_GAP_AUTH_CMPL_EVT: {
                if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                        ESP_LOGI(SPP_TAG, "authentication success: %s", param->auth_cmpl.device_name);
                        esp_log_buffer_hex(SPP_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
                } else {
                        ESP_LOGE(SPP_TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
                }
                break;
        }

        default:
                break;
        }
}







void esp_bt_hidd_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
        static const char* TAG = "esp_bt_hidd_cb";
        switch (event) {
        case ESP_HIDD_INIT_EVT:
                if (param->init.status == ESP_HIDD_SUCCESS) {
                        ESP_LOGI(TAG, "setting hid parameters");
                        esp_bt_hid_device_register_app(&app_param, &both_qos, &both_qos);
                } else {
                        ESP_LOGE(TAG, "init hidd failed!");
                }
                break;
        case ESP_HIDD_DEINIT_EVT:
                break;
        case ESP_HIDD_REGISTER_APP_EVT:
                if (param->register_app.status == ESP_HIDD_SUCCESS) {
                        ESP_LOGI(TAG, "setting hid parameters success!");
                        ESP_LOGI(TAG, "setting to connectable, discoverable");
                        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                        if (param->register_app.in_use && param->register_app.bd_addr != NULL) {
                                ESP_LOGI(TAG, "start virtual cable plug!");
                                esp_bt_hid_device_connect(param->register_app.bd_addr);
                        }
                } else {
                        ESP_LOGE(TAG, "setting hid parameters failed!");
                }
                break;
        case ESP_HIDD_UNREGISTER_APP_EVT:
                if (param->unregister_app.status == ESP_HIDD_SUCCESS) {
                        ESP_LOGI(TAG, "unregister app success!");
                } else {
                        ESP_LOGE(TAG, "unregister app failed!");
                }
                break;
        case ESP_HIDD_OPEN_EVT:
                if (param->open.status == ESP_HIDD_SUCCESS) {
                        if (param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTING) {
                                ESP_LOGI(TAG, "connecting...");
                        } else if (param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED) {
                                ESP_LOGI(TAG, "connected to %02x:%02x:%02x:%02x:%02x:%02x", param->open.bd_addr[0],
                                         param->open.bd_addr[1], param->open.bd_addr[2], param->open.bd_addr[3], param->open.bd_addr[4],
                                         param->open.bd_addr[5]);

                                ESP_LOGI(TAG, "making self non-discoverable and non-connectable.");
                                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);


                                //clear blinking LED - solid
                                vTaskDelete(BlinkHandle);
                                BlinkHandle = NULL;
                                gpio_set_level(LED_GPIO, 1);
                                //start solid
                                xSemaphoreTake(xSemaphore, portMAX_DELAY);
                                connected = true;
                                xSemaphoreGive(xSemaphore);
                                //restart send_task
                                if(SendingHandle != NULL)
                                {
                                        vTaskDelete(SendingHandle);
                                        SendingHandle = NULL;
                                }
                                xTaskCreatePinnedToCore(send_task, "send_task", 2048, NULL, 2, &SendingHandle, 0);
                        } else {
                                ESP_LOGE(TAG, "unknown connection status");
                        }
                } else {
                        ESP_LOGE(TAG, "open failed!");
                }
                break;
        case ESP_HIDD_CLOSE_EVT:
                ESP_LOGI(TAG, "ESP_HIDD_CLOSE_EVT");
                if (param->close.status == ESP_HIDD_SUCCESS) {
                        if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTING) {
                                ESP_LOGI(TAG, "disconnecting...");
                        } else if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED) {
                                ESP_LOGI(TAG, "disconnected!");

                                ESP_LOGI(TAG, "making self discoverable and connectable again.");
                                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

                                xTaskCreate(startBlink, "blink_task", 1024, NULL, 1, &BlinkHandle);
                                //start blink
                                xSemaphoreTake(xSemaphore, portMAX_DELAY);
                                connected = false;
                                xSemaphoreGive(xSemaphore);
                        } else {
                                ESP_LOGE(TAG, "unknown connection status");
                        }
                } else {
                        ESP_LOGE(TAG, "close failed!");
                }
                break;
        case ESP_HIDD_SEND_REPORT_EVT:
                ESP_LOGI(TAG, "ESP_HIDD_SEND_REPORT_EVT id:0x%02x, type:%d", param->send_report.report_id,
                         param->send_report.report_type);
                break;
        case ESP_HIDD_REPORT_ERR_EVT:
                ESP_LOGI(TAG, "ESP_HIDD_REPORT_ERR_EVT");
                break;
        case ESP_HIDD_GET_REPORT_EVT:
                ESP_LOGI(TAG, "ESP_HIDD_GET_REPORT_EVT id:0x%02x, type:%d, size:%d", param->get_report.report_id,
                         param->get_report.report_type, param->get_report.buffer_size);
                break;
        case ESP_HIDD_SET_REPORT_EVT:
                ESP_LOGI(TAG, "ESP_HIDD_SET_REPORT_EVT");
                break;
        case ESP_HIDD_SET_PROTOCOL_EVT:
                ESP_LOGI(TAG, "ESP_HIDD_SET_PROTOCOL_EVT");
                if (param->set_protocol.protocol_mode == ESP_HIDD_BOOT_MODE) {
                        ESP_LOGI(TAG, "  - boot protocol");
                } else if (param->set_protocol.protocol_mode == ESP_HIDD_REPORT_MODE) {
                        ESP_LOGI(TAG, "  - report protocol");
                }
                break;
        case ESP_HIDD_INTR_DATA_EVT:
                ESP_LOGI(TAG, "ESP_HIDD_INTR_DATA_EVT");




                //switch pairing sequence
                if(param->intr_data.len == 49)
                {
                        if(param->intr_data.data[10] == 2)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply02), reply02);
                        }
                        if(param->intr_data.data[10] == 8)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply08), reply08);
                        }
                        if(param->intr_data.data[10] == 16 && param->intr_data.data[11] == 0 && param->intr_data.data[12] == 96)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply1060), reply1060);
                        }
                        if(param->intr_data.data[10] == 16 && param->intr_data.data[11] == 80 && param->intr_data.data[12] == 96)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply1050), reply1050);
                        }
                        if(param->intr_data.data[10] == 3)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply03), reply03);
                        }
                        if(param->intr_data.data[10] == 4)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply04), reply04);
                        }
                        if(param->intr_data.data[10] == 16 && param->intr_data.data[11] == 128 && param->intr_data.data[12] == 96)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply1080), reply1080);
                        }
                        if(param->intr_data.data[10] == 16 && param->intr_data.data[11] == 152 && param->intr_data.data[12] == 96)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply1098), reply1098);
                        }
                        if(param->intr_data.data[10] == 16 && param->intr_data.data[11] == 16 && param->intr_data.data[12] == 128)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply1010), reply1010);
                        }
                        if(param->intr_data.data[10] == 16 && param->intr_data.data[11] == 61 && param->intr_data.data[12] == 96)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply103D), reply103D);
                        }
                        if(param->intr_data.data[10] == 16 && param->intr_data.data[11] == 32 && param->intr_data.data[12] == 96)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply1020), reply1020);
                        }
                        if(param->intr_data.data[10] == 64 && param->intr_data.data[11] == 1)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply4001), reply4001);
                        }
                        if(param->intr_data.data[10] == 72 && param->intr_data.data[11] == 1)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply4801), reply4801);
                        }
                        if(param->intr_data.data[10] == 48 && param->intr_data.data[11] == 1)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply3001), reply3001);
                        }

                        if(param->intr_data.data[10] == 33 && param->intr_data.data[11] == 33)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply3333), reply3333);
                                paired = 1;

                        }
                        if(param->intr_data.data[10] == 64 && param->intr_data.data[11] == 2)
                        {
                                esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0xa1, sizeof(reply4001), reply4001);
                        }
                        ESP_LOGI(TAG, "got an interrupt report from host, subcommand: %d  %d  %d Length: %d", param->intr_data.data[10], param->intr_data.data[11], param->intr_data.data[12], param->intr_data.len);
                }
                else
                {
                        ESP_LOGI("heap size:", "%d", xPortGetFreeHeapSize());
                        ESP_LOGI(TAG, "pairing packet size != 49, subcommand: %d  %d  %d  Length: %d", param->intr_data.data[10], param->intr_data.data[11], param->intr_data.data[12], param->intr_data.len);
                }
                break;
        case ESP_HIDD_VC_UNPLUG_EVT:
                ESP_LOGI(TAG, "ESP_HIDD_VC_UNPLUG_EVT");
                if (param->vc_unplug.status == ESP_HIDD_SUCCESS) {
                        if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED) {
                                ESP_LOGI(TAG, "disconnected!");

                                ESP_LOGI(TAG, "making self discoverable and connectable again.");
                                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                        } else {
                                ESP_LOGE(TAG, "unknown connection status");
                        }
                } else {
                        ESP_LOGE(TAG, "close failed!");
                }
                break;
        default:
                break;
        }
}



void app_main(void);
void app_main(void)
{
        //GameCube Contoller reading init
        rmt_tx_init();
        rmt_rx_init();
        xTaskCreatePinnedToCore(get_buttons, "gbuttons", 2048, NULL, 1, NULL, 1);
        //flash LED
        vTaskDelay(100);
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(100);
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(100);
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(100);
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(100);
        gpio_set_level(LED_GPIO, 0);
        const char* TAG = "app_main";
        esp_err_t ret;

        xSemaphore = xSemaphoreCreateMutex();

        gpio_config_t io_conf;
        //disable interrupt
        io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
        //set as output mode
        io_conf.mode = GPIO_MODE_OUTPUT;
        //bit mask of the pins that you want to set,e.g.GPIO18/19
        io_conf.pin_bit_mask = PIN_SEL;
        //disable pull-down mode
        io_conf.pull_down_en = 0;
        //disable pull-up mode
        io_conf.pull_up_en = 0;
        //configure GPIO with the given settings
        gpio_config(&io_conf);

        app_param.name = "BlueCubeMod";
        app_param.description = "BlueCubeMod Example";
        app_param.provider = "ESP32";
        app_param.subclass = 0x8;
        app_param.desc_list = hid_descriptor_gamecube;
        app_param.desc_list_len = hid_descriptor_gc_len;
        memset(&both_qos, 0, sizeof(esp_hidd_qos_param_t));

        ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                ESP_ERROR_CHECK(nvs_flash_erase());
                ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK( ret );

        set_bt_address();

        ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_bt_mem_release(ESP_BT_MODE_BLE);
        if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
                ESP_LOGE(TAG, "initialize controller failed: %s\n",  esp_err_to_name(ret));
                return;
        }

        if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
                ESP_LOGE(TAG, "enable controller failed: %s\n",  esp_err_to_name(ret));
                return;
        }

        if ((ret = esp_bluedroid_init()) != ESP_OK) {
                ESP_LOGE(TAG, "initialize bluedroid failed: %s\n",  esp_err_to_name(ret));
                return;
        }

        if ((ret = esp_bluedroid_enable()) != ESP_OK) {
                ESP_LOGE(TAG, "enable bluedroid failed: %s\n",  esp_err_to_name(ret));
                return;
        }

        if ((ret = esp_bt_gap_register_callback(esp_bt_gap_cb)) != ESP_OK) {
                ESP_LOGE(TAG, "gap register failed: %s\n", esp_err_to_name(ret));
                return;
        }

        ESP_LOGI(TAG, "setting device name");
        esp_bt_dev_set_device_name("Pro Controller");

        ESP_LOGI(TAG, "setting cod major, Pro Controller");
        esp_bt_cod_t cod;
        cod.major = 0x05;
        cod.minor = 0b0000010;
        esp_bt_gap_set_cod(cod,ESP_BT_SET_COD_MAJOR_MINOR     );

        vTaskDelay(2000 / portTICK_PERIOD_MS);

        ESP_LOGI(TAG, "register hid device callback");
        esp_bt_hid_device_register_callback(esp_bt_hidd_cb);

        ESP_LOGI(TAG, "starting hid device");
        esp_bt_hid_device_init();

        //start blinking
        xTaskCreate(startBlink, "blink_task", 1024, NULL, 1, &BlinkHandle);

        print_bt_address();
}
