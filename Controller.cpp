/****
 * Sming Framework Project - Open Source framework for high efficiency native ESP8266 development.
 * Created 2015 by Skurydin Alexey
 * http://github.com/anakod/Sming
 * All files of the Sming Core are provided under the LGPL v3 license.
 *
 * Controller.cpp
 *
 * @author: 11 December 2018 - mikee47 <mike@sillyhouse.net>
 *
 * Pin connections:
 *
 *				NodeMCU
 *		HSPI			Overlapped			S1D13781 eval.
 * MISO GPIO12 D6		GPIO7 SD0			J2.1
 * MOSI GPIO13 D7		GPIO8 SD1			J2.4
 * CLK  GPIO14 D5		GPIO6 CLK			J2.3
 * CS   GPIO15 D8		GPIO0 D3 (CS2)		J3.5
 *
 * # Interrupt tests
 *
 * Measured 1.6us from CS high to ISR clearing GPIO.
 *
 * # Setup time
 *
 * Tried using inline 32-bit copy operations for FIFO but memcpy was consistently faster.
 * Remove the default 'volatile' from SPI struct, and added only those required for it to work correctly.
 * Reduced the setup time very slightly, but safer to leave as-is.
 *
 */

#include <HSPI/Controller.h>
#include <HSPI/Device.h>
#include <esp_clk.h>
#include <esp_systemapi.h>
#include <espinc/spi_register.h>
#include <espinc/spi_struct.h>
#include <espinc/pin_mux_register.h>
#include <Platform/Timers.h>

namespace HSPI
{
volatile Stats Controller::stats;

// GPIO pin numbers for SPI
#define PIN_HSPI_MISO 12
#define PIN_HSPI_MOSI 13
#define PIN_HSPI_CLK 14
#define PIN_HSPI_CS0 15
#define PIN_SPI_CS1 1
#define PIN_SPI_CS2 0

/* SPI interrupt status register address definition for determining the interrupt source */
#define DPORT_SPI_INT_STATUS_REG 0x3ff00020
#define DPORT_SPI_INT_STATUS_SPI0 BIT4
#define DPORT_SPI_INT_STATUS_SPI1 BIT7

//#define SPI_ENABLE_TEST_PIN

#ifdef SPI_ENABLE_TEST_PIN
// For testing ISR latency, etc.
#define PIN_ISR_TEST 4

#define TESTPIN_HIGH()
#define TESTPIN_LOW()
#define TESTPIN_TOGGLE() GPO ^= BIT(PIN_ISR_TEST)
#else
#define TESTPIN_HIGH()
#define TESTPIN_LOW()
#define TESTPIN_TOGGLE()
#endif

// SPI FIFO
#define SPI_BUFSIZE sizeof(SPI1.data_buf)

/* Controller */

void Controller::begin()
{
	// Pinset and chip selects are device-dependent and not initialised here - see startPacket()

	// Configure interrupts

	//	debug_i("SPI0.slave = 0x%08x", SPI0.slave.val);
	SPI0.slave.val &= ~0x000003FF; // Don't want interrupts from SPI0

	SPI1.slave.val &= ~0x000003FF; // Clear all interrupt sources
	SPI1.slave.trans_inten = 1;	// Interrupt on command completion

	SPI1.slave.slave_mode = false;
	SPI1.slave.sync_reset = true;

	// For testing, we'll be toggling a pin
#ifdef SPI_ENABLE_TEST_PIN
	GPIO_OUTPUT_SET(PIN_ISR_TEST, 0);
#endif

	ETS_SPI_INTR_ATTACH(ets_isr_t(isr), this);
	ETS_SPI_INTR_ENABLE();
}

void IRAM_ATTR Controller::isr(Controller* spi)
{
	if(READ_PERI_REG(DPORT_SPI_INT_STATUS_REG) & DPORT_SPI_INT_STATUS_SPI1) {
		SPI1.slave.trans_done = 0;
		spi->transfer();
	}

	//   	if (READ_PERI_REG(DPORT_SPI_INT_STATUS_REG) & DPORT_SPI_INT_STATUS_SPI0) {
	//   		SPI0.slave.val &= ~0x000003FF;
	//    }
}

void Controller::end()
{
	ETS_SPI_INTR_DISABLE();

	configurePins(PinSet::None);

	// Disable all hardware chip selects, but leave IO MUX settings unchanged to ensure they stay inactive
	SPI1.pin.cs0_dis = 1;
	SPI1.pin.cs1_dis = 1;
	SPI1.pin.cs2_dis = 1;

	if(flags.cs0Configured) {
		PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_HSPI_CS0), FUNC_GPIO15);
		flags.cs0Configured = false;
	}
	if(flags.cs1Configured) {
		PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_SPI_CS1), FUNC_GPIO1);
		flags.cs1Configured = false;
	}

	if(flags.cs2Configured) {
		PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_SPI_CS2), FUNC_GPIO0);
		flags.cs2Configured = false;
	}
}

