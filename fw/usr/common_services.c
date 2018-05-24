/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <duke@dukelec.com>
 */

#include "cdctl_bx_it.h"
#include "app_main.h"

extern cdctl_intf_t r_intf;

static void get_uid(char *buf)
{
    const char tlb[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 12; i++) {
        uint8_t val = *((char *)UID_BASE + i);
        buf[i * 2 + 0] = tlb[val & 0xf];
        buf[i * 2 + 1] = tlb[val >> 4];
    }
    buf[24] = '\0';
}

// make sure local mac != 255 before call any service

// device info
void p1_service(cdnet_packet_t *pkt)
{
    char cpu_id[25];
    char info_str[100];

    // M: model; S: serial string; HW: hardware version; SW: software version
    get_uid(cpu_id);
    sprintf(info_str, "M: cdbus_bridge; S: %s; SW: %s", cpu_id, SW_VER);

    // filter string by input data
    if (pkt->len != 0 && strstr(info_str, (char *)pkt->dat) == NULL) {
        list_put(n_intf.free_head, &pkt->node);
        return;
    }

    strcpy((char *)pkt->dat, info_str);
    pkt->len = strlen(info_str);
    cdnet_exchg_src_dst(&n_intf, pkt);
    list_put(&n_intf.tx_head, &pkt->node);
}

// device baud rate
void p2_service(cdnet_packet_t *pkt)
{
    list_put(n_intf.free_head, &pkt->node);
}

// device addr
void p3_service(cdnet_packet_t *pkt)
{
    if (app_conf.mode == APP_BRIDGE) {
        if (pkt->len < 2 || pkt->dat[0] != 0x08 || pkt->dat[1] != INTF_RS485)
            goto err_free;
        // check mac
        if (pkt->len == 2) {
            pkt->len = 1;
            pkt->dat[0] = r_intf.cd_intf.get_filter(&r_intf.cd_intf);
            goto out_send;
        }
        // set mac
        if (pkt->len == 3) {
            r_intf.cd_intf.set_filter(&r_intf.cd_intf, pkt->dat[2]);
            pkt->len = 0;
            d_debug("set filter: %d...\n", pkt->dat[2]);
            goto out_send;
        }
        goto err_free;
    }
    // else APP_RAW

    // set mac
    if (pkt->len == 2 && pkt->dat[0] == 0x00) {
        r_intf.cd_intf.set_filter(&r_intf.cd_intf, pkt->dat[1]);
        n_intf.addr.mac = pkt->dat[1];
        pkt->len = 0;
        d_debug("set filter: %d...\n", n_intf.addr.mac);
        goto out_send;
    }
    // set net
    if (pkt->len == 2 && pkt->dat[0] == 0x01) {
        n_intf.addr.net = pkt->dat[1];
        pkt->len = 0;
        d_debug("set net: %d...\n", n_intf.addr.net);
        goto out_send;
    }
    // check net id
    if (pkt->len == 1 && pkt->dat[0] == 0x01) {
        pkt->len = 1;
        pkt->dat[0] = n_intf.addr.net;
        goto out_send;
    }
    goto err_free; // TODO: add mac address auto allocation

out_send:
    cdnet_exchg_src_dst(&n_intf, pkt);
    list_put(&n_intf.tx_head, &pkt->node);
    return;

err_free:
    list_put(n_intf.free_head, &pkt->node);
}


// flash memory manipulation
void p10_service(cdnet_packet_t *pkt)
{
    // erase: 0xff, addr_32, len_32  | return [] on success
    // read:  0x00, addr_32, len_8   | return [data]
    // write: 0x01, addr_32 + [data] | return [] on success

    if (pkt->dat[0] == 0xff && pkt->len == 9) {
        uint8_t ret;
        uint32_t err_page = 0;
        FLASH_EraseInitTypeDef f;
        uint32_t addr = *(uint32_t *)(pkt->dat + 1);
        uint32_t len = *(uint32_t *)(pkt->dat + 5);

        f.TypeErase = FLASH_TYPEERASE_PAGES;
        f.PageAddress = addr;
        f.NbPages = (len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

        ret = HAL_FLASH_Unlock();
        if (ret == HAL_OK)
            ret = HAL_FLASHEx_Erase(&f, &err_page);
        ret |= HAL_FLASH_Lock();

        d_debug("nvm erase: %08x +%08x, %08x, ret: %d\n",
                addr, len, err_page, ret);
        if (ret == HAL_OK) {
            pkt->len = 0;
        } else {
            pkt->len = 1;
            pkt->dat[0] = ret;
        }

    } else if (pkt->dat[0] == 0x00 && pkt->len == 6) {
        uint32_t *src_dat = (uint32_t *) *(uint32_t *)(pkt->dat + 1);
        uint8_t len = pkt->dat[5];
        uint8_t cnt = (len + 3) / 4;

        uint32_t *dst_dat = (uint32_t *)pkt->dat;
        uint8_t i;

        cnt = min(cnt, CDNET_DAT_SIZE / 4);

        for (i = 0; i < cnt; i++)
            *(dst_dat + i) = *(src_dat + i);
        d_debug("nvm read: %08x %d(%d)\n", src_dat, len, cnt);
        pkt->len = min(cnt * 4, len);

    } else if (pkt->dat[0] == 0x01 && pkt->len > 5) {
        uint8_t ret;
        uint32_t *dst_dat = (uint32_t *) *(uint32_t *)(pkt->dat + 1);
        uint8_t len = pkt->len - 5;
        uint8_t cnt = (len + 3) / 4;
        uint32_t *src_dat = (uint32_t *)(pkt->dat + 5);
        uint8_t i;

        ret = HAL_FLASH_Unlock();
        for (i = 0; ret == HAL_OK && i < cnt; i++)
            ret = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                    (uint32_t)(dst_dat + i), *(src_dat + i));
        ret |= HAL_FLASH_Lock();

        d_debug("nvm write: %08x %d(%d), ret: %d\n",
                dst_dat, pkt->len - 5, cnt, ret);
        if (ret == HAL_OK) {
            pkt->len = 0;
        } else {
            pkt->len = 1;
            pkt->dat[0] = ret;
        }

    } else {
        list_put(n_intf.free_head, &pkt->node);
        d_warn("nvm: wrong cmd, len: %d\n", pkt->len);
        return;
    }

    cdnet_exchg_src_dst(&n_intf, pkt);
    list_put(&n_intf.tx_head, &pkt->node);
    return;
}
