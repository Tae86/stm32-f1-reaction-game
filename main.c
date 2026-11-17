#include "device_driver.h"
#include <stdio.h>

/*=====================================================================
  F1 Start Light 반응속도 게임 (RGB LED 버전) - Non-Blocking 구조

  [구조]
  - TIM2_Delay() / while(!Key_Pressed) 같은 "멈춰서 기다리는" 코드 없음.
  - SysTick 1ms 인터럽트로 g_tick_ms(시스템 시간)를 만들고,
    게임 흐름을 State Machine으로 구성 -> Main의 for(;;)는 절대 멈추지 않음.
  - "기다린다" = "지금 시간이 목표 시간을 지났는지 매 루프마다 확인한다".
    (11과 Repeat Mode의 Check_Timeout 패턴을 게임 전체에 확장 적용한 것)

  [게임 규칙]
  - RGB LED 3개(U1,U3,U4)를 순차적으로 Red 점등(카운트다운)
    -> 랜덤 대기(1.0~2.5초) -> 3개 전부 Green으로 전환(GO!)
  - GO 신호부터 KEY0을 누를 때까지의 시간을 TIM2 스톱워치로 측정
  - 결과는 SPI(SC16IS752) 8개 LED 막대그래프 + UART로 출력, 최고기록 갱신
  - 카운트다운/랜덤 대기 중 KEY0을 누르면 False Start

  [참고] 부저(TIM3, PB0)는 사용하지 않음 -> PB0 핀이 비어 있음.
         (WS2812 DIN이 실제로 PB0에 배선되어 있다면 ws2812.c의
          WS2812_PIN을 1 -> 0으로 되돌려도 이제 충돌하지 않음)
=====================================================================*/

/* ---------------- 시간 상수 (ms) ---------------- */
#define COUNTDOWN_STEP_MS   1000    /* 빨간 LED 하나씩 켜지는 간격 */
#define RESULT_HOLD_MS      1500    /* 결과 확인 시간 */
#define FALSE_BLINK_MS       150    /* False Start 점멸 간격 */
#define KEY_DEBOUNCE_MS       20    /* 채터링 진정 시간 */
#define HEARTBEAT_MS         250    /* 보드 LED(PA5) 깜빡임 - 멈추지 않음을 눈으로 확인 */

/* RGB LED(WS2812B) 밝기, 카운트다운에 사용할 개수(U1,U3,U4 3개. U5는 미사용) */
#define RGB_BRIGHTNESS      80
#define RGB_COUNTDOWN_LEDS  3

/* ---------------- 게임 상태 ---------------- */
typedef enum
{
	ST_IDLE_PRINT,      /* 라운드 안내 출력 */
	ST_IDLE_WAIT,       /* 시작 버튼 대기 */
	ST_COUNTDOWN,       /* Red LED 순차 점등 */
	ST_RANDOM_WAIT,     /* 랜덤 대기(긴장 구간) */
	ST_MEASURE,         /* Green 점등 후 반응시간 측정 중 */
	ST_FALSE_START,     /* False Start 점멸 중 */
	ST_HOLD             /* 결과 확인 시간 */
} game_state_t;

static void Sys_Init(int baud);
static void SysTick_1ms_Init(void);
static void LED_Show(unsigned char on_mask);
static void WS2812_Countdown_Red(int count);
static void WS2812_Go_Green(int count);
static void Random_Init(void);
static unsigned int Random_Wait_Ms(void);
static void Show_Reaction_Bar(unsigned int ms);
static void Key_Arm_Now(void);
static void Key_Service(void);
static int  Key_Take(void);
static void Enter_False_Start(void);
static void Game_Step(void);
static void Heartbeat_Step(void);

/* EXTI ISR과 Main이 공유하는 플래그 (12과에서 배운 volatile+extern 패턴) */
volatile int Key_Pressed = 0;

/* SysTick ISR이 1ms마다 증가시키는 시스템 시간 (ISR이 바꾸므로 volatile 필수) */
static volatile unsigned int g_tick_ms = 0;