void Controller::configurePins(PinSet pinSet)
{
	if(pinSet == activePinSet) {
		return;
	}

	debug_w("Configuring PinSet %u", unsigned(pinSet));

	if(activePinSet == PinSet::Overlap) {
		// Disable HSPI overlap
		CLEAR_PERI_REG_MASK(HOST_INF_SEL, PERI_IO_CSPI_OVERLAP);
		// De-prioritise SPI vs HSPI
		SPI0.ext3.int_hold_ena = 0;
		SPI1.ext3.int_hold_ena = 0;
	}

	switch(pinSet) {
	case PinSet::Overlap:
		SET_PERI_REG_MASK(HOST_INF_SEL, PERI_IO_CSPI_OVERLAP);
		// Prioritise SPI over HSPI transactions
		SPI0.ext3.int_hold_ena = 1;
		SPI1.ext3.int_hold_ena = 3;

		if(!flags.overlappedPinsConfigured) {
			// From ESP32 code: 'we do need at least one clock of hold time in most cases'
			spi_dev_t::ctrl2_t ctrl2{};
			//			ctrl2.miso_delay_mode = 3;
			//			ctrl2.miso_delay_num = 4;
			//			ctrl2.mosi_delay_mode = 3;
			//			ctrl2.mosi_delay_num = 4;
			//			ctrl2.cs_delay_mode = 1;
			//			ctrl2.cs_delay_num = 4;
			SPI1.ctrl2.val = ctrl2.val;

			flags.overlappedPinsConfigured = true;
		}
		break;

	case PinSet::Normal:
		if(!flags.hspiPinsConfigured) {
			PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_HSPI_MISO), FUNC_HSPIQ_MISO);
			PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_HSPI_MOSI), FUNC_HSPID_MOSI);
			PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_HSPI_CLK), FUNC_HSPI_CLK);
			flags.hspiPinsConfigured = true;
		}
		break;

	case PinSet::None:
		flags.overlappedPinsConfigured = false;

		// Set any configured pins to GPIO
		if(flags.hspiPinsConfigured) {
			PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_HSPI_MISO), FUNC_GPIO12);
			PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_HSPI_MOSI), FUNC_GPIO13);
			PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_HSPI_CLK), FUNC_GPIO14);
			flags.hspiPinsConfigured = false;
		}
		break;

	default:
		assert(false);
	}

	activePinSet = pinSet;
}

static uint32_t getClockFrequency(const spi_dev_t::clock_t clk)
{
	return APB_CLK_FREQ / ((clk.clkdiv_pre + 1) * (clk.clkcnt_n + 1));
}

uint32_t Controller::clkRegToFreq(uint32_t regVal)
{
	return getClockFrequency({val: regVal});
}

uint32_t Controller::frequencyToClkReg(uint32_t freq)
{
	if(freq >= APB_CLK_FREQ) {
		return SPI_CLK_EQU_SYSCLK;
	}

	spi_dev_t::clock_t minClock{.val = 0x7FFFF000};
	uint32_t minFreq = getClockFrequency(minClock);
	if(freq < minFreq) {
		// use minimum possible clock
		return minClock.val;
	}

	uint8_t calN = 1;

	spi_dev_t::clock_t bestReg{};
	int32_t bestFreq = 0;

	// find the best match
	while(calN <= 0x3F) { // 0x3F max for N

		spi_dev_t::clock_t reg{};
		int32_t calFreq;
		int32_t calPre;
		int8_t calPreVari = -2;

		reg.clkcnt_n = calN;

		// Test different variants for prescale
		while(calPreVari++ <= 1) {
			calPre = (((APB_CLK_FREQ / (reg.clkcnt_n + 1)) / freq) - 1) + calPreVari;
			if(calPre > 0x1FFF) {
				reg.clkdiv_pre = 0x1FFF; // 8191
			} else if(calPre <= 0) {
				reg.clkdiv_pre = 0;
			} else {
				reg.clkdiv_pre = calPre;
			}

			reg.clkcnt_l = ((reg.clkcnt_n + 1) / 2);

			// Test calculation
			calFreq = getClockFrequency(reg);

			if(calFreq == (int32_t)freq) {
				// accurate match use it!
				bestReg = reg;
				break;
			} else if(calFreq < (int32_t)freq) {
				// never go over the requested frequency
				if(abs(int(freq - calFreq)) < abs(int(freq - bestFreq))) {
					bestFreq = calFreq;
					bestReg = reg;
				}
			}
		}
		if(calFreq == (int32_t)freq) {
			// accurate match use it!
			break;
		}
		calN++;
	}

	//	debug_i("[0x%08X][%d]\t EQU: %d\t Pre: %d\t N: %d\t H: %d\t L: %d\t - Real Frequency: %d\n", bestReg.val, freq,
	//			bestReg.clk_equ_sysclk, bestReg.clkdiv_pre, bestReg.clkcnt_n, bestReg.clkcnt_h, bestReg.clkcnt_l,
	//			getClockFrequency(bestReg));

	return bestReg.val;
}

