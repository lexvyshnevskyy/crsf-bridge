/**
 * @file main.c
 * @brief Transparent CRSF-to-CH9121 UART bridge for STM32F103C6T6.
 *
 * @details USART2 carries CRSF at 400000 baud (or the configured override),
 * while USART1 connects to a CH9121 at 921600 baud. Interrupt-driven ring
 * buffers provide a bidirectional byte-transparent path. When
 * CRSF_MODULE_BAY is defined, USART2 is operated as an inverted half-duplex
 * TX12/TX16 module-bay interface.
 */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>

/*
 * Radio side (TX12 module bay, SineLink master): 400000.
 * Aircraft side (SineLink slave UART1, full duplex):  416700.
 * Override with -DCRSF_BAUD=... in platformio.ini.
 */
/** @brief CRSF-side UART baud rate; may be overridden by the build. */
#ifndef CRSF_BAUD
#define CRSF_BAUD     400000u
#endif
/** @brief CH9121-side UART baud rate. */
#define ETH_BAUD      921600u

/* Ring buffers: sized for CRSF bursts with headroom on the 921600 link */
#define RING_SIZE     512u
#define RING_MASK     (RING_SIZE - 1u)

/** @brief Lock-free single-producer/single-consumer byte ring buffer. */
typedef struct {
	volatile uint16_t head;
	volatile uint16_t tail;
	uint8_t data[RING_SIZE];
} ring_t;

/** @brief Bytes waiting to travel from CRSF to Ethernet. */
static ring_t ring_crsf_to_eth;
/** @brief Bytes waiting to travel from Ethernet to CRSF. */
static ring_t ring_eth_to_crsf;

#ifdef CRSF_MODULE_BAY
/* PA2 idles low so the TX transistor releases bay pin 5. */
#define BAY_FRAME_GAP_US 80u

static volatile uint8_t bay_tx_active;
static volatile uint8_t bay_tx_left;
static volatile uint16_t bay_last_rx;

/** @brief Configure TIM2 as a free-running 1 MHz module-bay gap timer. */
static void bay_timer_setup(void)
{
	rcc_periph_clock_enable(RCC_TIM2);
	timer_set_prescaler(TIM2, 71); /* 72 MHz / 72 = 1 us */
	timer_set_period(TIM2, 0xffff);
	timer_generate_event(TIM2, TIM_EGR_UG);
	timer_enable_counter(TIM2);
}

/** @brief Drive PA2 low as GPIO so the external inverter releases bay pin 5. */
static void bay_release_tx_pin(void)
{
	gpio_clear(GPIOA, GPIO_USART2_TX);
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_2_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL, GPIO_USART2_TX);
}

/** @brief Return PA2 to the USART2 alternate function for transmission. */
static void bay_drive_tx_pin(void)
{
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART2_TX);
}
#endif

/* PC13 active-low LED on many F103 boards (optional) */
#define LED_PORT GPIOC
#define LED_PIN  GPIO13

/** @brief Test whether a ring contains no bytes. @param[in] r Ring to test. @return Nonzero when empty. */
static inline int ring_empty(const ring_t *r)
{
	return r->head == r->tail;
}

/** @brief Test whether a ring has no free slot. @param[in] r Ring to test. @return Nonzero when full. */
static inline int ring_full(const ring_t *r)
{
	return ((r->head + 1u) & RING_MASK) == r->tail;
}

/** @brief Append one byte, dropping it if the ring is full. @param[in,out] r Destination ring. @param[in] b Byte to append. */
static inline void ring_push(ring_t *r, uint8_t b)
{
	if (ring_full(r)) {
		return; /* drop on overflow; prefer freshness over stall */
	}
	r->data[r->head] = b;
	r->head = (r->head + 1u) & RING_MASK;
}

/** @brief Remove one byte from a ring. @param[in,out] r Source ring. @param[out] b Received byte. @return One on success, zero if empty. */
static inline int ring_pop(ring_t *r, uint8_t *b)
{
	if (ring_empty(r)) {
		return 0;
	}
	*b = r->data[r->tail];
	r->tail = (r->tail + 1u) & RING_MASK;
	return 1;
}

/** @brief Configure the 8 MHz HSE, 72 MHz system clock, and peripheral clocks. */
static void clock_setup(void)
{
	/* HSE 8 MHz -> PLL x9 = 72 MHz (standard Blue Pill / many F103 boards) */
	rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);

	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOC);
	rcc_periph_clock_enable(RCC_AFIO);
	rcc_periph_clock_enable(RCC_USART1);
	rcc_periph_clock_enable(RCC_USART2);
}

/** @brief Configure both USART pin pairs and the optional status LED. */
static void gpio_setup(void)
{
	/* USART1 (CH9121): PA9 TX, PA10 RX */
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);
	gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
		      GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);

	/* USART2 (CRSF): PA2 TX, PA3 RX */
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART2_TX);
	gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
		      GPIO_CNF_INPUT_FLOAT, GPIO_USART2_RX);

	/* Status LED */
	gpio_set_mode(LED_PORT, GPIO_MODE_OUTPUT_2_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL, LED_PIN);
	gpio_set(LED_PORT, LED_PIN); /* off (active low) */
}

/**
 * @brief Configure and enable one 8N1 USART.
 * @param[in] usart libopencm3 USART peripheral identifier.
 * @param[in] baud Requested baud rate.
 * @param[in] irqn NVIC interrupt number for the peripheral.
 */
