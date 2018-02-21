/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <duke@dukelec.com>
 */

#include "app_main.h"
#include "cdbus_uart.h"
#include "cdctl_bx_it.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include "modbus_crc.h"
#include "main.h"

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;
extern SPI_HandleTypeDef hspi1;
extern TIM_HandleTypeDef htim1;
extern USBD_HandleTypeDef hUsbDeviceFS;

gpio_t led1 = { .group = LED1_GPIO_Port, .num = LED1_Pin };
gpio_t led2 = { .group = LED2_GPIO_Port, .num = LED2_Pin };

uart_t debug_uart = { .huart = &huart3 };
static uart_t ttl_uart = { .huart = &huart1 };
static uart_t rs232_uart; // = { .huart = &huart2 };
uart_t *hw_uart = NULL;

static gpio_t r_rst_n = { .group = CDCTL_RST_N_GPIO_Port, .num = CDCTL_RST_N_Pin };
static gpio_t r_int_n = { .group = CDCTL_INT_N_GPIO_Port, .num = CDCTL_INT_N_Pin };
static gpio_t r_ns = { .group = CDCTL_NS_GPIO_Port, .num = CDCTL_NS_Pin };
static spi_t r_spi = { .hspi = &hspi1, .ns_pin = &r_ns };

#define CDC_RX_MAX 3
#define CDC_TX_MAX 3
static cdc_buf_t cdc_rx_alloc[CDC_RX_MAX];
static cdc_buf_t cdc_tx_alloc[CDC_TX_MAX];
list_head_t cdc_rx_free_head = {0};
list_head_t cdc_tx_free_head = {0};
list_head_t cdc_rx_head = {0};
list_head_t cdc_tx_head = {0};
cdc_buf_t *cdc_rx_buf = NULL;
cdc_buf_t *cdc_tx_buf = NULL;

#define FRAME_MAX 10
static cd_frame_t frame_alloc[FRAME_MAX];
static list_head_t frame_free_head = {0};

#define PACKET_MAX 10
static cdnet_packet_t packet_alloc[PACKET_MAX];
static list_head_t packet_free_head = {0};

cdctl_intf_t r_intf = {0};   // RS485
cdnet_intf_t n_intf = {0};   // CDNET

// for raw mode only
static list_head_t raw2u_head = {0};

// dummy interface for UART, for pass-thru mode only
static cduart_intf_t d_intf = {0};
static cd_frame_t *d_conv_frame = NULL;


static void device_init(void)
{
    int i;
    for (i = 0; i < CDC_RX_MAX; i++)
        list_put(&cdc_rx_free_head, &cdc_rx_alloc[i].node);
    for (i = 0; i < CDC_TX_MAX; i++)
        list_put(&cdc_tx_free_head, &cdc_tx_alloc[i].node);
    for (i = 0; i < FRAME_MAX; i++)
        list_put(&frame_free_head, &frame_alloc[i].node);
    for (i = 0; i < PACKET_MAX; i++)
        list_put(&packet_free_head, &packet_alloc[i].node);

    cdc_rx_buf = list_get_entry(&cdc_rx_free_head, cdc_buf_t);
    d_conv_frame = list_get_entry(&frame_free_head, cd_frame_t);

    cdctl_intf_init(&r_intf, &frame_free_head, app_conf.rs485_addr.mac,
            app_conf.rs485_baudrate_low, app_conf.rs485_baudrate_high,
            &r_spi, &r_rst_n, &r_int_n);
    cduart_intf_init(&d_intf, &frame_free_head);
    d_intf.remote_filter[0] = 0xaa;
    d_intf.remote_filter_len = 1;
    d_intf.local_filter[0] = 0x55;
    d_intf.local_filter[1] = 0x56;
    d_intf.local_filter_len = 2;

    if (app_conf.mode == APP_PASS_THRU) {
        cdnet_addr_t addr = { .net = 0, .mac = 0x55 };
        cdnet_intf_init(&n_intf, &packet_free_head, &d_intf.cd_intf, &addr);
    } else {
        cdnet_intf_init(&n_intf, &packet_free_head, &r_intf.cd_intf, &app_conf.rs485_addr);
    }

    if (app_conf.intf_idx == INTF_TTL) {
        hw_uart = &ttl_uart;
    } else if (app_conf.intf_idx == INTF_RS232) {
        hw_uart = &rs232_uart;
    }
}

