#include <rcc/rcc.h>
#include <gpio/gpio.h>
#include <interrupt/interrupt.h>
#include <timer/timer.h>
#include <dma/dma.h>
#include <adc/adc_f3.h>
#include <os/time.h>
#include <spi/spi.h>
#include <usb/usb.h>
#include <usb/descriptor.h>

#include "report_desc.h"
#include "usb_strings.h"
#include "configloader.h"
#include "config.h"

static uint32_t& reset_reason = *(uint32_t*)0x10000000;

static bool do_reset_bootloader;
static bool do_reset;

void reset() {
	SCB.AIRCR = (0x5fa << 16) | (1 << 2); // SYSRESETREQ
}

void reset_bootloader() {
	reset_reason = 0xb007;
	reset();
}

Configloader configloader(0x801f800);

config_t config;

auto dev_desc = device_desc(0x200, 0, 0, 0, 64, 0x1d50, 0x6080, 0x110, 1, 2, 3, 1);
auto conf_desc = configuration_desc(1, 1, 0, 0xc0, 0,
	// HID interface.
	interface_desc(0, 0, 1, 0x03, 0x00, 0x00, 0,
		hid_desc(0x111, 0, 1, 0x22, sizeof(report_desc)),
		endpoint_desc(0x81, 0x03, 16, 1)
	)
);

desc_t dev_desc_p = {sizeof(dev_desc), (void*)&dev_desc};
desc_t conf_desc_p = {sizeof(conf_desc), (void*)&conf_desc};
desc_t report_desc_p = {sizeof(report_desc), (void*)&report_desc};

static Pin usb_dm = GPIOA[11];
static Pin usb_dp = GPIOA[12];
static Pin usb_pu = GPIOA[15];

static PinArray button_inputs = GPIOB.array(0, 10);
static PinArray button_leds = GPIOC.array(0, 10);

static Pin qe1a = GPIOA[0];
static Pin qe1b = GPIOA[1];
static Pin qe2a = GPIOA[6];
static Pin qe2b = GPIOA[7];

static Pin led1 = GPIOA[8];
static Pin led2 = GPIOA[9];

static Pin spi_ack = GPIOB[11];
static Pin spi_nss = GPIOB[12];
static Pin spi_sck = GPIOB[13];
static Pin spi_miso = GPIOB[14];
static Pin spi_mosi = GPIOB[15];

RBLog<256, 2> ps_rblog;

class SPI_PS {
	private:
		uint32_t last_byte_time;

		int32_t pos;

		uint32_t state;

		bool received_byte(uint8_t data) {
			if(Time::time() - last_byte_time > 2) {
				pos = 0; // Assume start of new command.
			}

			last_byte_time = Time::time();

			if(pos == -1) {
				return false;
			}

			ps_rblog.log("pos = %d", pos);

			if(pos == 0 && data != 0x01) {
				pos = -1;
				return false;
			}

			if(pos == 1 && data != 0x42) {
				pos = -1;
				return false;
			}

			if(pos > 4) {
				pos = -1;
				return false;
			}

			switch(pos) {
				case 0:
					SPI2.reg.DR8 = 0x41;
					break;

				case 1:
					SPI2.reg.DR8 = 0x5a;
					break;

				case 2:
					SPI2.reg.DR8 = state & 0xff;
					break;

				case 3:
					SPI2.reg.DR8 = (state >> 8) & 0xff;
					break;

				case 4:
					//SPI2.reg.DR8 = 0xff;
					break;
			}

			pos++;

			return true;
		}

