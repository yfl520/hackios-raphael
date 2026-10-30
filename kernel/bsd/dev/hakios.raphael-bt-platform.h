/* Raphael UART preparation, explicitly enabled by btprep=1, step >=2 only.
 * Register/rate source: downstream gcc-sm8150.c and clk-rcg2.c,
 * LineageOS android_kernel_xiaomi_sm8150 59220a048d04516e7fe1dfc094507930166a1e60.
 * Reuses the SE UART microcode already observed on this phone (protocol 2).
 * No PLL, other SE, GPIO84/85/86 or Wi-Fi reset register is changed.
 */
static bool g_platform_prepare;

static bool
bt_gcc_wait_clear(uint32_t offset, uint32_t mask)
{
    uint64_t until = bt_now_us() + 10000;
    do {
        if (!(bt_rd(gcc_va, offset) & mask)) return true;
        delay(10);
    } while (bt_now_us() < until);
    printf("raphael-bt: platform clock timeout offset=%#x value=%#x\n", offset, bt_rd(gcc_va, offset));
    return false;
}

/* Exact additional rate in the downstream SM8150 QUPV3 table:
 * GPLL0 EVEN 300MHz * 8/25 = 96MHz, 32x oversampling for 3Mbaud.
 * Caller has stopped RX and completed TX. No PLL or peer SE is changed. */
static bool
bt_platform_oper_clock(void)
{
    if (!g_platform_prepare || !hakios_is_raphael() || !gcc_va || !se_va ||
        (bt_se_rd(SE_GENI_STATUS) & (SE_STATUS_M_ACTIVE | SE_STATUS_S_ACTIVE)) ||
        (bt_rd(gcc_va, GCC_WRAP2_S3_DFS) & 1u)) return false;
    uint32_t cmd = bt_rd(gcc_va, GCC_WRAP2_S3_CMD);
    if (cmd & 1u) return false;
    uint32_t cfg = bt_rd(gcc_va, GCC_WRAP2_S3_CFG);
    cfg = (cfg & ~((7u << 8) | (3u << 12) | 0x1fu | (1u << 20))) |
        (6u << 8) | (2u << 12) | 1u;
    uint32_t m = (bt_rd(gcc_va, GCC_WRAP2_S3_RCG_M) & ~0xffffu) | 8u;
    uint32_t n = (bt_rd(gcc_va, GCC_WRAP2_S3_RCG_N) & ~0xffffu) | ((~17u) & 0xffffu);
    uint32_t d = (bt_rd(gcc_va, GCC_WRAP2_S3_CMD + 0x10) & ~0xffffu) | ((~25u) & 0xffffu);
    bt_wr(gcc_va, GCC_WRAP2_S3_RCG_M, m);
    bt_wr(gcc_va, GCC_WRAP2_S3_RCG_N, n);
    bt_wr(gcc_va, GCC_WRAP2_S3_CMD + 0x10, d);
    bt_wr(gcc_va, GCC_WRAP2_S3_CFG, cfg);
    bt_wr(gcc_va, GCC_WRAP2_S3_CMD, cmd | 1u);
    if (!bt_gcc_wait_clear(GCC_WRAP2_S3_CMD, 1u)) return false;
    bool valid = false;
    uint64_t rate = bt_dfs_rate(bt_rd(gcc_va,GCC_WRAP2_S3_CFG),
        bt_rd(gcc_va,GCC_WRAP2_S3_RCG_M),bt_rd(gcc_va,GCC_WRAP2_S3_RCG_N),&valid);
    if (!valid || rate != 96000000ull) return false;
    for (unsigned i = 0; i < GCC_DFS_LEVELS; i++) {
        g_dfs_rate[i] = i == 0 ? rate : 0;
        g_dfs_valid[i] = i == 0;
    }
    printf("raphael-bt: UART operating clock readback 96000000Hz (QUP2 SE3 only)\n");
    return true;
}