/*
 * OK, so we have two versions of execute() to deal with single FIFO and multiple. For longer
 * transactions we have some options:
 *
 * 	1. As at present, we repeat the operation. It's fine for addressed devices, but may not be for others.
 * 	2a. After the first iteration we set up an ISR to kick off subsequent transactions. We can add a flag
 * 	to the packet to request this behaviour. Completion would be indicated by setting a flag in the packet.
 * 	If execute() is called again it would block until all previous transactions have completed.
 * 	2b. Queue multiple requests. This extends 2a to allow a chain of transactions to be submitted, as a
 * 	linked list of packets. The entire chain would need to complete before processing another.
 *
 *  The problem with (1) is splitting a single request into multiple transactions. We should instead keep
 *  CS asserted, which implies we don't use hardware CS control for that. This isn't an option in overlapped
 *  mode. Interleaving smaller transactions would be better, hence option (2).
 *
 *  Also bear in mind that once a transaction has been submitted to the hardware, it could be delayed by
 *  higher-priority flash accesses on SPI0. It would not cause blocking in the ISR though, as arbitration
 *  happens in hardware.
 *
 *  With the ESP32 we have both regular FIFO operation and the alternative DMA operation. In both cases
 *  a transaction is set up as usual, command, address, etc. with the only difference with the data
 *  transfer. It's not only faster but there's no interrupt overhead and the processor doesn't need
 *  to do any memory copies. The ESP32 can handle a single transfer of up to 4092 bytes.
 *
 *  It is likely we'll have a few well-defined ways to deal with this, and the caller can choose which is
 *  most appropriate for the application and hardware.
 *
 *  UPDATE: Using interrupts to handle transfers works very well, the most benefit being in larger burst
 *  transfers which require splitting out over multiple packets.
 *
 */
void Controller::execute(Packet& packet)
{
	packet.next = nullptr;
	packet.busy = 1;
	// Packet transfer already in progress?
	ETS_SPI_INTR_DISABLE();
	if(trans.busy) {
		// Tack new packet onto end of chain
		auto pkt = trans.packet;
		while(pkt->next) {
			pkt = pkt->next;
		}
		pkt->next = &packet;
	} else {
		// Not currently running, so do this one now
		trans.packet = &packet;
		startPacket();
	}
	ETS_SPI_INTR_ENABLE();

	if(!packet.async) {
		CpuCycleTimer timer;
		while(packet.busy) {
			//
		}
		stats.waitCycles += timer.elapsedTicks();
	}
}