	public:
		void init() {
			RCC.enable(RCC.SPI2);

			spi_ack.on();
			spi_ack.set_type(Pin::OpenDrain);
			spi_ack.set_mode(Pin::Output);

			spi_nss.set_af(5);
			spi_nss.set_type(Pin::OpenDrain);
			spi_nss.set_mode(Pin::AF);

			spi_sck.set_af(5);
			spi_sck.set_type(Pin::OpenDrain);
			spi_sck.set_mode(Pin::AF);

			spi_miso.set_af(5);
			spi_miso.set_type(Pin::OpenDrain);
			spi_miso.set_mode(Pin::AF);

			spi_mosi.set_af(5);
			spi_mosi.set_type(Pin::OpenDrain);
			spi_mosi.set_mode(Pin::AF);

			SPI2.reg.CR2 = (1 << 12) | (7 << 8) | (1 << 6); // FRXTH, DS = 8bit, RXNEIE
			SPI2.reg.CR1 = (1 << 7) | (1 << 6) | (1 << 1); // LSBFIRST, SPE, CPOL

			Interrupt::enable(Interrupt::SPI2);
		}

		void spi_irq() {
			ps_rblog.log("SPI2 interrupt, SR = %#06x", SPI2.reg.SR);

			// RXNE
			if(SPI2.reg.SR & (1 << 0)) {
				uint8_t data = SPI2.reg.DR8;

				ps_rblog.log("Received byte: %#04x", data);

				bool ack = received_byte(data);

				if(ack) {
					ps_rblog.log("ACK");
					spi_ack.off();

					for(int i = 0; i < 100; i++) {
						asm volatile("nop");
					}

					spi_ack.on();
				} else {
					ps_rblog.log("NAK");
				}
			}
		}

		void set_state(uint32_t s) {
			state = ~s;
		}
};

SPI_PS spi_ps;

template <>
void interrupt<Interrupt::SPI2>() {
	spi_ps.spi_irq();
}

USB_f1 usb(USB, dev_desc_p, conf_desc_p);

class WS2812B {
	private:
		uint8_t dmabuf[25];
		volatile uint32_t cnt;
		volatile bool busy;

		void schedule_dma() {
			cnt--;

			DMA1.reg.C[6].NDTR = 25;
			DMA1.reg.C[6].MAR = (uint32_t)&dmabuf;
			DMA1.reg.C[6].PAR = (uint32_t)&TIM4.CCR3;
			DMA1.reg.C[6].CR = (0 << 10) | (1 << 8) | (1 << 7) | (0 << 6) | (1 << 4) | (1 << 1) | (1 << 0);
		}

		void set_color(uint8_t r, uint8_t g, uint8_t b) {
			uint32_t n = 0;

			for(uint32_t i = 8; i-- > 0; n++) {
				dmabuf[n] = g & (1 << i) ? 58 : 29;
			}

			for(uint32_t i = 8; i-- > 0; n++) {
				dmabuf[n] = r & (1 << i) ? 58 : 29;
			}

			for(uint32_t i = 8; i-- > 0; n++) {
				dmabuf[n] = b & (1 << i) ? 58 : 29;
			}

			dmabuf[n] = 0;
		}

	public:
		void init() {
			RCC.enable(RCC.TIM4);
			RCC.enable(RCC.DMA1);

			Interrupt::enable(Interrupt::DMA1_Channel7);

			TIM4.ARR = (72000000 / 800000) - 1; // period = 90, 0 = 29, 1 = 58
			TIM4.CCR3 = 0;

			TIM4.CCMR2 = (6 << 4) | (1 << 3);
			TIM4.CCER = 1 << 8;
			TIM4.DIER = 1 << 8;

			GPIOB[8].set_af(2);
			GPIOB[8].set_mode(Pin::AF);
			GPIOB[8].set_pull(Pin::PullNone);

			TIM4.CR1 = 1 << 0;

			Time::sleep(1);

			update(0, 0, 0);
		}

		void update(uint8_t r, uint8_t g, uint8_t b) {
			set_color(r, g, b);

			cnt = 15;
			busy = true;

			schedule_dma();
		}

		void irq() {
			DMA1.reg.C[6].CR = 0;
			DMA1.reg.IFCR = 1 << 24;

			if(cnt) {
				schedule_dma();
			} else {
				busy = false;
			}
		}
};

WS2812B ws2812b;

template <>
void interrupt<Interrupt::DMA1_Channel7>() {
	ws2812b.irq();
}

