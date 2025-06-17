/*
  Copyright (c), 2001-2024, Shenshu Tech. Co., Ltd.
 */

#include "sensor_common.h"
#include "sc4336p_cfg.h"

static void sc4336p_default_reg_init(cis_info *cis)
{
    td_u32 i;
    td_s32 ret = TD_SUCCESS;
    ot_isp_sns_state *past_sensor = TD_NULL;

    past_sensor = cis->sns_state;
    for (i = 0; i < past_sensor->regs_info[0].reg_num; i++) {
        ret += cis_write_reg(&cis->i2c,
            past_sensor->regs_info[0].i2c_data[i].reg_addr,
            past_sensor->regs_info[0].i2c_data[i].data);
    }

    if (ret != TD_SUCCESS) {
        isp_err_trace("write register failed!\n");
    }
    return;
}

static td_s32 sc4336p_reg_init(cis_info *cis, cis_reg_cfg *cfg, td_u32 len)
{
    td_u32 i;

    sns_check_return(cis_write_reg(&cis->i2c, 0x0103, 0x01));
    cis_delay_ms(1); /* 1ms */

    for (i = 0; i < len; i++) {
        sns_check_return(cis_write_reg(&cis->i2c, cfg->addr, cfg->data));
        cfg++;
    }

    sc4336p_default_reg_init(cis);

    sns_check_return(cis_write_reg(&cis->i2c, 0x0100, 0x01));

    return TD_SUCCESS;
}

td_s32 sc4336p_linear_4m30_10bit_init(cis_info *cis)
{
    td_s32 ret;
    td_u32 len;
    cis_reg_cfg *cfg = sc4336p_linear_4m30_10bit;

    sns_check_pointer_return(cis);

    len = (td_u32)(sizeof(sc4336p_linear_4m30_10bit) / sizeof(sc4336p_linear_4m30_10bit[0]));
    ret = sc4336p_reg_init(cis, cfg, len);
    if (ret != TD_SUCCESS) {
        isp_err_trace("sc4336p_reg_init failed!\n");
        return ret;
    }

    printf("===================================================================================\n");
    printf("vi_pipe:%d,== SC4336P_MIPI_27Minput_2lane_10bit_630Mbps_2560x1440_30fps Init OK! ==\n", cis->pipe);
    printf("===================================================================================\n");

    return TD_SUCCESS;
}
