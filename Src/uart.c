#include <stdio.h>
#include <stdint.h>
#include "uart.h"
#include "osKernel.h" // We need this to use RTOS yield function
#include "stm32f4xx.h"

#define GPIOAEN (1U<<0)
#define UART2EN (1U<<17)

#define CR1_RE      (1U<<2)  // This is for USART's control register's receiver enable bit
#define CR1_RXNEIE  (1U<<5)  // This is for USART's control register's RX Not Empty interrupt enable bit
#define SR_RXNE     (1U<<5)  // Status register's Read Data Register Not Empty flag

static uint16_t compute_uart_bd(uint32_t periph_clk, uint32_t baudrate);
static void uart_set_baudrate(uint32_t periph_clk, uint32_t baudrate);
static void uart_write(int ch);
 
FILE __stdout; //for printf
FILE __stdin; //for scanf

//printf function to hardware
int fputc(int ch, FILE *f) {
    return ITM_SendChar(ch); 
}

#define SYS_FREQ 16000000 //16Mhz internal clock for STM32F411 boards
#define APB1_CLK SYS_FREQ
#define UART_BAUDRATE 115200

#define CR1_TE (1U<<3) //This is for USART's control register's transmitter enable bit
#define CR1_UE (1U<<13) //This is for USART's control register's UART enable bit

#define SR_TXE (1U<<7) //We need to make sure status register's 7th bit is empty to confirm transmit data register is empty

void uart_tx_init(void) {
	//Each peripheral requires clock access
	//In default clock access mechanism is disabled so we need to enable it
	//Also we need to enable clock access to the UART
	//Before it, UART peripheral requires pins and it have RX, TX
	//We need to configure these pins in alternate function mode because GPIO pins work as either inputs or outputs
	//We would use USART2

	//Enable clock access to GPIOA
	RCC->AHB1ENR |= GPIOAEN;

	//Set PA2 mode to alternate function mode
	GPIOA->MODER &= ~(1U<<4); //bit 4 to 0
	GPIOA->MODER |= (1U<<5); //bit 5 to 1

	//Set alternate function type to AF7 (UART2_TX)
	//We will use alternate function low register
	//We will chose 2nd one
	//0111: AF7
	//First index is AFRL and second is AFRH
	GPIOA->AFR[0] |= (1U<<8);
	GPIOA->AFR[0] |= (1U<<9);
	GPIOA->AFR[0] |= (1U<<10);
	GPIOA->AFR[0] &= ~(1U<<11);


	//Enable clock access to UART
	//We will use APB1 peripheral clock to enable register for USART2
	RCC->APB1ENR |= UART2EN;

	//Configure baud rate
	uart_set_baudrate(APB1_CLK, UART_BAUDRATE);

	//Configure transfer direction
	USART2->CR1 = CR1_TE;

	//Enable UART module
	//This time we make OR operation because we don't want to clean TE configuration
	USART2->CR1 |= CR1_UE;
}

static void uart_write(int ch) {
		// We need to make sure status register's 7th bit (TXE) is empty to confirm transmit data register is empty
    // Polling (empty while loop) is dangerous for RTOS because it blocks the CPU
    // Instead, we yield the thread so CPU can execute other tasks while waiting for hardware
    while (!(USART2->SR & SR_TXE)) {
        osThreadYield(); // Give up remaining time quanta to the next thread
    }

	//Write to transmit data register
	//8-bit data
	USART2->DR = (ch & 0xFF);
}

void uart_rx_interrupt_init(void) {
	// Enable clock access to GPIOA
	RCC->AHB1ENR |= GPIOAEN;

	// Set PA3 mode to alternate function mode (UART2_RX is on PA3)
	GPIOA->MODER &= ~(1U<<6); // bit 6 to 0
	GPIOA->MODER |= (1U<<7);  // bit 7 to 1

	// Set alternate function type to AF7 (UART2_RX)
	// We will use alternate function low register
	// 0111: AF7 for Pin 3 (bits 12, 13, 14, 15)
	GPIOA->AFR[0] |= (1U<<12);
	GPIOA->AFR[0] |= (1U<<13);
	GPIOA->AFR[0] |= (1U<<14);
	GPIOA->AFR[0] &= ~(1U<<15);

	// Enable clock access to UART
	RCC->APB1ENR |= UART2EN;

	// Configure baud rate
	uart_set_baudrate(APB1_CLK, UART_BAUDRATE);

	// Configure transfer direction (Receiver Enable)
	USART2->CR1 |= CR1_RE;

	// Enable RX Not Empty Interrupt
	// This will trigger an interrupt whenever data is received
	USART2->CR1 |= CR1_RXNEIE;

	// Enable UART module
	USART2->CR1 |= CR1_UE;

	// Enable USART2 interrupt in NVIC (Nested Vectored Interrupt Controller)
	NVIC_EnableIRQ(USART2_IRQn);
}

static void uart_set_baudrate(uint32_t periph_clk, uint32_t baudrate) {
	//BRR = baud rate register
	USART2->BRR = compute_uart_bd(periph_clk, baudrate);
}

static uint16_t compute_uart_bd(uint32_t periph_clk, uint32_t baudrate) {
	return ((periph_clk + (baudrate / 2U)) / baudrate);
}


