# What the MD asks the AP about the board (RPC), as the stock device tree
# describes the Redmi K30 Ultra (nodes gpio_usage_mapping, md_attr_node,
# md1_sim*_hot_plug_eint of MIUI 14's FDT).

# GPIO name -> (AP pin, MD view id); the MD gets the MD view id
GPIOS = {
    'GPIO_SIM1_HOT_PLUG': (43, 43),
    'GPIO_SIM2_SCLK': (45, 45),
    'GPIO_SIM2_SRST': (46, 46),
    'GPIO_SIM2_SIO': (47, 47),
    'GPIO_SIM1_SIO': (48, 48),
    'GPIO_SIM1_SRST': (49, 49),
    'GPIO_SIM1_SCLK': (50, 50),
    'GPIO_FDD_BAND_SUPPORT_DETECT_1ST_PIN': (63, 63),
    'GPIO_FDD_BAND_SUPPORT_DETECT_2ND_PIN': (64, 64),
    'GPIO_FDD_BAND_SUPPORT_DETECT_3RD_PIN': (65, 65),
    'GPIO_FDD_BAND_SUPPORT_DETECT_4TH_PIN': (66, 66),
}

# names the MD uses for some of them (BSP port_rpc.c gpio_mapping_table)
GPIO_ALIASES = {
    'GPIO_FDD_Band_Support_Detection_%s' % k: 'GPIO_FDD_BAND_SUPPORT_DETECT_%s_PIN' % v
    for k, v in (('1', '1ST'), ('2', '2ND'), ('3', '3RD'), ('4', '4TH'), ('5', '5TH'),
                 ('6', '6TH'), ('7', '7TH'), ('8', '8TH'), ('9', '9TH'), ('A', 'ATH'))
}
GPIO_ALIASES['GPIO_RF_PWREN_RST_PIN'] = 'GPIO_RF_PWREN_RST_PIN'

# SIM hot-plug EINT attributes by query type: number, debounce (ms),
# polarity, sensitivity, socket type, dedicated, source pin. SIM2's node
# exists without properties (every query fails with -12).
NO_ATTR = -12
EINTS = {
    'MD1_SIM1_HOT_PLUG_EINT': [0, 50, 0, 0, 0, 0, 1],
    'MD1_SIM2_HOT_PLUG_EINT': [NO_ATTR] * 7,
}

# md_attr_node
DTSI = {
    'mediatek,md_product_name_model_id': 0x0f0f0f0f,
    'mediatek,md_drdi_rf_set_idx': 0x0c,
}

# Android system properties the MD may ask for (IPC_RPC_QUERY_AP_SYS_PROPERTY)
PROPERTIES = {
    'ro.product.vendor.name': 'cezanne',
    'ro.product.vendor.device': 'cezanne',
    'ro.product.name': 'cezanne',
    'ro.product.device': 'cezanne',
    'ro.product.model': 'M2006J10C',
    'ro.product.brand': 'Redmi',
    'ro.product.manufacturer': 'Xiaomi',
}
