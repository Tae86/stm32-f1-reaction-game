#include "device_driver.h"

/*
 * WS2812B 4EA Cascade Driver (U1 -> U3 -> U4 -> U5, LED_DIN 단일선)
 *
 * ※ 이 파일은 1601.DMA_EX_RGB_LED 프로젝트에서 coretex_final_project로
 *   그대로 이식한 것이며, 핀 하나만 변경되었습니다.
 *
 *   원래 코드는 "메인보드 회로도가 없어 정확한 핀을 확인할 수 없으므로
 *   우선 PB0으로 가정하여 작성함"이라고 되어 있었는데, coretex_final_project
 *   에서는 PB0이 이미 TIM3 부저(PWM, timer.c의 TIM3_Out_Init)로 사용 중이라
 *   그대로 가져오면 충돌합니다. 그래서 인접한 PB1로 옮겼습니다.
 *   -> WS2812 DIN이 실제로 어느 핀에 연결되어 있는지 보드 실물/커넥터(J1)로
 *      반드시 재확인하시고, 다르면 아래 WS2812_PORT / WS2812_PIN만 수정하면 됩니다.
 *
 * - 본 프로젝트(option.h)의 SYSCLK = 96,000,000 Hz 이므로
 *   Cortex-M4 DWT->CYCCNT 1 tick = 1/96MHz ≈ 10.42ns 로 계산함.
 *
 * - WS2812B datasheet 규격
 *     T0H = 0.4us  , T0L = 0.85us
 *     T1H = 0.85us , T1L = 0.4us   (모두 ±150ns)
 *     RESET(Latch) = 50us 이상 Low 유지
 *     데이터 순서 : GRB, 각 바이트는 MSB(상위비트) 먼저 전송
 */

#define WS2812_PORT     GPIOB
#define WS2812_PIN      1        // PB0(TIM3 부저)과 충돌 회피 위해 PB1로 변경
#define WS2812_RCC_BIT  1        // GPIOB Clock Enable Bit (RCC->AHB1ENR)

#define WS2812_NUM_OF_LED   4    // U1, U3, U4, U5

/* SYSCLK 96MHz 기준 cycle 수 (1cycle = 1/96,000,000 sec) */
#define WS2812_T0H_CYCLE    (unsigned int)(0.40 * (SYSCLK/1000000.0))   // 약 38
#define WS2812_T0L_CYCLE    (unsigned int)(0.85 * (SYSCLK/1000000.0))   // 약 82
#define WS2812_T1H_CYCLE    (unsigned int)(0.85 * (SYSCLK/1000000.0))   // 약 82
#define WS2812_T1L_CYCLE    (unsigned int)(0.40 * (SYSCLK/1000000.0))   // 약 38
#define WS2812_RES_CYCLE    (unsigned int)(60   * (SYSCLK/1000000.0))   // 60us (RESET 여유)

/* BSRR을 이용한 단일 사이클 Set/Reset (ODR의 Read-Modify-Write보다 타이밍이 일정함) */
#define WS2812_PIN_HIGH()   (WS2812_PORT->BSRR = (1u << WS2812_PIN))
#define WS2812_PIN_LOW()    (WS2812_PORT->BSRR = (1u << (WS2812_PIN + 16)))

/* ------------------------------------------------------------
 * DWT Cycle Counter 초기화 : 정밀한 나노초 단위 딜레이 생성용
 * (for문 NOP 딜레이는 컴파일러 최적화에 흔들리므로 사용하지 않음)
 * ------------------------------------------------------------ */
static void WS2812_DWT_Init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

static void WS2812_Delay_Cycle(unsigned int cycle)
{
	unsigned int start = DWT->CYCCNT;

	while((DWT->CYCCNT - start) < cycle);
}

