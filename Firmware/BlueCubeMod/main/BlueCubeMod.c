/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "BlueCubeMod.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "btstack.h"
#include "btstack_event.h"
#include "btstack_stdin.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "btstack_run_loop_freertos.h"
#include "driver/gpio.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/rmt.h"
#include "driver/periph_ctrl.h"
#include "soc/rmt_reg.h"

/*
 GameCube controller advertises as a Dualshock 4 "Wireless Controller"
 Mac/PC/PS4 supported (tested using Dolphin Emulator on Mac)
 For Switch/RPi, use an 8Bitdo USB adapter
*/

/*
 Wiring
 Connect the following pins together:
 -RMT_TX_GPIO_NUM (18)
 -RMT_RX_GPIO_NUM (23)
 -GameCube controller's data pin (Red)
 
 Connect GND wires (Black)
*/

#define RMT_TX_GPIO_NUM  18     // TX GPIO ----
#define RMT_RX_GPIO_NUM  23     // RX GPIO ----

#define RMT_TX_CHANNEL    2     /*!< RMT channel for transmitter */
#define RMT_RX_CHANNEL    3     /*!< RMT channel for receiver */
#define RMT_CLK_DIV      80    /*!< RMT counter clock divider */
#define RMT_TICK_10_US    (80000000/RMT_CLK_DIV/100000)   /*!< RMT counter value for 10 us.(Source clock is APB clock) */
#define rmt_item32_tIMEOUT_US  9500   /*!< RMT receiver timeout value(us) */


//HID Descriptor for GameCube Controller matching a DS4
const uint8_t hid_descriptor_gamecube[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    //Padding
    0x95, 0x03,          //     REPORT_COUNT = 3
    0x75, 0x08,          //     REPORT_SIZE = 8
    0x81, 0x03,          //     INPUT = Cnst,Var,Abs
    //Sticks
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x09, 0x32,        //   Usage (Z)
    0x09, 0x35,        //   Usage (Rz)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    //DPAD
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x07,        //   Logical Maximum (7)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x65, 0x14,        //   Unit (System: English Rotation, Length: Centimeter)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
    //Buttons
    0x65, 0x00,        //   Unit (None)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x0E,        //   Usage Maximum (0x0E)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0E,        //   Report Count (14)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    //Padding
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x20,        //   Usage (0x20)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x7F,        //   Logical Maximum (127)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    //Triggers
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x33,        //   Usage (Rx)
    0x09, 0x34,        //   Usage (Ry)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,
    0xc0
};

//Based on https://www.psdevwiki.com/ps4/DS4-BT
//                                 -     -    -     -    Lx    Ly    Cx    Cy    B1   B2  0  LT RT
static uint8_t send_report[] = { 0xa1, 0x11, 0xc0, 0x00, 0x7d, 0x7d, 0x7d, 0x7d, 0x08, 0, 0, 0, 0};

static uint8_t hid_service_buffer[400];
static uint8_t device_id_sdp_service_buffer[400];
static const char hid_device_name[] = "Wireless Controller";
static btstack_packet_callback_registration_t hci_event_callback_registration;
static uint16_t hid_cid = 0;

// Calibration
static int lxcalib = 0;
static int lycalib = 0;
static int cxcalib = 0;
static int cycalib = 0;
static int lcalib = 0;
static int rcalib = 0;
//Buttons and sticks
static uint8_t but1_send = 0;
static uint8_t but2_send = 0;
static uint8_t lx_send = 0;
static uint8_t ly_send = 0;
static uint8_t cx_send = 0;
static uint8_t cy_send = 0;
static uint8_t lt_send = 0;
static uint8_t rt_send = 0;