uint32_t last_led_time;

class HID_arcin : public USB_HID {
	private:
		bool set_feature_bootloader(bootloader_report_t* report) {
			switch(report->func) {
				case 0:
					return true;

				case 0x10: // Reset to bootloader
					do_reset_bootloader = true;
					return true;

				case 0x20: // Reset to runtime
					do_reset = true;
					return true;

				default:
					return false;
			}
		}

		bool set_feature_config(config_report_t* report) {
			if(report->segment != 0) {
				return false;
			}

			configloader.write(report->size, report->data);

			return true;
		}

		bool get_feature_config() {
			config_report_t report = {0xc0, 0, sizeof(config)};

			memcpy(report.data, &config, sizeof(config));

			usb.write(0, (uint32_t*)&report, sizeof(report));

			return true;
		}

	public:
		HID_arcin(USB_generic& usbd, desc_t rdesc) : USB_HID(usbd, rdesc, 0, 1, 64) {}

	protected:
		virtual bool set_output_report(uint32_t* buf, uint32_t len) {
			if(len != sizeof(output_report_t)) {
				return false;
			}

			output_report_t* report = (output_report_t*)buf;

			last_led_time = Time::time();
			button_leds.set(report->leds);

			ws2812b.update(report->r, report->b, report->g);

			return true;
		}

		virtual bool set_feature_report(uint32_t* buf, uint32_t len) {
			switch(*buf & 0xff) {
				case 0xb0:
					if(len != sizeof(bootloader_report_t)) {
						return false;
					}

					return set_feature_bootloader((bootloader_report_t*)buf);

				case 0xc0:
					if(len != sizeof(config_report_t)) {
						return false;
					}

					return set_feature_config((config_report_t*)buf);

				default:
					return false;
			}
		}

		virtual bool get_feature_report(uint8_t report_id) {
			switch(report_id) {
				case 0xc0:
					return get_feature_config();

				default:
					return false;
			}
		}
};

HID_arcin usb_hid(usb, report_desc_p);

USB_strings usb_strings(usb, config.label);

class Axis {
	public:
		virtual uint32_t get() = 0;
};

class QEAxis : public Axis {
	private:
		TIM_t& tim;

	public:
		QEAxis(TIM_t& t) : tim(t) {}

		void enable(bool invert, int8_t sens) {
			if(!invert) {
				tim.CCER = 1 << 1;
			}

			tim.CCMR1 = (1 << 8) | (1 << 0);
			tim.SMCR = 3;
			tim.CR1 = 1;

			if(sens < 0) {
				tim.ARR = 256 * -sens - 1;
			} else {
				tim.ARR = 256 - 1;
			}
		}

		virtual uint32_t get() final {
			return tim.CNT;
		}
};

QEAxis axis_qe1(TIM2);
QEAxis axis_qe2(TIM3);

class AnalogAxis : public Axis {
	private:
		ADC_t& adc;
		uint32_t ch;

	public:
		AnalogAxis(ADC_t& a, uint32_t c) : adc(a), ch(c) {}

		void enable() {
			// Turn on ADC regulator.
			adc.CR = 0 << 28;
			adc.CR = 1 << 28;
			Time::sleep(2);

			// Calibrate ADC.
			// TODO

			// Configure continous capture on one channel.
			adc.CFGR = (1 << 13) | (1 << 12) | (1 << 5); // CONT, OVRMOD, ALIGN
			adc.SQR1 = (ch << 6);
			adc.SMPR1 = (7 << (ch * 3)); // 72 MHz / 64 / 614 = apx. 1.8 kHz

			// Enable ADC.
			adc.CR |= 1 << 0; // ADEN
			while(!(adc.ISR & (1 << 0))); // ADRDY
			adc.ISR = (1 << 0); // ADRDY

			// Start conversion.
			adc.CR |= 1 << 2; // ADSTART
		}

		virtual uint32_t get() final {
			return adc.DR >> 8;
		}
};

