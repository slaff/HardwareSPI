/****
 * Controller.h
 *
 * Copyright 2018 mikee47 <mike@sillyhouse.net>
 *
 * This file is part of the HardwareSPI Library
 *
 * This library is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, version 3 or later.
 *
 * 
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this library.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * @author: 11 December 2018 - mikee47 <mike@sillyhouse.net>
 *
 * # SPI master-mode hardware controller
 *
 * The ESP8266 SPI hardware is capable of very high transfer speeds and has a number of
 * features which require a more flexible driver to take advantage of. This module,
 * together with Device, provide the following features:
 *
 *  - Support multiple slave devices sharing the same bus
 * 	- Custom CS multiplexing supported via callbacks. For example, routing CS2 via HC138
 * 	3:8 decoder allows 8 (or more) SPI devices to share the same bus.
 * 	- Use of HSPI (SPI1) using either its own pins or sharing pins with SPI0 (overlapped)
 * 	- (Potentially) enabling use of dual/quad operating modes when overlapped
 * 	- Making use of hardware command/address/data phases for best efficiency
 * 	- Pre-calculation of all register values to optimise switching between slave devices
 *  - Write-only transactions can return immediately rather than waiting for the transfer to
 *  complete. The time spent waiting can be used to prepare the next transaction which can
 *  potentially double the throughput
 *  - Interrupt callback on transaction completion. This can be used to improve system efficiency
 *  on slower devices.
 *  - (Potentially) Using DMA for larger read/write transactions. The SDK only demonstrates
 *  DMA for I2S services but it _may_ be possible to use it for SPI.
 *
 *
 * # Transactions
 *
 * Applications call Controller to perform a transfer, or sequence of transfers, as follows:
 *
 *	- Session setup
 *		- Wait for any HSPI transaction to complete (WAIT_READY)
 * 		- Configure clock & mode settings
 * 	- Transaction
 * 		- WAIT_READY
 * 		- Configure command / address / data
 * 		- Start operation
 * 		- If read required:
 * 			- WAIT_READY
 * 			- Copy data from FIFO
 *
 *	Transaction may be repeated for subsequent transfers on same device
 *	CS will be asserted/de-asserted by hardware so not need to end a transaction
 *
 * # Overlapped operation
 *
 * Both SPI controllers are able to share the pin signals from the flash SPI interface (SPI0).
 * This is handled through hardware.
 *
 * Advantages:
 * 	- Gain three pins (GPIO12-14), which liberates the I2S controller
 * 	- Dual and quad SPI modes can be used with HSPI
 *
 * Disadvantages:
 * 	- Slow SPI devices may reduce retrieval speed of program code from Flash memory
 *
 *	A primary IO MUX (PERIPHS_IO_MUX_CONF_U) selects whether the CPU clock goes through the
 * 	SPI clock divider circuitry or not. In overlapped mode the SPI0 setting is used for both,
 * 	therefore as most SPI slave devices will not operate at 80MHz this setting has to be disabled
 * 	to allow the clocks to be set independently. See PERIPHS_IO_MUX_CONF_U.
 *
 * The ESP32 Technical Reference manual gives a very useful insight into how the two SPI
 * devices work together, as the hardware appears largely similar.
 *
 ****/

#pragma once

#include <stdint.h>
#include <esp_attr.h>
#include "Request.h"
#include <bitset>
#include "Common.h"

#ifdef ARCH_ESP32
#include <soc/soc_caps.h>
struct spi_transaction_t;
struct spi_device_t;
#endif

namespace HSPI
{
class Device;

#ifdef ARCH_ESP32
struct EspTransaction;
#endif

static constexpr uint8_t SPI_PIN_NONE{0xff};
static constexpr uint8_t SPI_PIN_DEFAULT{0xfe};

/**
 * @brief Identifies bus selection
 */
enum class SpiBus {
	INVALID = 0,
	MIN = 1,
	SPI1 = 1,
#ifdef ARCH_ESP32
	SPI2 = 2,
	SPI3 = 3,
	MAX = SOC_SPI_PERIPH_NUM,
	DEFAULT = SPI2,
#else
	DEFAULT = SPI1,
#endif
};

/**
 * @brief SPI pin connections
 */
struct SpiPins {
	uint8_t sck{SPI_PIN_DEFAULT};
	uint8_t miso{SPI_PIN_DEFAULT};
	uint8_t mosi{SPI_PIN_DEFAULT};
	uint8_t ss{SPI_PIN_DEFAULT};
};

/**
 * @brief Manages access to SPI hardware
 *
 * @ingroup hw_spi
 */
class Controller
{
public:
#ifdef ARCH_ESP32
	static constexpr size_t hardwareBufferSize{4096 - 4}; // SPI_MAX_DMA_LEN
#else
	static constexpr size_t hardwareBufferSize{64};
#endif

	struct Config {
#ifdef ARCH_ESP32
		spi_device_t* handle;
#else
		bool dirty{true}; ///< Set when values require updating
		// Pre-calculated register values - see updateConfig()
		struct {
			uint32_t clock{0};
			uint32_t ctrl{0};
			uint32_t pin{0};
			uint32_t user{0};
			uint32_t user1{0};
		} reg;
#endif
	};

