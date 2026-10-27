#include "device_driver.h"
#include <stdio.h>

/*=====================================================================
  F1 Start Light 반응속도 게임
  - LED0~4 순차 점등(카운트다운) -> 랜덤 대기 -> 전체 소등(GO!)
  - GO 신호부터 KEY0을 누를 때까지의 시간을 TIM2 스톱워치로 측정
  - 결과를 LED 막대그래프(8개)로 표시 + UART로 출력, 최고기록 갱신
  - 랜덤 대기 중 KEY0을 누르면 False Start 처리
=====================================================================*/

static void Sys_Init(int baud);
static void LED_Show(unsigned char on_mask);
static void Random_Init(void);
static unsigned int Random_Wait_Ms(void);
static void Play_Round(void);
static void Show_False_Start(void);
static void Show_Reaction_Bar(unsigned int ms);

/* EXTI15_10 ISR과 Main이 공유하는 플래그 (12과에서 배운 volatile+extern 패턴) */
volatile int Key_Pressed = 0;

unsigned int best_time = 0xFFFFFFFF;   /* 아직 기록 없음 */
unsigned int round_no = 0;

void Main(void)
{
	Sys_Init(115200);
	printf("\n====================================\n");
	printf(" F1 Start Light - Reaction Game\n");
	printf("====================================\n");

	/* LED(8bit, SPI1 -> SC16IS752) 초기화 */
	SPI1_SC16IS752_Init(32);                /* 96MHz/32 = 3MHz */
	SPI1_SC16IS752_Config_GPIO(0xFF);        /* 8핀 전부 Output */
	LED_Show(0x00);                            /* 시작 시 전부 OFF */

	/* 부저(TIM3, PB0) 초기화 */
	TIM3_Out_Init();

	/* KEY0(PC13) 인터럽트 활성화 */
	Key_ISR_Enable(1);
	Key_Pressed = 0;

	Random_Init();   /* 난수용 TIM4를 처음 한 번만 켜서 계속 자유회전시킴 */

	for(;;)
	{
		printf("\n[Round %u] Press KEY0 to start...\n", round_no + 1);

		Key_Pressed = 0;
		while(!Key_Pressed);   /* KEY0이 눌릴 때까지 대기 (인터럽트로 세팅됨) */
		Key_Pressed = 0;

		round_no++;
		Play_Round();

		TIM2_Delay(1500);        /* 결과 확인 시간 */
		LED_Show(0x00);
	}
}

static void Sys_Init(int baud)
{
	SCB->CPACR |= (0x3 << 10*2)|(0x3 << 11*2);
	Clock_Init();
	Uart2_Init(baud);
	setvbuf(stdout, NULL, _IONBF, 0);
	LED_Init();
}

/* on_mask: 켜고 싶은 LED에 대응하는 비트를 1로 넣으면 됨(직관적인 인터페이스)
   SC16IS752 GPIO는 Active Low이므로, 실제 전송 시에는 반전(~)해서 보낸다 */
static void LED_Show(unsigned char on_mask)
{
	unsigned int off_mask = (unsigned int)((~(unsigned int)on_mask) & 0xffu);
	SPI1_SC16IS752_Write_GPIO(off_mask);
}

/* TIM4를 프로그램 시작 후 계속 자유회전시켜두고(다른 곳에서 건드리지 않음),
   그 순간의 CNT값을 시드로 활용한다. TIM2(딜레이/스톱워치)와는 완전히 분리되어
   서로 간섭하지 않으므로, 사람이 버튼을 누르는 미세한 타이밍마다 실제로 값이 달라진다. */
static void Random_Init(void)
{
	Macro_Set_Bit(RCC->APB1ENR, 2);
	TIM4->CR1 = (1<<4);            /* Down, Repeat(OPM=0) - 절대 멈추지 않음 */
	TIM4->PSC = 0;                    /* 분주 없이 최대한 빠르게 */
	TIM4->ARR = 0xFFFF;
	Macro_Set_Bit(TIM4->EGR, 0);
	Macro_Set_Bit(TIM4->CR1, 0);      /* 시작 후 다시는 Stop 하지 않음 */
}

