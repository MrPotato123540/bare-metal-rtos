#include "led.h"

//There is a 32-bit number, it will represent our register
#define GPIOAEN	(1U<<0) //0b 0000 0000 0000 0000 0000 0000 0000 0000 0001
#define LED_PIN (1U<<5)

void led_init(void) {
	//Enable clock access led port (Port A)

	//Everything in this register will set to 0
	//Example of 'friendly programming'
	//Initial State 0b 0000 0000 0000 0000 0000 1100 0000 0000 0000
	//Set bit0 = (1u<<0) 0b 0000 0000 0000 0000 0000 0000 0000 0000 0001
	//Final state = Initial state OR set bit0 = 0b 0000 0000 0000 0000 0000 1100 0000 0000 0001

	RCC->AHB1ENR |= GPIOAEN;

	//Set led pin as output pin
	//we will use direction and data register
	//direction means the pin should be an input to accept voltage levels or an output to give out voltage levels
	//data register is the register where the data is kept
	//in toda2s design these two are compulsory
	//we will configure it to output mode
	GPIOA->MODER |= (1U<<10); //bit 10 to 1
	GPIOA->MODER &= (1U<<11); //bit 11 to 0
	//we configure it as 10 because in datasheet 10 is output mode
}

void led_on(void) {
	//Set led pin HIGH (PA5)
	//data register is not one register, it's been broken into input and output data register
	GPIOA->ODR |= LED_PIN;
}

void led_off(void) {
	//Set led pin LOW (PA5)
	GPIOA->ODR &= ~LED_PIN;
}