void set_led_state(led_state_t state)
{

}

// alloc from n_intf.free_head, send through n_intf.tx_head
static void app_raw_from_u(const uint8_t *buf, int size,
        const uint8_t *wr, const uint8_t *rd)
{
    static cdnet_packet_t *pkt = NULL;
    static uint32_t t_last = 0;
    int max_len;
    int cpy_len;

    if (!app_conf.rpt_en) {
        d_warn("raw <- u: rpt_en disabled\n");
        return;
    }

    if (rd == wr && pkt && pkt->len && get_systick() - t_last > (2000 / SYSTICK_US_DIV)) {
        list_put(&n_intf.tx_head, &pkt->node);
        pkt = NULL;
        return;
    }

    while (true) {
        if (rd == wr)
            return;
        else if (rd > wr)
            max_len = buf + size - rd;
        else // rd < wr
            max_len = wr - rd;

        if (!pkt) {
            pkt = cdnet_packet_get(n_intf.free_head);
            if (!pkt) {
                d_error("raw <- u: no free pkt\n");
                return;
            }
            pkt->level = app_conf.rpt_pkt_level;
            pkt->seq = app_conf.rpt_seq;
            pkt->multi = app_conf.rpt_multi;
            cdnet_fill_src_addr(&n_intf, pkt);
            pkt->dst_mac = app_conf.rpt_mac;
            pkt->dst_addr = app_conf.rpt_addr;
            pkt->src_port = CDNET_DEF_PORT;
            pkt->dst_port = RAW_SER_PORT;
            pkt->len = 0;
        }

        t_last = get_systick();
        cpy_len = min(243 - pkt->len, max_len);

        memcpy(pkt->dat + pkt->len, rd, cpy_len);
        pkt->len += cpy_len;
        rd += cpy_len;
        if (rd == buf + size)
            rd = buf;

        if (pkt->len == 243) {
            list_put(&n_intf.tx_head, &pkt->node);
            pkt = NULL;
        }
    }
}


// alloc from r_intf.free_head, send through r_intf.tx_head or d_rx_head
static void app_pass_thru_from_u(const uint8_t *buf, int size,
        const uint8_t *wr, const uint8_t *rd)
{
    list_node_t *pre, *cur;

    if (rd > wr) {
        cduart_rx_handle(&d_intf, rd, buf + size - rd);
        rd = buf;
    }
    if (rd < wr)
        cduart_rx_handle(&d_intf, rd, wr - rd);

    list_for_each(&d_intf.rx_head, pre, cur) {
        cd_frame_t *fr_src = list_entry(cur, cd_frame_t);
        if (fr_src->dat[1] == 0x56) {
            memcpy(d_conv_frame->dat, fr_src->dat + 3, 2);
            d_conv_frame->dat[2] = fr_src->dat[2] - 2;
            memcpy(d_conv_frame->dat + 3, fr_src->dat + 5, d_conv_frame->dat[2]);

            list_pick(&d_intf.rx_head, pre, cur);
            cdctl_put_tx_frame(&r_intf.cd_intf, d_conv_frame);
            d_conv_frame = fr_src;
            cur = pre;
        }
    }
}


#define CIRC_BUF_SZ 1024
static uint8_t circ_buf[CIRC_BUF_SZ];
static uint32_t rd_pos = 0;

extern uint32_t end; // end of bss


