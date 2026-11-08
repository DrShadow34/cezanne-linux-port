/* stub: MTK sync_write.h, write then barrier */
#include <linux/io.h>
#define mt_reg_sync_writel(v, a) do { __raw_writel((v), (void __force __iomem *)(a)); mb(); } while (0)
#define mt_reg_sync_writew(v, a) do { __raw_writew((v), (void __force __iomem *)(a)); mb(); } while (0)
#define mt_reg_sync_writeb(v, a) do { __raw_writeb((v), (void __force __iomem *)(a)); mb(); } while (0)