void IRAM_ATTR Controller::startPacket()
{
	TESTPIN_TOGGLE();

	if(trans.packet->device != activeDevice) {
		activeDevice = trans.packet->device;

		configurePins(activeDevice->pinSet);

		// Set register values here and write in one go
		struct {
			spi_dev_t::user_t user;
			spi_dev_t::ctrl_t ctrl;
			spi_dev_t::pin_t pin;
		} reg;
		reg.user.val = 0;
		reg.user.cs_setup = true;
		reg.user.cs_hold = true;
		reg.ctrl.val = 0;
		reg.ctrl.wp_reg = true;
		reg.pin.val = 0;

		// Enable Hardware CS
		switch(activeDevice->chipSelect) {
		case 0:
			if(!flags.cs0Configured) {
				PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_HSPI_CS0), FUNC_HSPI_CS0);
				flags.cs0Configured = true;
			}
			reg.pin.cs1_dis = true;
			reg.pin.cs2_dis = true;
			reg.pin.cs0_dis = false;
			break;
		case 1:
			if(!flags.cs1Configured) {
				PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_SPI_CS1), FUNC_SPICS1);
				flags.cs1Configured = true;
			}
			reg.pin.cs0_dis = true;
			reg.pin.cs2_dis = true;
			reg.pin.cs1_dis = false;
			break;
		case 2:
			if(!flags.cs2Configured) {
				PIN_FUNC_SELECT(PERIPHS_GPIO_MUX_REG(PIN_SPI_CS2), FUNC_SPICS2);
				flags.cs2Configured = true;
			}
			reg.pin.cs0_dis = true;
			reg.pin.cs1_dis = true;
			reg.pin.cs2_dis = false;
			break;
		default:
			// TODO: Call method for manual CS control, NON-OVERLAPPED mode ONLY!
			reg.pin.cs0_dis = true;
			reg.pin.cs1_dis = true;
			reg.pin.cs2_dis = true;
		}

		// Bit order
		trans.bitOrder = activeDevice->getBitOrder();
		reg.ctrl.rd_bit_order = (trans.bitOrder == MSBFIRST) ? 0 : 1;
		reg.ctrl.wr_bit_order = (trans.bitOrder == MSBFIRST) ? 0 : 1;

		// Byte order
		auto byteOrder = LSBFIRST;
		reg.user.wr_byte_order = (byteOrder == MSBFIRST) ? 1 : 0;
		reg.user.rd_byte_order = (byteOrder == MSBFIRST) ? 1 : 0;

		// Data mode
		auto ioMode = activeDevice->getIoMode();
		reg.user.duplex = (ioMode == IoMode::SPI);
		switch(ioMode) {
		case IoMode::SPI:
		case IoMode::SPIHD:
			break;
		case IoMode::SDI:
		case IoMode::DIO:
			reg.ctrl.fastrd_mode = true;
			reg.ctrl.fread_dio = true;
			reg.user.fwrite_dio = true;
			break;
		case IoMode::DUAL:
			reg.ctrl.fastrd_mode = true;
			reg.ctrl.fread_dual = true;
			reg.user.fwrite_dual = true;
			break;
		case IoMode::SQI:
		case IoMode::QIO:
			reg.ctrl.fastrd_mode = true;
			reg.ctrl.fread_qio = true;
			reg.user.fwrite_qio = true;
			break;
		case IoMode::QUAD:
			reg.ctrl.fastrd_mode = true;
			reg.ctrl.fread_quad = true;
			reg.user.fwrite_quad = true;
			break;
		default:
			assert(false);
		}

		// Clock phase/polarity
		auto clockMode = uint8_t(activeDevice->getClockMode());
		reg.user.ck_out_edge = (clockMode & 0x01) ? 1 : 0; // CPHA
		reg.pin.ck_idle_edge = (clockMode & 0x10) ? 1 : 0; // CPOL

		SPI1.ctrl.val = reg.ctrl.val;
		SPI1.ctrl1.val = 0;
		SPI1.pin.val = reg.pin.val;
		SPI1.user.val = reg.user.val;

		// Clock
		auto clockReg = activeDevice->getClockReg();
		auto ioMux = READ_PERI_REG(PERIPHS_IO_MUX_CONF_U);
		if(clockReg == SPI_CLK_EQU_SYSCLK) {
			ioMux |= SPI1_CLK_EQU_SYS_CLK;
		} else {
			ioMux &= ~SPI1_CLK_EQU_SYS_CLK;

			// In overlap mode, SPI0 sysclock selection overrides SPI1
			if(!flags.spi0ClockChanged && activePinSet == PinSet::Overlap) {
				if(ioMux & SPI0_CLK_EQU_SYS_CLK) {
					spi_dev_t::clock_t div2{{
						.clkcnt_l = 1,
						.clkcnt_h = 0,
						.clkcnt_n = 1,
						.clkdiv_pre = 0,
						.clk_equ_sysclk = 0,
					}};
					SPI0.clock.val = div2.val;
					ioMux &= ~SPI0_CLK_EQU_SYS_CLK;
					flags.spi0ClockChanged = true;
				}
			}
		}
		WRITE_PERI_REG(PERIPHS_IO_MUX_CONF_U, ioMux);
		SPI1.clock.val = clockReg;
	}

	trans.addrOffset = 0;
	trans.outOffset = 0;
	trans.inOffset = 0;
	trans.inlen = 0;

	transfer();
}

/** @brief called from ISR
 *  @retval bool true when transaction has finished
 */