static void usart_setup_one(uint32_t usart, uint32_t baud, uint8_t irqn)
{
	usart_set_baudrate(usart, baud);
	usart_set_databits(usart, 8);
	usart_set_stopbits(usart, USART_STOPBITS_1);
	usart_set_parity(usart, USART_PARITY_NONE);
	usart_set_mode(usart, USART_MODE_TX_RX);
	usart_set_flow_control(usart, USART_FLOWCONTROL_NONE);

	usart_enable_rx_interrupt(usart);
	/* TXE enabled on demand when data is queued */

	nvic_set_priority(irqn, 0);
	nvic_enable_irq(irqn);

	usart_enable(usart);
}

/** @brief Configure the CH9121 and CRSF UARTs. */
static void usart_setup(void)
{
	/* USART1 on APB2 @ 72 MHz → 921600 (BRR 0x4E, +0.16%) */
	usart_setup_one(USART1, ETH_BAUD, NVIC_USART1_IRQ);
	/* USART2 on APB1 @ 36 MHz → 400000 exact, 416700 within 0.02% */
	usart_setup_one(USART2, CRSF_BAUD, NVIC_USART2_IRQ);
#ifdef CRSF_MODULE_BAY
	bay_timer_setup();
	bay_release_tx_pin();
#endif
}

/** @brief Enable TX-empty interrupts to start or resume queued transmission. @param[in] usart USART to start. */
static void kick_tx(uint32_t usart)
{
	USART_CR1(usart) |= USART_CR1_TXEIE;
}

/** @brief Service CH9121-side receive and transmit interrupts. */
void usart1_isr(void)
{
	/* CH9121 <-> eth ring */
	if ((USART_SR(USART1) & USART_SR_RXNE) != 0) {
		uint8_t b = usart_recv(USART1);
		ring_push(&ring_eth_to_crsf, b);
#ifndef CRSF_MODULE_BAY
		kick_tx(USART2);
#endif
	}

	if ((USART_CR1(USART1) & USART_CR1_TXEIE) &&
	    (USART_SR(USART1) & USART_SR_TXE)) {
		uint8_t b;
		if (ring_pop(&ring_crsf_to_eth, &b)) {
			usart_send(USART1, b);
		} else {
			USART_CR1(USART1) &= ~USART_CR1_TXEIE;
		}
	}
}

/** @brief Service CRSF-side receive, transmit, and half-duplex completion. */
void usart2_isr(void)
{
	/* CRSF <-> crsf ring */
	if ((USART_SR(USART2) & USART_SR_RXNE) != 0) {
		uint8_t b = usart_recv(USART2);
#ifdef CRSF_MODULE_BAY
		/* Drop the echo of our own transmission. */
		if (!bay_tx_active) {
			ring_push(&ring_crsf_to_eth, b);
			kick_tx(USART1);
			bay_last_rx = (uint16_t)TIM_CNT(TIM2);
		}
#else
		ring_push(&ring_crsf_to_eth, b);
		kick_tx(USART1);
#endif
	}

	if ((USART_CR1(USART2) & USART_CR1_TXEIE) &&
	    (USART_SR(USART2) & USART_SR_TXE)) {
		uint8_t b;
#ifdef CRSF_MODULE_BAY
		if (bay_tx_left && ring_pop(&ring_eth_to_crsf, &b)) {
			usart_send(USART2, b);
			bay_tx_left--;
		} else {
			USART_CR1(USART2) &= ~USART_CR1_TXEIE;
			USART_CR1(USART2) |= USART_CR1_TCIE;
		}
#else
		if (ring_pop(&ring_eth_to_crsf, &b)) {
			usart_send(USART2, b);
		} else {
			USART_CR1(USART2) &= ~USART_CR1_TXEIE;
		}
#endif
	}

#ifdef CRSF_MODULE_BAY
	if ((USART_CR1(USART2) & USART_CR1_TCIE) &&
	    (USART_SR(USART2) & USART_SR_TC)) {
		USART_CR1(USART2) &= ~USART_CR1_TCIE;
		USART_SR(USART2) &= ~USART_SR_TC;
		bay_release_tx_pin();
		bay_tx_active = 0;
	}
#endif
}

/** @brief Configure SysTick to generate a 1 ms system timebase. */
static void systick_setup(void)
{
	/* 1 ms tick @ 72 MHz */
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_set_reload(72000 - 1);
	systick_clear();
	systick_interrupt_enable();
	systick_counter_enable();
}

/** @brief Milliseconds elapsed since startup. */
static volatile uint32_t millis;

/** @brief Increment the millisecond timebase from the SysTick interrupt. */
void sys_tick_handler(void)
{
	millis++;
}

/** @brief Initialize the bridge and run its forwarding loop. @return Unreachable. */
int main(void)
{
	clock_setup();
	gpio_setup();
	usart_setup();
	systick_setup();

	uint32_t last_blink = 0;

	while (1) {
		/* Heartbeat ~2 Hz: firmware is alive */
		if ((millis - last_blink) >= 250u) {
			last_blink = millis;
			gpio_toggle(LED_PORT, LED_PIN);
		}
#ifdef CRSF_MODULE_BAY
		uint16_t bay_rx_idle_us =
			(uint16_t)(TIM_CNT(TIM2) - bay_last_rx);
		/* Send only after the radio frame has ended (~3 byte times). */
		if (!bay_tx_active && !ring_empty(&ring_eth_to_crsf)) {
			if (bay_rx_idle_us > BAY_FRAME_GAP_US) {
				bay_tx_active = 1;
				bay_tx_left = 64;
				bay_drive_tx_pin();
				kick_tx(USART2);
			}
		}
#else
		__asm__("wfi");
#endif
	}

	return 0;
}
