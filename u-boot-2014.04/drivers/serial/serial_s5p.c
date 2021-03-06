/*
 * (C) Copyright 2009 SAMSUNG Electronics
 * Minkyu Kang <mk7.kang@samsung.com>
 * Heungjun Kim <riverful.kim@samsung.com>
 *
 * based on drivers/serial/s3c64xx.c
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <fdtdec.h>
#include <linux/compiler.h>
#include <asm/io.h>
#include <asm/arch/uart.h>
#include <asm/arch/clk.h>
#include <serial.h>

DECLARE_GLOBAL_DATA_PTR;

/* add by Nick. */
/*
 * 移植linux 3.3.5版本时发现一个问题，启动kernel时停在“Starting kernel ...”，后来查资料发现是因为linux 3.3.5 uart发送时如果uart配置为fifo模式，但由于
 * fifo_mask、fifo_max没被赋值，导致死在了一个while循环里面（arch/arm/plat-samsung/include/plat/uncompress.h -> static void putc(int ch)），不知道算不算linux的bug。
 * 为了解决这个问题暂时采用关闭uart fifo的方式。通过宏ENABLE_FIFO来控制是否打开uart fifo。
 */
//#define ENABLE_FIFO

#define RX_FIFO_COUNT_MASK	0xff
#define RX_FIFO_FULL_MASK	(1 << 8)
#define TX_FIFO_FULL_MASK	(1 << 24)

/* Information about a serial port */
struct fdt_serial {
	u32 base_addr;  /* address of registers in physical memory */
	u8 port_id;     /* uart port number */
	u8 enabled;     /* 1 if enabled, 0 if disabled */
} config __attribute__ ((section(".data")));

static inline struct s5p_uart *s5p_get_base_uart(int dev_index)
{
#ifdef CONFIG_OF_CONTROL
	return (struct s5p_uart *)(config.base_addr);
#else
	u32 offset = dev_index * sizeof(struct s5p_uart);
	return (struct s5p_uart *)(samsung_get_base_uart() + offset);
#endif
}

/*
 * The coefficient, used to calculate the baudrate on S5P UARTs is
 * calculated as
 * C = UBRDIV * 16 + number_of_set_bits_in_UDIVSLOT
 * however, section 31.6.11 of the datasheet doesn't recomment using 1 for 1,
 * 3 for 2, ... (2^n - 1) for n, instead, they suggest using these constants:
 */
static const int udivslot[] = {
	0,
	0x0080,
	0x0808,
	0x0888,
	0x2222,
	0x4924,
	0x4a52,
	0x54aa,
	0x5555,
	0xd555,
	0xd5d5,
	0xddd5,
	0xdddd,
	0xdfdd,
	0xdfdf,
	0xffdf,
};

static void serial_setbrg_dev(const int dev_index)
{
	struct s5p_uart *const uart = s5p_get_base_uart(dev_index);
	u32 uclk = get_uart_clk(dev_index);
	u32 baudrate = gd->baudrate;
	u32 val;

#if defined(CONFIG_SILENT_CONSOLE) && \
		defined(CONFIG_OF_CONTROL) && \
		!defined(CONFIG_SPL_BUILD)
	if (fdtdec_get_config_int(gd->fdt_blob, "silent_console", 0))
		gd->flags |= GD_FLG_SILENT;
#endif

	if (!config.enabled)
		return;

	val = uclk / baudrate;

	writel(val / 16 - 1, &uart->ubrdiv);

	if (s5p_uart_divslot())
		writew(udivslot[val % 16], &uart->rest.slot);
	else
		writeb(val % 16, &uart->rest.value);
}

/*
 * Initialise the serial port with the given baudrate. The settings
 * are always 8 data bits, no parity, 1 stop bit, no start bits.
 */
static int serial_init_dev(const int dev_index)
{
	struct s5p_uart *const uart = s5p_get_base_uart(dev_index);
	/* modify by Nick */
#if defined (ENABLE_FIFO)
	/* enable FIFOs, auto clear Rx FIFO */
	writel(0x3, &uart->ufcon);
#else
	//disable fifo
	writel(0x0, &uart->ufcon);
#endif /*ENABLE_FIFO*/
	writel(0, &uart->umcon);
	/* 8N1 */
	writel(0x3, &uart->ulcon);
	/* No interrupts, no DMA, pure polling */
	writel(0x245, &uart->ucon);

	serial_setbrg_dev(dev_index);

	return 0;
}

static int serial_err_check(const int dev_index, int op)
{
	struct s5p_uart *const uart = s5p_get_base_uart(dev_index);
	unsigned int mask;

	/*
	 * UERSTAT
	 * Break Detect	[3]
	 * Frame Err	[2] : receive operation
	 * Parity Err	[1] : receive operation
	 * Overrun Err	[0] : receive operation
	 */
	if (op)
		mask = 0x8;
	else
		mask = 0xf;

	return readl(&uart->uerstat) & mask;
}

/*
 * Read a single byte from the serial port. Returns 1 on success, 0
 * otherwise. When the function is succesfull, the character read is
 * written into its argument c.
 */
static int serial_getc_dev(const int dev_index)
{
	struct s5p_uart *const uart = s5p_get_base_uart(dev_index);

	if (!config.enabled)
		return 0;

	/* wait for character to arrive */
	/*modify by Nick. */
#if defined (ENABLE_FIFO)	
	while (!(readl(&uart->ufstat) & (RX_FIFO_COUNT_MASK |
					 RX_FIFO_FULL_MASK))) {
		if (serial_err_check(dev_index, 0))
			return 0;
	}
#else
	while (!(readl(&uart->utrstat) & 0x1)) {
		if (serial_err_check(dev_index, 0))
			return 0;
	}

#endif /*ENABLE_FIFO*/

	return (int)(readb(&uart->urxh) & 0xff);
}

