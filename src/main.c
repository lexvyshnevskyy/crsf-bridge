/*
 * CRSF baud-rate bridge for STM32F103C6T6
 *
 * USART2 @ 400000  <->  CRSF (receiver or TX12/TX16)
 * USART1 @ 921600 <->  CH9121 (highest rate in Default clock mode)
 *
 * Bidirectional transparent byte pipe. Same binary on both boards.
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
#ifndef CRSF_BAUD
#define CRSF_BAUD     400000u
#endif
#define ETH_BAUD      921600u

/* Ring buffers: sized for CRSF bursts with headroom on the 921600 link */
#define RING_SIZE     512u
#define RING_MASK     (RING_SIZE - 1u)

typedef struct {
	volatile uint16_t head;
	volatile uint16_t tail;
	uint8_t data[RING_SIZE];
} ring_t;

static ring_t ring_crsf_to_eth;
static ring_t ring_eth_to_crsf;

#ifdef CRSF_MODULE_BAY
/* PA2 idles low so the TX transistor releases bay pin 5. */
#define BAY_FRAME_GAP_US 80u

static volatile uint8_t bay_tx_active;
static volatile uint8_t bay_tx_left;
static volatile uint16_t bay_last_rx;
#ifdef CRSF_APPEND_FF_MARKER
static volatile uint8_t bay_ff_pending;
#endif

static void bay_timer_setup(void)
{
	rcc_periph_clock_enable(RCC_TIM2);
	timer_set_prescaler(TIM2, 71); /* 72 MHz / 72 = 1 us */
	timer_set_period(TIM2, 0xffff);
	timer_generate_event(TIM2, TIM_EGR_UG);
	timer_enable_counter(TIM2);
}

static void bay_release_tx_pin(void)
{
	gpio_clear(GPIOA, GPIO_USART2_TX);
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_2_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL, GPIO_USART2_TX);
}

static void bay_drive_tx_pin(void)
{
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART2_TX);
}
#endif

/* PC13 active-low LED on many F103 boards (optional) */
#define LED_PORT GPIOC
#define LED_PIN  GPIO13

static inline int ring_empty(const ring_t *r)
{
	return r->head == r->tail;
}

static inline int ring_full(const ring_t *r)
{
	return ((r->head + 1u) & RING_MASK) == r->tail;
}

static inline void ring_push(ring_t *r, uint8_t b)
{
	if (ring_full(r)) {
		return; /* drop on overflow; prefer freshness over stall */
	}
	r->data[r->head] = b;
	r->head = (r->head + 1u) & RING_MASK;
}

static inline int ring_pop(ring_t *r, uint8_t *b)
{
	if (ring_empty(r)) {
		return 0;
	}
	*b = r->data[r->tail];
	r->tail = (r->tail + 1u) & RING_MASK;
	return 1;
}

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

static void kick_tx(uint32_t usart)
{
	USART_CR1(usart) |= USART_CR1_TXEIE;
}

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
#ifdef CRSF_APPEND_FF_MARKER
			bay_ff_pending = 1;
#endif
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

static void systick_setup(void)
{
	/* 1 ms tick @ 72 MHz */
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_set_reload(72000 - 1);
	systick_clear();
	systick_interrupt_enable();
	systick_counter_enable();
}

static volatile uint32_t millis;

void sys_tick_handler(void)
{
	millis++;
}

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

#ifdef CRSF_APPEND_FF_MARKER
		/* Diagnostic delimiter: append 0xFF after each bay RX burst. */
		if (bay_ff_pending && bay_rx_idle_us > BAY_FRAME_GAP_US) {
			bay_ff_pending = 0;
			ring_push(&ring_crsf_to_eth, 0xffu);
			kick_tx(USART1);
		}
#endif

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
