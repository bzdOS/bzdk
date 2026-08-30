/* phycut.c — cold-boot PHY-lottery repro from inside the FreeBSD guest.
 *
 * Cuts the AXP803 DC1SW output ("vcc-phy", the RTL8211E Ethernet PHY's only
 * supply on this board) for <ms> ms, then restores it. The HV's CPU1-tick
 * link watchdog then owns the recovery — watching it retrain IS the point.
 *
 * Register/bit citation: OUTPUT_CTRL2 (0x12) bit 7 = SW_EN (DC1SW), per
 * U-Boot's include/axp818.h (AXP803 register-compatible). Cross-checked on
 * the board by readback: bit 7 is set while the PHY works.
 *
 * Transport: the guest's own iichb1 (aw_rsb) adapter, serialized against the
 * axp8xx_pmu0 driver by the adapter lock — no EL2/guest RSB race. The
 * transfer shape is aw_rsb.c's contract: exactly two iic_msg — msg0 = 1-byte
 * write of the register offset, msg1 = 1-byte read or write of the data.
 * Slave = 0x3a3 << 1 = 0x746 (aw_rsb.c's rtamap: RSB_ADDR_PMIC_PRIMARY).
 *
 * ONLY bit 7 of register 0x12 is ever written (read-modify-write). No
 * voltage register is touched. Every step is verified by readback; if the
 * restore fails after 5 attempts the program exits nonzero and LOUD.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dev/iicbus/iic.h>

#define IIC_DEV     "/dev/iic1"
#define PMU_SLAVE   0x746u      /* 0x3a3 << 1 */
#define REG_OUTCTL2 0x12u
#define BIT_SW      0x80u

static int xfer(int fd, uint8_t reg, uint8_t *val, int rd)
{
	struct iic_msg msgs[2];
	struct iic_rdwr_data pkt;
	uint8_t r = reg;

	msgs[0].slave = PMU_SLAVE;
	msgs[0].flags = IIC_M_WR;
	msgs[0].len = 1;
	msgs[0].buf = &r;
	msgs[1].slave = PMU_SLAVE;
	msgs[1].flags = rd ? IIC_M_RD : IIC_M_WR;
	msgs[1].len = 1;
	msgs[1].buf = val;
	pkt.msgs = msgs;
	pkt.nmsgs = 2;
	return ioctl(fd, I2CRDWR, &pkt);
}

int main(int argc, char **argv)
{
	int fd, rc;
	uint8_t val = 0;
	long ms = 0;

	fd = open(IIC_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " IIC_DEV);
		return 1;
	}
	rc = xfer(fd, REG_OUTCTL2, &val, 1);
	if (rc < 0) {
		perror("read OUTCTL2");
		return 1;
	}
	printf("OUTCTL2=0x%02x (SW_EN=%d)\n", val, (val & BIT_SW) ? 1 : 0);

	if (argc < 2)
		return 0;
	ms = atol(argv[1]);
	if (ms < 100)
		ms = 100;
	if (ms > 10000)
		ms = 10000;

	val &= ~BIT_SW;
	rc = xfer(fd, REG_OUTCTL2, &val, 0);
	if (rc < 0) {
		perror("write SW_EN=0");
		return 1;
	}
	val = 0;
	if (xfer(fd, REG_OUTCTL2, &val, 1) < 0 || (val & BIT_SW)) {
		printf("FAIL: rail did not read back dark (0x%02x)\n", val);
		return 2;
	}
	fflush(stdout);
	usleep(ms * 1000);

	{
		int tries;
		for (tries = 0; tries < 5; tries++) {
			val |= BIT_SW;
			if (xfer(fd, REG_OUTCTL2, &val, 0) == 0) {
				uint8_t rb = 0;
				if (xfer(fd, REG_OUTCTL2, &rb, 1) == 0 && (rb & BIT_SW)) {
					printf("rail restored after %ld ms (OUTCTL2=0x%02x, tries=%d)\n",
					    ms, rb, tries + 1);
					return 0;
				}
			}
			usleep(50000);
		}
	}
	printf("FATAL: rail restore FAILED after 5 attempts\n");
	return 3;
}