void app_main(void)
{
    debug_init();
    device_init();
    local_irq_enable();
    d_debug("start app_main...\n");
    *(uint32_t *)(&end) = 0xababcdcd;

    if (app_conf.intf_idx != INTF_USB)
        HAL_UART_Receive_DMA(hw_uart->huart, circ_buf, CIRC_BUF_SZ);


    while (true) {
        if (*(uint32_t *)(&end) != 0xababcdcd) {
            printf("stack overflow: %08lx\n", *(uint32_t *)(&end));
            while (true);
        }

        if (app_conf.intf_idx != INTF_USB &&
                hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) {
            d_info("usb connected\n");
            app_conf.intf_idx = INTF_USB;
            HAL_UART_DMAStop(hw_uart->huart);
        }


        // handle cdnet

        cdnet_rx(&n_intf);
        cdnet_packet_t *pkt = cdnet_packet_get(&n_intf.rx_head);
        if (pkt) {
            if (pkt->level == CDNET_L2 || pkt->src_port < CDNET_DEF_PORT ||
                    pkt->dst_port >= CDNET_DEF_PORT) {
                d_warn("unexpected pkg port\n");
                list_put(n_intf.free_head, &pkt->node);
            } else {
                switch (pkt->dst_port) {
                case 1:
                    p1_service(pkt);
                    break;
                case 2:
                    p2_service(pkt);
                    break;
                case 3:
                    p3_service(pkt);
                    break;

                case RAW_SER_PORT:
                    if (app_conf.mode == APP_RAW)
                        list_put(&raw2u_head, &pkt->node);
                    else
                        list_put(n_intf.free_head, &pkt->node);
                    break;

                case RAW_CONF_PORT:
                    if (pkt->len == 4) {
                        app_conf.rpt_en = !!(pkt->dat[0] & 0x80);
                        app_conf.rpt_multi = pkt->dat[0] & 0x30;
                        app_conf.rpt_seq = pkt->dat[0] & 0x08;
                        app_conf.rpt_pkt_level = pkt->dat[0] & 0x03;
                        app_conf.rpt_mac = pkt->dat[1];
                        app_conf.rpt_addr.net = pkt->dat[2];
                        app_conf.rpt_addr.mac = pkt->dat[3];

                        pkt->len = 0;
                        cdnet_exchg_src_dst(&n_intf, pkt);
                        list_put(&n_intf.tx_head, &pkt->node);
                        d_debug("raw_conf: en: %d, mac: %d, lev: %d\n",
                                app_conf.rpt_en, app_conf.rpt_mac,
                                app_conf.rpt_pkt_level);
                    } else {
                        // TODO: report setting
                        list_put(n_intf.free_head, &pkt->node);
                        d_warn("raw_conf: wrong len: %d\n", pkt->len);
                    }
                    break;

                default:
                    d_warn("unknown pkg\n");
                    list_put(n_intf.free_head, &pkt->node);
                }
            }

        }
        cdnet_tx(&n_intf);


        // handle data exchange

        USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassData;
        uint32_t wd_pos = CIRC_BUF_SZ - hw_uart->huart->hdmarx->Instance->CNDTR;

        if (app_conf.intf_idx == INTF_USB) {
            int size;
            uint8_t *wr, *rd;
            cdc_buf_t *bf = list_get_entry_it(&cdc_rx_head, cdc_buf_t);
            if (bf) {
                size = bf->len + 1; // avoid scroll to begin
                wr = bf->dat + bf->len;
                rd = bf->dat;
            }

            if (app_conf.mode == APP_PASS_THRU) {
                if (bf)
                    app_pass_thru_from_u(bf->dat, size, wr, rd);
            } else {
                if (bf)
                    app_raw_from_u(bf->dat, size, wr, rd);
                else
                    app_raw_from_u(NULL, 0, NULL, NULL); // check for timeout
            }
            if (bf) {
                uint32_t flags;
                local_irq_save(flags);
                list_put(&cdc_rx_free_head, &bf->node);
                if (!cdc_rx_buf) {
                    cdc_rx_buf = list_get_entry(&cdc_rx_free_head, cdc_buf_t);
                    d_warn("continue CDC Rx\n");
                    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, cdc_rx_buf->dat);
                    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
                }
                local_irq_restore(flags);
            }
        } else { // hw_uart
            if (app_conf.mode == APP_PASS_THRU)
                app_pass_thru_from_u(circ_buf, CIRC_BUF_SZ, circ_buf + wd_pos, circ_buf + rd_pos);
            else
                app_raw_from_u(circ_buf, CIRC_BUF_SZ, circ_buf + wd_pos, circ_buf + rd_pos);
        }
        rd_pos = wd_pos;

        cdc_buf_t *bf = NULL;
        if (!cdc_tx_head.last) {
            bf = list_get_entry(&cdc_tx_free_head, cdc_buf_t);
            if (!bf) {
                d_warn("no cdc_tx_free, 0\n");
                goto send_list;
            }
            bf->len = 0;
            list_put(&cdc_tx_head, &bf->node);
        } else {
            bf = list_entry(cdc_tx_head.last, cdc_buf_t);
        }

        if (app_conf.mode == APP_PASS_THRU) {
            if (d_intf.tx_head.first) {
                // send to u: d_intf.tx_head
                cd_frame_t *frm = list_entry(d_intf.tx_head.first, cd_frame_t);

                if (bf->len + frm->dat[2] + 5 > 512) {
                    bf = list_get_entry(&cdc_tx_free_head, cdc_buf_t);
                    if (!bf) {
                        d_warn("no cdc_tx_free, 1\n");
                        goto send_list;
                    }
                    bf->len = 0;
                    list_put(&cdc_tx_head, &bf->node);
                }

                d_verbose("pass_thru -> u: 55, dat len %d\n", frm->dat[2]);
                cduart_fill_crc(frm->dat);
                memcpy(bf->dat + bf->len, frm->dat, frm->dat[2] + 5);
                bf->len += frm->dat[2] + 5;

                list_get(&d_intf.tx_head);
                list_put_it(r_intf.free_head, &frm->node);

            } else if (r_intf.rx_head.first) {
                // send to u: r_intf.rx_head (add 56 aa)
                cd_frame_t *frm = list_entry(r_intf.rx_head.first, cd_frame_t);

                if (bf->len + frm->dat[2] + 5 + 2 > 512) {
                    bf = list_get_entry(&cdc_tx_free_head, cdc_buf_t);
                    if (!bf) {
                        d_warn("no cdc_tx_free, 2\n");
                        goto send_list;
                    }
                    bf->len = 0;
                    list_put(&cdc_tx_head, &bf->node);
                }

                uint8_t *buf_dst = bf->dat + bf->len;
                *buf_dst = 0x56;
                *(buf_dst + 1) = 0xaa;
                *(buf_dst + 2) = frm->dat[2] + 2;
                memcpy(buf_dst + 3, frm->dat, 2);
                memcpy(buf_dst + 5, frm->dat + 3, *(buf_dst + 2));
                cduart_fill_crc(buf_dst);
                bf->len += frm->dat[2] + 7;

                list_get_it(&r_intf.rx_head);
                list_put_it(r_intf.free_head, &frm->node);
            }
        } else { // app_raw
            // send to u: raw2u_head
            if (raw2u_head.first) {
                cdnet_packet_t *pkt = list_entry(raw2u_head.first, cdnet_packet_t);

                if (bf->len + pkt->len > 512) {
                    bf = list_get_entry(&cdc_tx_free_head, cdc_buf_t);
                    if (!bf) {
                        d_warn("no cdc_tx_free_head, 3\n");
                        goto send_list;
                    }
                    bf->len = 0;
                    list_put(&cdc_tx_head, &bf->node);
                }

                memcpy(bf->dat + bf->len, pkt->dat, pkt->len);
                bf->len += pkt->len;

                list_get(&raw2u_head);
                list_put(n_intf.free_head, &pkt->node);
            }
        }

send_list:
        if (cdc_tx_buf) {
            if (app_conf.intf_idx == INTF_USB) {
                if (hcdc->TxState == 0) {
                    list_put(&cdc_tx_free_head, &cdc_tx_buf->node);
                    cdc_tx_buf = NULL;
                }
            } else { // hw_uart
                if (hw_uart->huart->TxXferCount == 0) {
                    hw_uart->huart->gState = HAL_UART_STATE_READY;
                    list_put(&cdc_tx_free_head, &cdc_tx_buf->node);
                    cdc_tx_buf = NULL;
                    //d_verbose("hw_uart dma done.\n");
                }
            }
        }
        if (!cdc_tx_buf && cdc_tx_head.first) {
            cdc_buf_t *bf = list_entry(cdc_tx_head.first, cdc_buf_t);
            if (bf->len != 0) {
                if (app_conf.intf_idx == INTF_USB) {
                    CDC_Transmit_FS(bf->dat, bf->len);
                } else { // hw_uart
                    //d_verbose("hw_uart dma tx...\n");
                    HAL_UART_Transmit_DMA(hw_uart->huart, bf->dat, bf->len);
                }
                list_get(&cdc_tx_head);
                cdc_tx_buf = bf;
            }
        }

        debug_flush();
    }
}


void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == r_int_n.num) {
        cdctl_int_isr(&r_intf);
    }
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    cdctl_spi_isr(&r_intf);
}
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    cdctl_spi_isr(&r_intf);
}
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    cdctl_spi_isr(&r_intf);
}
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    d_error("spi error...\n");
}