unsigned int best_time = 0xFFFFFFFF;   /* 아직 기록 없음 */
unsigned int round_no = 0;

/* ---------------- 상태 머신용 변수 ---------------- */
static game_state_t state = ST_IDLE_PRINT;
static unsigned int phase_ms = 0;      /* 현재 단계에 진입한 시각 */
static int step = 0;                     /* 단계 안에서의 진행 번호 */
static unsigned int wait_ms = 0;         /* 랜덤 대기 시간 */

/* 채터링 대응 : 재무장 예약 */
static int key_rearm_pending = 0;
static unsigned int key_rearm_ms = 0;

/* 경과 시간 계산 (unsigned 뺄셈이라 오버플로우가 나도 안전) */
#define ELAPSED(since)   ((unsigned int)(g_tick_ms - (since)))

/*=====================================================================
  Main : 절대 멈추지 않는 루프.
  매 바퀴마다 "할 일이 생겼는지"만 확인하고 바로 다음으로 넘어간다.
=====================================================================*/
void Main(void)
{
	Sys_Init(115200);
	printf("\n====================================\n");
	printf(" F1 Start Light - Reaction Game\n");
	printf("====================================\n");

	/* LED(8bit, SPI1 -> SC16IS752) : 결과 막대그래프 전용 */
	SPI1_SC16IS752_Init(32);                /* 96MHz/32 = 3MHz */
	SPI1_SC16IS752_Config_GPIO(0xFF);        /* 8핀 전부 Output */
	LED_Show(0x00);                            /* 시작 시 전부 OFF */

	/* RGB LED(WS2812B, PB1) : 카운트다운 + GO 신호 전용 */
	WS2812_Init();
	WS2812_Countdown_Red(0);                 /* 시작 시 전부 OFF */

	/* KEY0(PB8) 인터럽트 활성화 */
	Key_ISR_Enable(1);
	Key_Arm_Now();

	Random_Init();          /* 난수용 TIM4를 처음 한 번만 켜서 계속 자유회전시킴 */
	SysTick_1ms_Init();      /* 시스템 시간축 시작 */

	for(;;)
	{
		Key_Service();       /* 채터링 재무장 처리 (논블로킹) */
		Game_Step();          /* 게임 상태 머신 1스텝 */
		Heartbeat_Step();     /* 보드 LED 깜빡임 - 루프가 살아있다는 증거 */

		/* 여기에 다른 작업을 자유롭게 추가해도 게임 진행이 멈추지 않는다.
		   (예: ADC 폴링, UART 수신 처리 등) */
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

/* SysTick을 1ms 주기 인터럽트로 설정 (Cortex-M 코어 내장, 11과)
   LOAD = HCLK/1000 - 1, CLKSOURCE=HCLK, TICKINT=1(인터럽트 허용), ENABLE=1 */
static void SysTick_1ms_Init(void)
{
	SysTick->LOAD = (HCLK / 1000) - 1;
	SysTick->VAL  = 0;
	SysTick->CTRL = (1<<2)|(1<<1)|(1<<0);
}

/*=====================================================================
  게임 상태 머신 : 각 case는 "조건이 충족됐는지"만 확인하고 즉시 return.
  절대 그 안에서 기다리지 않는다.
=====================================================================*/
static void Game_Step(void)
{
	unsigned int reaction_ms;

	switch(state)
	{
	/* --------- 라운드 안내 출력 --------- */
	case ST_IDLE_PRINT:
		printf("\n[Round %u] Press KEY0 to start...\n", round_no + 1);
		Key_Arm_Now();
		state = ST_IDLE_WAIT;
		break;

	/* --------- 시작 버튼 대기 --------- */
	case ST_IDLE_WAIT:
		if(Key_Take())
		{
			round_no++;
			step = 0;
			WS2812_Countdown_Red(1);       /* 첫 번째 Red 점등 */
			phase_ms = g_tick_ms;
			state = ST_COUNTDOWN;
		}
		break;

	/* --------- 카운트다운 : Red를 하나씩 추가 점등 --------- */
	case ST_COUNTDOWN:
		if(Key_Take())                      /* 카운트다운 중 눌렀으면 False Start */
		{
			Enter_False_Start();
			break;
		}

		if(ELAPSED(phase_ms) >= COUNTDOWN_STEP_MS)
		{
			phase_ms = g_tick_ms;
			step++;

			if(step < RGB_COUNTDOWN_LEDS)
			{
				WS2812_Countdown_Red(step + 1);
			}
			else
			{
				wait_ms = Random_Wait_Ms();   /* 1.0 ~ 2.5초 */
				state = ST_RANDOM_WAIT;
			}
		}
		break;

	/* --------- 랜덤 대기 (긴장 구간) --------- */
	case ST_RANDOM_WAIT:
		if(Key_Take())                      /* 이 구간에 누르면 False Start */
		{
			Enter_False_Start();
			break;
		}

		if(ELAPSED(phase_ms) >= wait_ms)
		{
			/* GO! : Red 3개 -> Green 3개로 전환 */
			WS2812_Go_Green(RGB_COUNTDOWN_LEDS);

			Key_Arm_Now();                   /* 지금부터 깨끗한 상태로 입력을 듣는다 */
			TIM2_Stopwatch_Start();           /* Green이 켜지는 바로 그 순간부터 측정 */

			phase_ms = g_tick_ms;
			state = ST_MEASURE;
		}
		break;

	/* --------- 반응시간 측정 중 --------- */
	case ST_MEASURE:
		if(Key_Take())
		{
			reaction_ms = TIM2_Stopwatch_Stop() / 1000;   /* usec -> msec */

			printf("Reaction Time: %u ms\n", reaction_ms);
			Show_Reaction_Bar(reaction_ms);   /* SPI 8-LED 막대그래프 */
			WS2812_Countdown_Red(0);           /* RGB는 Off */

			if(reaction_ms < best_time)
			{
				best_time = reaction_ms;
				printf(">> New Best Record: %u ms !!\n", best_time);
			}
			else
			{
				printf("   (Best: %u ms)\n", best_time);
			}

			phase_ms = g_tick_ms;
			state = ST_HOLD;
		}
		break;

	/* --------- False Start : SPI LED 3회 점멸 --------- */
	case ST_FALSE_START:
		if(ELAPSED(phase_ms) >= FALSE_BLINK_MS)
		{
			phase_ms = g_tick_ms;
			step++;

			if(step >= 6)                       /* on/off 3회 = 6단계 */
			{
				LED_Show(0x00);
				state = ST_HOLD;
			}
			else
			{
				LED_Show((step & 0x1) ? 0x00 : 0xFF);
			}
		}
		break;

	/* --------- 결과 확인 시간 --------- */
	case ST_HOLD:
		if(ELAPSED(phase_ms) >= RESULT_HOLD_MS)
		{
			LED_Show(0x00);
			WS2812_Countdown_Red(0);
			state = ST_IDLE_PRINT;
		}
		break;

	default:
		state = ST_IDLE_PRINT;
		break;
	}
}

/* False Start 진입 처리 */
static void Enter_False_Start(void)
{
	printf("False Start!! Wait for the LEDs to go OFF next time.\n");

	WS2812_Countdown_Red(0);          /* 진행 중이던 RGB Red 표시 정리 */
	LED_Show(0xFF);                     /* 점멸 시작 */

	step = 0;
	phase_ms = g_tick_ms;
	state = ST_FALSE_START;
}

/*=====================================================================
  KEY 처리 (채터링 대응) - 기다리지 않는 방식

  ISR    : 눌림 감지 즉시 EXTI를 마스킹하고 플래그만 세팅
  Take   : Main이 눌림을 "소비"하면서 재무장을 예약(20ms 뒤)
  Service: 매 루프마다 예약 시간이 됐는지 확인 후 재무장
=====================================================================*/
static void Key_Arm_Now(void)
{
	Key_Pressed = 0;
	key_rearm_pending = 0;
	EXTI->PR = (0x1 << 8);              /* 쌓여있던 Pending 정리 */
	Macro_Set_Bit(EXTI->IMR, 8);         /* 인터럽트 허용 */
}

static int Key_Take(void)
{
	if(!Key_Pressed) return 0;

	Key_Pressed = 0;
	key_rearm_ms = g_tick_ms;
	key_rearm_pending = 1;               /* 20ms 뒤에 재무장 예약 */
	return 1;
}

static void Key_Service(void)
{
	if(key_rearm_pending && (ELAPSED(key_rearm_ms) >= KEY_DEBOUNCE_MS))
	{
		EXTI->PR = (0x1 << 8);           /* 채터링으로 쌓인 Pending 정리 */
		Macro_Set_Bit(EXTI->IMR, 8);      /* 인터럽트 재허용 */
		key_rearm_pending = 0;
	}
}

/* 보드 내장 LED(PA5)를 250ms마다 토글 : Main 루프가 멈추지 않았다는 시각적 증거 */
static void Heartbeat_Step(void)
{
	static unsigned int last_ms = 0;
	static int on = 0;

	if(ELAPSED(last_ms) >= HEARTBEAT_MS)
	{
		last_ms = g_tick_ms;
		on ^= 1;

		if(on) LED_On();
		else   LED_Off();
	}
}

/* on_mask: 켜고 싶은 LED에 대응하는 비트를 1로 넣으면 됨
   SC16IS752 GPIO는 Active Low이므로, 실제 전송 시에는 반전(~)해서 보낸다 */
static void LED_Show(unsigned char on_mask)
{
	unsigned int off_mask = (unsigned int)((~(unsigned int)on_mask) & 0xffu);
	SPI1_SC16IS752_Write_GPIO(off_mask);
}

/* count(0~3)개를 Red로 켜고 나머지는 끔 - 카운트다운용 */
static void WS2812_Countdown_Red(int count)
{
	unsigned char color[4][3] = { {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0} };
	int i;

	for(i = 0; i < count && i < 4; i++)
	{
		color[i][0] = RGB_BRIGHTNESS;   /* R */
	}

	WS2812_Show(color);
}

/* count(0~3)개를 Green으로 켜고 나머지는 끔 - GO! 신호용 */
static void WS2812_Go_Green(int count)
{
	unsigned char color[4][3] = { {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0} };
	int i;

	for(i = 0; i < count && i < 4; i++)
	{
		color[i][1] = RGB_BRIGHTNESS;   /* G */
	}

	WS2812_Show(color);
}

/* TIM4를 프로그램 시작 후 계속 자유회전시켜두고(다른 곳에서 건드리지 않음),
   그 순간의 CNT값을 시드로 활용한다. TIM2(스톱워치)와는 완전히 분리되어
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
  SysTick 인터럽트 : 1ms마다 시스템 시간을 1 증가시키는 일만 한다.
  (Event Driven 패러다임 - ISR은 최소한의 일만)
=====================================================================*/
void SysTick_Handler(void)
{
	g_tick_ms++;
}

/*=====================================================================
  EXTI9_5 인터럽트 서비스 루틴
  key.c의 Key_ISR_Enable()이 PB8(EXTI8)을 이 그룹(NVIC 23번)으로
  설정해두었으므로, 실제 처리 함수는 이 이름으로 반드시 존재해야 한다.
=====================================================================*/
void EXTI9_5_IRQHandler(void)
{
	EXTI->PR = (0x1 << 8);              /* Pending Clear: 반드시 대입연산(1을 씀) */
	Macro_Clear_Bit(EXTI->IMR, 8);       /* 재무장 전까지 인터럽트 차단 - 채터링 방지 */

	Key_Pressed = 1;                      /* Event Driven: 플래그만 세팅 */
}