/*
 * Output a single byte to the serial port.
 */
static void serial_putc_dev(const char c, const int dev_index)
{
	struct s5p_uart *const uart = s5p_get_base_uart(dev_index);

	if (!config.enabled)
		return;

	/* wait for room in the tx FIFO */
	/* modify by Nick. */
#if defined (ENABLE_FIFO)	
	while ((readl(&uart->ufstat) & TX_FIFO_FULL_MASK)) {
		if (serial_err_check(dev_index, 1))
			return;
	}
#else
	while (!(readl(&uart->utrstat) & 0x2)) {
		if (serial_err_check(dev_index, 1))
			return;
	}
#endif /*ENABLE_FIFO*/

	writeb(c, &uart->utxh);

	/* If \n, also do \r */
	if (c == '\n')
		serial_putc('\r');
}

/*
 * Test whether a character is in the RX buffer
 */
static int serial_tstc_dev(const int dev_index)
{
	struct s5p_uart *const uart = s5p_get_base_uart(dev_index);

	if (!config.enabled)
		return 0;

	return (int)(readl(&uart->utrstat) & 0x1);
}

static void serial_puts_dev(const char *s, const int dev_index)
{
	while (*s)
		serial_putc_dev(*s++, dev_index);
}

/* Multi serial device functions */
#define DECLARE_S5P_SERIAL_FUNCTIONS(port) \
static int s5p_serial##port##_init(void) { return serial_init_dev(port); } \
static void s5p_serial##port##_setbrg(void) { serial_setbrg_dev(port); } \
static int s5p_serial##port##_getc(void) { return serial_getc_dev(port); } \
static int s5p_serial##port##_tstc(void) { return serial_tstc_dev(port); } \
static void s5p_serial##port##_putc(const char c) { serial_putc_dev(c, port); } \
static void s5p_serial##port##_puts(const char *s) { serial_puts_dev(s, port); }

#define INIT_S5P_SERIAL_STRUCTURE(port, __name) {	\
	.name	= __name,				\
	.start	= s5p_serial##port##_init,		\
	.stop	= NULL,					\
	.setbrg	= s5p_serial##port##_setbrg,		\
	.getc	= s5p_serial##port##_getc,		\
	.tstc	= s5p_serial##port##_tstc,		\
	.putc	= s5p_serial##port##_putc,		\
	.puts	= s5p_serial##port##_puts,		\
}

DECLARE_S5P_SERIAL_FUNCTIONS(0);
struct serial_device s5p_serial0_device =
	INIT_S5P_SERIAL_STRUCTURE(0, "s5pser0");
DECLARE_S5P_SERIAL_FUNCTIONS(1);
struct serial_device s5p_serial1_device =
	INIT_S5P_SERIAL_STRUCTURE(1, "s5pser1");
DECLARE_S5P_SERIAL_FUNCTIONS(2);
struct serial_device s5p_serial2_device =
	INIT_S5P_SERIAL_STRUCTURE(2, "s5pser2");
DECLARE_S5P_SERIAL_FUNCTIONS(3);
struct serial_device s5p_serial3_device =
	INIT_S5P_SERIAL_STRUCTURE(3, "s5pser3");

#ifdef CONFIG_OF_CONTROL
int fdtdec_decode_console(int *index, struct fdt_serial *uart)
{
	const void *blob = gd->fdt_blob;
	int node;

	node = fdt_path_offset(blob, "console");
	if (node < 0)
		return node;

	uart->base_addr = fdtdec_get_addr(blob, node, "reg");
	if (uart->base_addr == FDT_ADDR_T_NONE)
		return -FDT_ERR_NOTFOUND;

	uart->port_id = fdtdec_get_int(blob, node, "id", -1);
	uart->enabled = fdtdec_get_is_enabled(blob, node);

	return 0;
}
#endif

__weak struct serial_device *default_serial_console(void)
{
#ifdef CONFIG_OF_CONTROL
	int index = 0;

	if ((!config.base_addr) && (fdtdec_decode_console(&index, &config))) {
		debug("Cannot decode default console node\n");
		return NULL;
	}

	switch (config.port_id) {
	case 0:
		return &s5p_serial0_device;
	case 1:
		return &s5p_serial1_device;
	case 2:
		return &s5p_serial2_device;
	case 3:
		return &s5p_serial3_device;
	default:
		debug("Unknown config.port_id: %d", config.port_id);
		break;
	}

	return NULL;
#else
	config.enabled = 1;
#if defined(CONFIG_SERIAL0)
	return &s5p_serial0_device;
#elif defined(CONFIG_SERIAL1)
	return &s5p_serial1_device;
#elif defined(CONFIG_SERIAL2)
	return &s5p_serial2_device;
#elif defined(CONFIG_SERIAL3)
	return &s5p_serial3_device;
#else
#error "CONFIG_SERIAL? missing."
#endif
#endif
}

void s5p_serial_initialize(void)
{
	serial_register(&s5p_serial0_device);
	serial_register(&s5p_serial1_device);
	serial_register(&s5p_serial2_device);
	serial_register(&s5p_serial3_device);
}