void IRAM_ATTR Controller::transfer()
{
	TESTPIN_LOW();
	TESTPIN_HIGH();

	if(trans.packet == nullptr) {
		return;
	}

	Packet& packet = *trans.packet;

	// Read incoming data
	if(trans.inlen != 0) {
		if(packet.in.isPointer) {
			memcpy(packet.in.ptr8 + trans.inOffset, (const void*)SPI1.data_buf, ALIGNUP4(trans.inlen));
		} else {
			packet.in.data32 = SPI1.data_buf[0];
		}
		trans.inOffset += trans.inlen;
		TESTPIN_HIGH();
	}

	// Packet complete?
	unsigned inlen = packet.in.length - trans.inOffset;
	unsigned outlen = packet.out.length - trans.outOffset;
	if(trans.busy && inlen == 0 && outlen == 0) {
		TESTPIN_LOW();
		trans.busy = false;
		packet.busy = false;
		// Note next packet in chain before invoking callback
		trans.packet = packet.next;
		packet.next = nullptr;
		activeDevice->transferComplete(packet);
		// Start the next packet, if there is one
		if(trans.packet != nullptr) {
			startPacket();
		} else {
			// All transfers have completed, set SPI0 clock back to full speed
			if(flags.spi0ClockChanged) {
				SET_PERI_REG_MASK(PERIPHS_IO_MUX_CONF_U, SPI0_CLK_EQU_SYS_CLK);
				flags.spi0ClockChanged = false;
			}
			activeDevice = nullptr;
		}
		return;
	}

	// Set up next transfer
	trans.busy = true;

	// Build register values in a temp is faster than modifying registers directly
	struct {
		spi_dev_t::user_t user;
		spi_dev_t::user1_t user1;
	} reg;
	reg.user.val = SPI1.user.val;
	reg.user1.val = 0;

	// Setup command bits
	if(packet.cmdLen != 0) {
		uint16_t cmd = packet.cmd;
		if(trans.bitOrder == MSBFIRST) {
			// Command sent bit 7->0 then 15->8 so adjust ordering
			cmd = bswap16(cmd << (16 - packet.cmdLen));
		}
		spi_dev_t::user2_t tmp{};
		tmp.usr_command_value = cmd;
		tmp.usr_command_bitlen = packet.cmdLen - 1;
		SPI1.user2.val = tmp.val;
		reg.user.usr_command = 1;
	} else {
		reg.user.usr_command = 0;
	}

	// Setup address bits
	if(packet.addrLen) {
		uint32_t addr = packet.addr + trans.addrOffset;
		if(trans.bitOrder == MSBFIRST) {
			// Address sent MSB to LSB of register value, so shift up as required
			addr <<= 32 - packet.addrLen;
		}

		reg.user1.usr_addr_bitlen = packet.addrLen - 1;
		SPI1.addr = addr;
		reg.user.usr_addr = 1;
	} else {
		reg.user.usr_addr = 0;
	}

	// Setup dummy bits
	if(packet.dummyLen) {
		reg.user1.usr_dummy_cyclelen = packet.dummyLen - 1;
		reg.user.usr_dummy = 1;
	} else {
		reg.user.usr_dummy = 0;
	}

	// Setup outgoing data (MOSI)
	if(outlen) {
		if(packet.out.isPointer) {
			if(outlen > SPI_BUFSIZE) {
				outlen = SPI_BUFSIZE;
			}
			memcpy((void*)SPI1.data_buf, packet.out.ptr8 + trans.outOffset, ALIGNUP4(outlen));
		} else {
			SPI1.data_buf[0] = packet.out.data32;
		}
		reg.user1.usr_mosi_bitlen = (outlen * 8) - 1;
		trans.outOffset += outlen;
		reg.user.usr_mosi = 1;
	} else {
		reg.user.usr_mosi = 0;
	}

	// Setup incoming data (MISO)
	if(inlen) {
		if(inlen > SPI_BUFSIZE) {
			inlen = SPI_BUFSIZE;
		}
		trans.inlen = inlen;
		// In duplex mode data is read during MOSI stage
		if(reg.user.duplex) {
			if(inlen > outlen) {
				reg.user1.usr_mosi_bitlen = (inlen * 8) - 1;
			}
			reg.user.usr_miso = 0;
		} else {
			reg.user1.usr_miso_bitlen = (inlen * 8) - 1;
			reg.user.usr_miso = 1;
		}
	} else {
		reg.user.usr_miso = 0;
	}

	SPI1.user1.val = reg.user1.val;
	SPI1.user.val = reg.user.val;

	// Execute now
	TESTPIN_LOW();
	SPI1.cmd.usr = 1;
	TESTPIN_HIGH();

	/*
	 * This caters for in-only, out-only or (for full duplex modes) in/out transactions.
	 * @todo We should probably identify invalid requests and reject them.
	 */
	trans.addrOffset += std::max(outlen, inlen);

	++stats.transCount;
}

} // namespace HSPI
