# 0001 parses the 3GPP reporting level 3 answers of the MT6885 modem
# (+CEREG: 3,1,"tac","ci",13,0,0), which it gives whatever +CEREG= mode is
# set; 0002 accepts its "+CPMS: "ME", 0, 50, ..." spacing; 0003 adds the
# mtk-ccmni plugin (bearer on the DPMAIF network link: +CGACT, link moved to
# the interface of "+EIF: <id>, ifup", address from +CGPADDR, DNS / MTU from
# +CGCONTRDP; SIM hot swap from the MD's "+ESIMS: <inserted>,<cause>").
cz_patch ModemManager \
	0001-modem-helpers-parse-3GPP-registration-level-3.patch \
	0002-modem-helpers-allow-spaces-in-CPMS-query-reply.patch \
	0003-plugins-new-mtk-ccmni-plugin-for-MediaTek-SoC-modems.patch