/* ------------------------------------------------------------
 * GPIO 초기화 : LED_DIN => GP-Out, Push-Pull, Very High Speed
 *
 * ※ 리셋(make run/리셋버튼) 직후 GPIO가 아직 설정되기 전 짧은 구간 동안
 *   DIN 핀이 플로팅되어 노이즈를 주울 수 있음 → Pull-Down으로 방지 +
 *   부팅 시 강제로 긴 Reset(WS2812_RES_CYCLE*3)을 한 번 넣어서
 *   이전 글리치 데이터가 남아있어도 확실히 0비트부터 다시 시작하게 함
 * ------------------------------------------------------------ */
void WS2812_Init(void)
{
	Macro_Set_Bit(RCC->AHB1ENR, WS2812_RCC_BIT);				// Port Clock On

	Macro_Write_Block(WS2812_PORT->MODER, 0x3, 0x1, WS2812_PIN*2);	// GP-Out
	Macro_Clear_Bit(WS2812_PORT->OTYPER, WS2812_PIN);				// Push-Pull
	Macro_Write_Block(WS2812_PORT->OSPEEDR, 0x3, 0x3, WS2812_PIN*2);	// Very High Speed
	Macro_Write_Block(WS2812_PORT->PUPDR, 0x3, 0x2, WS2812_PIN*2);	// Pull-Down (플로팅 방지)

	WS2812_PIN_LOW();										// 초기 Low

	WS2812_DWT_Init();

	WS2812_Delay_Cycle(WS2812_RES_CYCLE * 3);				// 부팅 직후 강제 긴 Reset (재동기화)
}

/* 비트 1개 전송 : T0H/T0L 또는 T1H/T1L 파형 생성 */
static void WS2812_Send_Bit(unsigned int bit)
{
	if(bit)
	{
		WS2812_PIN_HIGH();
		WS2812_Delay_Cycle(WS2812_T1H_CYCLE);
		WS2812_PIN_LOW();
		WS2812_Delay_Cycle(WS2812_T1L_CYCLE);
	}
	else
	{
		WS2812_PIN_HIGH();
		WS2812_Delay_Cycle(WS2812_T0H_CYCLE);
		WS2812_PIN_LOW();
		WS2812_Delay_Cycle(WS2812_T0L_CYCLE);
	}
}

/* 1 Byte(8bit) 전송, MSB(상위비트) 먼저 */
static void WS2812_Send_Byte(unsigned char data)
{
	int i;

	for(i = 7; i >= 0; i--)
	{
		WS2812_Send_Bit((data >> i) & 0x1);
	}
}

/* Pixel 1개(24bit) 전송 : datasheet 규정대로 G -> R -> B 순서 */
static void WS2812_Send_Pixel(unsigned char r, unsigned char g, unsigned char b)
{
	WS2812_Send_Byte(g);
	WS2812_Send_Byte(r);
	WS2812_Send_Byte(b);
}

/* ------------------------------------------------------------
 * WS2812_Show : 4개(U1,U3,U4,U5) LED 전체 갱신
 *   color[0] = U1(맨앞) ... color[3] = U5(맨끝)
 *   color[n][0]=R, color[n][1]=G, color[n][2]=B
 * ------------------------------------------------------------ */
void WS2812_Show(unsigned char color[WS2812_NUM_OF_LED][3])
{
	int led;

	__disable_irq();			// 타이밍이 민감하므로 전송 중 인터럽트 차단

	for(led = 0; led < WS2812_NUM_OF_LED; led++)
	{
		WS2812_Send_Pixel(color[led][0], color[led][1], color[led][2]);
	}

	__enable_irq();

	WS2812_PIN_LOW();
	WS2812_Delay_Cycle(WS2812_RES_CYCLE);		// RESET(Latch) code
}

/* 4개를 전부 동일한 색으로 켜는 헬퍼 함수 */
void WS2812_Show_All_Same_Color(unsigned char r, unsigned char g, unsigned char b)
{
	unsigned char color[WS2812_NUM_OF_LED][3];
	int led;

	for(led = 0; led < WS2812_NUM_OF_LED; led++)
	{
		color[led][0] = r;
		color[led][1] = g;
		color[led][2] = b;
	}

	WS2812_Show(color);
}