//RMT Transmitter Init
rmt_item32_t items[25];
rmt_config_t rmt_tx;
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

    uint8_t but1 = 0;
    uint8_t but2 = 0;
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
            
            //Buttons1
            if(item[32].duration0 == 1) but1 += 0x40;//A
            if(item[31].duration0 == 1) but1 += 0x20;//B
            if(item[30].duration0 == 1) but1 += 0x80;//X
            if(item[29].duration0 == 1) but1 += 0x10;//Y
            //DPAD
            if(item[40].duration0 == 1) dpad = 0x06;//L
            if(item[39].duration0 == 1) dpad = 0x02;//R
            if(item[38].duration0 == 1) dpad = 0x04;//D
            if(item[37].duration0 == 1) dpad = 0x00;//U
            
            //Buttons2
            if(item[36].duration0 == 1) but2 += 0x02;//Z
            if(item[35].duration0 == 1) but2 += 0x08;//RB
            if(item[34].duration0 == 1) but2 += 0x04;//LB
            if(item[28].duration0 == 1) but2 += 0x20;//START/OPTIONS/+
            if(but2 == 0x22)  but2 += 0x10;//Select =  Z + Start

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
            }
            
            but1_send = but1 + dpad;
            but2_send = but2;
            lx_send = lx + lxcalib;
            ly_send = ly + lycalib;
            cx_send = cx + cxcalib;
            cy_send = cy + cycalib;
            lt_send = lt;
            rt_send = rt;
            
        }else{
            //log_info("read fail");
        }
        
    }
}


static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t packet_size){
    UNUSED(channel);
    UNUSED(packet_size);
    switch (packet_type){
        case HCI_EVENT_PACKET:
            switch (packet[0]){
                case HCI_EVENT_HID_META:
                    switch (hci_event_hid_meta_get_subevent_code(packet)){
                        case HID_SUBEVENT_CONNECTION_OPENED:
                            if (hid_subevent_connection_opened_get_status(packet)) return;
                            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                            hid_device_request_can_send_now_event(hid_cid); //start loop
                            log_info("HID Connected");
                            break;
                        case HID_SUBEVENT_CONNECTION_CLOSED:
                            log_info("HID Disconnected");
                            hid_cid = 0;
                            break;
                        case HID_SUBEVENT_CAN_SEND_NOW:
                            send_report[4] = lx_send;
                            send_report[5] = (0xff - ly_send);
                            send_report[6] = cx_send;
                            send_report[7] = (0xff - cy_send);
                            send_report[8] = but1_send;
                            send_report[9] = but2_send;
                            send_report[11] = lt_send;
                            send_report[12] = rt_send;
                            hid_device_send_interrupt_message(hid_cid, &send_report[0], sizeof(send_report));
                            hid_device_request_can_send_now_event(hid_cid);
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

/*  Main */
int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    (void)argc;
    (void)argv;
    
    //RMT init
    rmt_tx_init();
    rmt_rx_init();
    
    //format button report from controller
    xTaskCreate(get_buttons, "get_buttons", 2048, NULL, 1, NULL);
    
    // Wait for stick calibration
    vTaskDelay(200);
    
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    hci_register_sco_packet_handler(&packet_handler);
    gap_discoverable_control(1);
    gap_set_class_of_device(0x2508);
    gap_set_local_name("Wireless Controller");

    l2cap_init();
    sdp_init();
    memset(hid_service_buffer, 0, sizeof(hid_service_buffer));
    hid_create_sdp_record(hid_service_buffer, 0x10001, 0x2508, 33, 0, 0, 0, hid_descriptor_gamecube, sizeof(hid_descriptor_gamecube), hid_device_name);
    sdp_register_service(hid_service_buffer);
    device_id_create_sdp_record(device_id_sdp_service_buffer, 0x10003, DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    sdp_register_service(device_id_sdp_service_buffer);
    hid_device_init(1, sizeof(hid_descriptor_gamecube), hid_descriptor_gamecube);
    hid_device_register_packet_handler(&packet_handler);
    
    hci_power_control(HCI_POWER_ON);

    return 0;
}