static unsigned int Random_Wait_Ms(void)
{
	unsigned int seed = TIM4->CNT;      /* "지금 이 순간"의 자유회전 값 */

	return (seed % 1500) + 1000;          /* 1.0 ~ 2.5초 */
}

static void Play_Round(void)
{
	int i;
	unsigned char pattern = 0x00;
	unsigned int wait_ms;
	unsigned int reaction_ms;

	/* ① 카운트다운: LED0~4를 순서대로 점등 */
	for(i = 0; i < 5; i++)
	{
		pattern |= (1 << i);
		LED_Show(pattern);
		TIM2_Delay(500);

		if(Key_Pressed)          /* 카운트다운 중 눌렀어도 False Start */
		{
			Show_False_Start();
			return;
		}
	}

	/* ② 랜덤 대기 (긴장 구간) - 이 동안 눌리면 False Start */
	wait_ms = Random_Wait_Ms();
	Key_Pressed = 0;
	TIM2_Delay(wait_ms);

	if(Key_Pressed)
	{
		Show_False_Start();
		return;
	}

	/* ③ GO! 신호 - 전부 소등 + 짧은 삐 소리 */
	LED_Show(0x00);
	TIM3_Out_Freq_Generation(2000);
	TIM2_Delay(80);
	TIM3_Out_Stop();

	/* ④ 반응시간 측정 시작 */
	Key_Pressed = 0;
	TIM2_Stopwatch_Start();

	while(!Key_Pressed);          /* KEY0 눌릴 때까지 대기 */

	reaction_ms = TIM2_Stopwatch_Stop() / 1000;   /* usec -> msec */
	Key_Pressed = 0;

	/* ⑤ 결과 출력 */
	printf("Reaction Time: %u ms\n", reaction_ms);
	Show_Reaction_Bar(reaction_ms);

	if(reaction_ms < best_time)
	{
		best_time = reaction_ms;
		printf(">> New Best Record: %u ms !!\n", best_time);

		/* 신기록 축하 비프 (높은음 두 번) */
		TIM3_Out_Freq_Generation(3000);
		TIM2_Delay(100);
		TIM3_Out_Stop();
		TIM2_Delay(80);
		TIM3_Out_Freq_Generation(3500);
		TIM2_Delay(100);
		TIM3_Out_Stop();
	}
	else
	{
		printf("   (Best: %u ms)\n", best_time);
	}
}

static void Show_False_Start(void)
{
	int i;

	printf("False Start!! Wait for the LEDs to go OFF next time.\n");

	TIM3_Out_Freq_Generation(300);   /* 낮은 경고음 */

	for(i = 0; i < 3; i++)
	{
		LED_Show(0xFF);
		TIM2_Delay(150);
		LED_Show(0x00);
		TIM2_Delay(150);
	}

	TIM3_Out_Stop();
	Key_Pressed = 0;
}

/* 반응이 빠를수록 더 많은 LED가 켜지는 "점수 막대"로 표시
   기준: 50ms 단위 1레벨, 최대 8레벨(=0ms~50ms대가 최고점) */
static void Show_Reaction_Bar(unsigned int ms)
{
	int level;
	unsigned char pattern;

	level = 8 - (int)(ms / 50);
	if(level < 0) level = 0;
	if(level > 8) level = 8;

	pattern = (level >= 8) ? 0xFF : (unsigned char)((1u << level) - 1u);
	LED_Show(pattern);
}

/*=====================================================================
  EXTI9_5 인터럽트 서비스 루틴
  key.c의 Key_ISR_Enable()이 PB8(EXTI8)을 이 그룹(NVIC 23번)으로
  설정해두었으므로, 실제 처리 함수는 이 이름으로 반드시 존재해야 한다.
=====================================================================*/
void EXTI9_5_IRQHandler(void)
{
	EXTI->PR = (0x1 << 8);   /* Pending Clear: 반드시 대입연산(1을 씀), PB8이니 8번 비트 */

	Key_Pressed = 1;            /* Event Driven: 플래그만 세팅 */
}
#if 0
void EXTI15_10_IRQHandler(void)
{
	EXTI->PR = (0x1 << 13);   /* Pending Clear: 반드시 대입연산(1을 씀) */

	Key_Pressed = 1;            /* Event Driven: 플래그만 세팅 */
}
#endif