static bool
bt_platform_prepare(void)
{
    if (!g_platform_prepare) return true;
    if (!hakios_is_raphael() || !gcc_va || !bt_ahb_on() || !bt_se_ready() || !bt_tlmm_ready()) return false;
    uint32_t fw = bt_se_rd(GENI_FW_REVISION_RO);
    if (((fw & FW_REV_PROTOCOL_MSK) >> FW_REV_PROTOCOL_SHFT) != SE_PROTO_UART ||
        (bt_se_rd(SE_GENI_STATUS) & SE_STATUS_M_ACTIVE)) {
        printf("raphael-bt: platform: not an idle UART; stopped without reprogramming\n");
        return false;
    }
    /* A previous RAM kernel leaves its receive command active. Cancel that
     * bounded UART receive before changing the same SE's clock or dividers.
     * An active transmitter remains a hard refusal above. */
    if (bt_se_rd(SE_GENI_STATUS) & SE_STATUS_S_ACTIVE) {
        printf("raphael-bt: platform: cancel inherited UART RX before reinitialization\n");
        if (!bt_cancel_s()) return false;
    }
    if (bt_rd(gcc_va, GCC_WRAP2_S3_DFS) & 1) {
        printf("raphael-bt: platform: active DFS is not this board's observed state; stopped\n");
        return false;
    }
    uint32_t cmd = bt_rd(gcc_va, GCC_WRAP2_S3_CMD);
    if (cmd & 1) return false; /* no overwrite of a pending update */
    uint32_t cfg = bt_rd(gcc_va, GCC_WRAP2_S3_CFG);
    uint32_t m = bt_rd(gcc_va, GCC_WRAP2_S3_RCG_M);
    uint32_t n = bt_rd(gcc_va, GCC_WRAP2_S3_RCG_N);
    uint32_t d = bt_rd(gcc_va, GCC_WRAP2_S3_CMD + 0x10);
    /* 300MHz GPLL0 EVEN * 384 / 15625 = 7.3728MHz; HID pre-divider=1.
     * CLK_CFG_HW_CLK_CTRL bit20 is cleared as clk_rcg2_configure does. */
    cfg = (cfg & ~((7u << 8) | (3u << 12) | 0x1fu | (1u << 20))) |
        (6u << 8) | (2u << 12) | 1u;
    m = (m & ~0xffffu) | 384u;
    n = (n & ~0xffffu) | ((~(15625u - 384u)) & 0xffffu);
    d = (d & ~0xffffu) | ((~15625u) & 0xffffu);
    printf("raphael-bt: platform: configure UART RCG 7372800Hz; cfg=%#x M=%#x N=%#x D=%#x\n",cfg,m,n,d);
    bt_wr(gcc_va, GCC_WRAP2_S3_RCG_M, m);
    bt_wr(gcc_va, GCC_WRAP2_S3_RCG_N, n);
    bt_wr(gcc_va, GCC_WRAP2_S3_CMD + 0x10, d);
    bt_wr(gcc_va, GCC_WRAP2_S3_CFG, cfg);
    bt_wr(gcc_va, GCC_WRAP2_S3_CMD, cmd | 1u);
    if (!bt_gcc_wait_clear(GCC_WRAP2_S3_CMD, 1)) return false;
    /* Vote only wrapper2 AHB and this SE3; preserve every unrelated vote bit. */
    uint32_t vote = bt_rd(gcc_va, GCC_QUPV3_VOTE);
    bt_wr(gcc_va, GCC_QUPV3_VOTE, vote | GCC_VOTE_S_AHB | GCC_VOTE_M_AHB | GCC_VOTE_SE3);
    if (!bt_gcc_wait_clear(GCC_WRAP2_S3_HALT, GCC_HALT_BIT)) return false;
    bool valid = false;
    uint64_t rate = bt_dfs_rate(bt_rd(gcc_va,GCC_WRAP2_S3_CFG),
        bt_rd(gcc_va,GCC_WRAP2_S3_RCG_M),bt_rd(gcc_va,GCC_WRAP2_S3_RCG_N),&valid);
    if (!valid || rate != 7372800) {
        printf("raphael-bt: platform: clock readback %lluHz does not match\n",(unsigned long long)rate);
        return false;
    }
    for (unsigned i = 0; i < GCC_DFS_LEVELS; i++) {
        g_dfs_rate[i] = i == 0 ? rate : 0;
        g_dfs_valid[i] = i == 0;
    }
    /* Active pinctrl from live DTS: CTS/RX bias-disable, RTS pull-down,
     * TX pull-up; retain drive-strength and unrelated configuration bits. */
    static const uint32_t pull[4] = { 0, 1, 3, 0 };
    for (unsigned i = 0; i < 4; i++) {
        uint32_t old = bt_rd(tlmm_va,TLMM_PIN_STRIDE*i);
        uint32_t next = (old & ~(0x3fu | (1u << TLMM_CFG_OE_SHFT))) | (1u << TLMM_CFG_FUNC_SHFT) | pull[i];
        bt_wr(tlmm_va,TLMM_PIN_STRIDE*i,next);
    }
    printf("raphael-bt: platform: clock branch enabled, rate verified, GPIO43..46 prepared\n");
    return bt_pins();
}