	/**
	 * @brief Interrupt callback for custom Controllers
	 * @param chipSelect The value passed to `startDevice()`
	 * @param active true when transaction is about to start, false when completed
	 *
	 * For manual CS (PinSet::manual) the actual CS GPIO must be asserted/de-asserted.
	 *
	 * Expanding the SPI bus using a HC138 3:8 multiplexer, for example, can also
	 * be handled here, setting the GPIO address lines appropriately.
	 */
	using SelectDevice = void (*)(uint8_t chipSelect, bool active);

	Controller(SpiBus id = SpiBus::DEFAULT) : busId(id)
	{
	}

	Controller(SpiBus id, SpiPins pins) : busId(id), pins(pins)
	{
	}

	virtual ~Controller();

	/* @brief Initialize the HSPI controller
	 */
	bool begin();

	/** @brief Disable HSPI controller
	 * 	@note Reverts HSPI pins to GPIO and disables the controller
	 */
	void end();

	/**
	 * @brief Set interrupt callback to use for manual CS control (PinSet::manual)
	 * or if CS pin is multiplexed.
	 *
	 * @note Callback MUST be marked IRAM_ATTR
	 */
	void onSelectDevice(SelectDevice callback)
	{
		selectDeviceCallback = callback;
	}

	/**
	 * @brief Assign a device to a CS# using a specific pin set.
	 * Only one device may be assigned to any CS.
	 *
	 * Custom controllers should override this method to verify/configure chip selects,
	 * and also provide a callback (via `onSelectDevice()`).
	 */
	virtual bool startDevice(Device& dev, PinSet pinSet, uint8_t chipSelect, uint32_t clockSpeed);

	/**
	 * @brief Release CS for a device.
	 */
	virtual void stopDevice(Device& dev);

	/**
	 * @brief Devices call this method to tell the Controller about configuration changes.
	 * Internally, we just set a flag and update the register values when required.
	 */
	void configChanged(Device& dev);

	/**
	 * @brief Get the active bus identifier
	 *
	 * On successful call to begin() returns actual bus in use.
	 */
	SpiBus getBusId() const
	{
		return busId;
	}

#ifdef ARCH_ESP32
	/**
	 * @brief Get the active ESP32 SPI host identifier
	 */
	uint8_t getHost() const
	{
		return unsigned(busId) - 1;
	}
#endif

#ifdef HSPI_ENABLE_STATS
	struct Stats {
		uint32_t requestCount;   ///< Completed requests
		uint32_t transCount;	 ///< Completed SPI transactions
		uint32_t waitCycles;	 ///< Total blocking CPU cycles
		uint32_t tasksQueued;	///< Number of times task callback registered for async execution (no interrupts)
		uint32_t tasksCancelled; ///< Tasks cancelled by blocking requests

		void clear() volatile
		{
			requestCount = 0;
			transCount = 0;
			waitCycles = 0;
			tasksQueued = 0;
			tasksCancelled = 0;
		}
	};
	static volatile Stats stats;
#endif

	PinSet IRAM_ATTR getActivePinSet() const
	{
		return activePinSet;
	}

	void wait(Request& request);

protected:
	friend Device;

	virtual void execute(Request& request);

private:
#ifdef ARCH_ESP32
	static void IRAM_ATTR pre_transfer_callback(spi_transaction_t* t);
	static void IRAM_ATTR post_transfer_callback(spi_transaction_t* t);
#endif

	static void updateConfig(Device& dev);

	void queueTask();
	void executeTask();
	void startRequest();
	void nextTransaction();
	static void isr(Controller* spi);
	void transactionDone();

	SpiBus busId;
	SpiPins pins;
	PinSet activePinSet{PinSet::none};
	SelectDevice selectDeviceCallback{nullptr}; ///< Callback for custom controllers
	uint8_t normalDevices{0};					///< Number of registered devices using HSPI pins (SPI1)
#ifndef ARCH_ESP32
	uint8_t overlapDevices{0};		 ///< Number of registered devices using overlap pins (SPI0)
	std::bitset<8> chipSelectsInUse; ///< Ensures each CS is used only once
#endif
	struct Flags {
		bool initialised : 1;
#ifndef ARCH_ESP32
		bool spi0ClockChanged : 1; ///< SPI0 clock MUX setting was changed for a transaction
		bool taskQueued : 1;
#endif
	};
	Flags flags{};

	// State of the current transaction in progress
	struct Transaction {
		Request* request;   ///< The current request being executed
		uint32_t addr;		///< Address for next transfer
		uint16_t outOffset; ///< Where to read data for next outgoing transfer
		uint16_t inOffset;  ///< Where to write incoming data from current transfer
		uint8_t inlen;		///< Incoming data for current transfer
		IoMode ioMode;
		// Flags
		uint8_t bitOrder : 1;
		volatile uint8_t busy : 1;
		uint8_t addrShift;	///< How many bits to shift address left
		uint32_t addrCmdMask; ///< In SDI/SQI modes this is combined with address
	};
	Transaction trans{};
#ifdef ARCH_ESP32
	EspTransaction* esp_trans{nullptr};
	uint32_t dmaBuffer[hardwareBufferSize / sizeof(uint32_t)];
#endif
};

} // namespace HSPI