AnalogAxis axis_ana1(ADC1, 2);
AnalogAxis axis_ana2(ADC2, 4);

int main() {
	rcc_init();

	// Set ADC12PRES to /64.
	RCC.CFGR2 |= (0x19 << 4);

	// Initialize system timer.
	STK.LOAD = 72000000 / 8 / 1000; // 1000 Hz.
	STK.CTRL = 0x03;

	// Load config.
	configloader.read(sizeof(config), &config);

	RCC.enable(RCC.GPIOA);
	RCC.enable(RCC.GPIOB);
	RCC.enable(RCC.GPIOC);

	usb_dm.set_mode(Pin::AF);
	usb_dm.set_af(14);
	usb_dp.set_mode(Pin::AF);
	usb_dp.set_af(14);

	if(config.ps2_mode > 0) {
		spi_ps.init();
	}

	RCC.enable(RCC.USB);

	usb.init();

	usb_pu.set_mode(Pin::Output);
	usb_pu.on();

	button_inputs.set_mode(Pin::Input);
	button_inputs.set_pull(Pin::PullUp);

	button_leds.set_mode(Pin::Output);

	led1.set_mode(Pin::Output);
	led1.on();

	led2.set_mode(Pin::Output);
	led2.on();

	Axis* axis_1;

	if(0) {
		RCC.enable(RCC.ADC12);

		axis_ana1.enable();

		axis_1 = &axis_ana1;

	} else {
		RCC.enable(RCC.TIM2);

		axis_qe1.enable(config.flags & (1 << 1), config.qe1_sens);

		qe1a.set_af(1);
		qe1b.set_af(1);
		qe1a.set_mode(Pin::AF);
		qe1b.set_mode(Pin::AF);

		axis_1 = &axis_qe1;
	}

	Axis* axis_2;

	if(0) {
		RCC.enable(RCC.ADC12);

		axis_ana2.enable();

		axis_2 = &axis_ana2;

	} else {
		RCC.enable(RCC.TIM3);

		axis_qe2.enable(config.flags & (1 << 2), config.qe2_sens);

		qe2a.set_af(2);
		qe2b.set_af(2);
		qe2a.set_mode(Pin::AF);
		qe2b.set_mode(Pin::AF);

		axis_2 = &axis_qe2;
	}

	ws2812b.init();

	uint32_t last_time = 0;

	uint8_t last_x = 0;
	uint8_t last_y = 0;

	int8_t last_dir_x = 0;
	int8_t last_dir_y = 0;

	int8_t state_x = 0;
	int8_t state_y = 0;

	while(1) {
		usb.process();

		uint16_t buttons = button_inputs.get() ^ 0x7ff;

		if(do_reset_bootloader) {
			Time::sleep(10);
			reset_bootloader();
		}

		if(do_reset) {
			Time::sleep(10);
			reset();
		}

		uint32_t time = Time::time();

		if(time - last_led_time > 1000) {
			button_leds.set(buttons);
		}

		if(last_time != time) {
			last_time = time;
			uint32_t qe1_count = axis_1->get();
			uint32_t qe2_count = axis_2->get();

			int8_t rx = qe1_count - last_x;
			int8_t ry = qe2_count - last_y;

			if((last_dir_x == 1 && rx > 0) || (rx > 1)) {
				state_x = 100;
				last_x = qe1_count;
			} else if((last_dir_x == -1 && rx < 0) || (rx < -1)) {
				state_x = -100;
				last_x = qe1_count;
			} else if(state_x > 0) {
				state_x--;
				last_x = qe1_count;
			} else if(state_x < 0) {
				state_x++;
				last_x = qe1_count;
			} else {
				last_dir_x = 0;
				last_x = qe1_count;
			}

			if((last_dir_y == 1 && ry > 0) || (ry > 1)) {
				state_y = 100;
				last_y = qe2_count;
			} else if((last_dir_y == -1 && ry < 0) || (ry < -1)) {
				state_y = -100;
				last_y = qe2_count;
			} else if(state_y > 0) {
				state_y--;
				last_y = qe2_count;
			} else if(state_y < 0) {
				state_y++;
				last_y = qe2_count;
			} else {
				last_dir_y = 0;
				last_y = qe2_count;
			}

			if(rx > 0) {
				last_dir_x = 1;
			} else if(rx < 0) {
				last_dir_x = -1;
			}

			if(ry > 0) {
				last_dir_y = 1;
			} else if(ry < 0) {
				last_dir_y = -1;
			}

			if(state_x > 0) {
				buttons |= 1 << 11;
			} else if(state_x < 0) {
				buttons |= 1 << 12;
			}

			if(state_y > 0) {
				buttons |= 1 << 13;
			} else if(state_y < 0) {
				buttons |= 1 << 14;
			}

			if(config.qe1_sens < 0) {
				qe1_count /= -config.qe1_sens;
			} else if(config.qe1_sens > 0) {
				qe1_count *= config.qe1_sens;
			}

			if(config.qe2_sens < 0) {
				qe2_count /= -config.qe2_sens;
			} else if(config.qe2_sens > 0) {
				qe2_count *= config.qe2_sens;
			}

			if(config.ps2_mode == 1) {
				uint32_t ps_state = (1 << 5) | (1 << 6) | (1 << 7);
				ps_state |= buttons & (1 <<  0) ? 1 << 12 : 0; // B1
				ps_state |= buttons & (1 <<  1) ? 1 << 13 : 0; // B2
				ps_state |= buttons & (1 <<  2) ? 1 << 11 : 0; // B3
				ps_state |= buttons & (1 <<  3) ? 1 << 14 : 0; // B4
				ps_state |= buttons & (1 <<  4) ? 1 << 10 : 0; // B5
				ps_state |= buttons & (1 <<  5) ? 1 << 15 : 0; // B6
				ps_state |= buttons & (1 <<  6) ? 1 <<  9 : 0; // B7
				ps_state |= buttons & (1 <<  6) ? 1 <<  4 : 0; // B8
				ps_state |= buttons & (1 <<  6) ? 1 <<  8 : 0; // B9
				ps_state |= buttons & (1 << 10) ? 1 <<  0 : 0; // select
				ps_state |= buttons & (1 <<  9) ? 1 <<  3 : 0; // start
				spi_ps.set_state(ps_state);
			} else if(config.ps2_mode == 2 || config.ps2_mode == 3) {
				uint32_t ps_state = 0;
				ps_state |= buttons & (1 <<  0) ? 1 << 15 : 0; // B1
				ps_state |= buttons & (1 <<  1) ? 1 << 10 : 0; // B2
				ps_state |= buttons & (1 <<  2) ? 1 << 14 : 0; // B3
				ps_state |= buttons & (1 <<  3) ? 1 << 11 : 0; // B4
				ps_state |= buttons & (1 <<  4) ? 1 << 13 : 0; // B5
				ps_state |= buttons & (1 <<  5) ? 1 <<  8 : 0; // B6
				ps_state |= buttons & (1 <<  6) ? 1 <<  7 : 0; // B7
				ps_state |= buttons & (1 << 10) ? 1 <<  0 : 0; // select
				ps_state |= buttons & (1 <<  9) ? 1 <<  3 : 0; // start
				if(config.ps2_mode == 2) {
					ps_state |= buttons & (1 << 11) ? 1 <<  4 : 0; // TT+
					ps_state |= buttons & (1 << 12) ? 1 <<  6 : 0; // TT-
				} else if(config.ps2_mode == 3) {
					ps_state |= buttons & (1 << 13) ? 1 <<  4 : 0; // TT+
					ps_state |= buttons & (1 << 14) ? 1 <<  6 : 0; // TT-
				}
				spi_ps.set_state(ps_state);
			}

			if(usb.ep_ready(1)) {
				input_report_t report = {1, buttons, uint8_t(qe1_count), uint8_t(qe2_count)};

				usb.write(1, (uint32_t*)&report, sizeof(report));
			}
		}
	}
}
