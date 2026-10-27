# 🏁 F1 Start Light — STM32 반응속도 게임

> F1 레이스의 스타트 신호를 모사한 **반응속도 게임**입니다. LED가 하나씩 켜지는 카운트다운 후 **모두 꺼지는 순간(GO!)** 부터 버튼을 누를 때까지의 시간을 측정해 LED 막대그래프와 UART로 보여줍니다.
> *AI 시스템반도체 SW개발자 2기 (대한상공회의소) · Cortex-M4 파이널 프로젝트*

## 🎮 게임 규칙

1. `KEY0`을 누르면 라운드 시작
2. LED0~4가 0.5초 간격으로 순차 점등 (카운트다운)
3. **1.0 ~ 2.5초 랜덤 대기** 후 전체 소등 + 짧은 비프음 = **GO!**
4. GO 직후 `KEY0`을 누르면 반응시간(ms)을 측정해 UART 출력, 8개 LED 막대로 표시 (50ms 단위 1레벨, 빠를수록 많이 점등)
5. 최고 기록 갱신 시 축하 비프음 두 번
6. 카운트다운/랜덤 대기 중 버튼을 누르면 **False Start** (경고음 + LED 점멸)

## ⚙️ 구현 포인트

| 항목 | 구현 |
|---|---|
| MCU | STM32F411RE (Cortex-M4F, 96MHz), 레지스터 직접 제어 |
| 입력 | KEY0 **EXTI 인터럽트** → `volatile` 플래그로 메인과 공유 |
| 반응시간 측정 | TIM2 µs 스톱워치 (`TIM2_Stopwatch_Start/Stop`) |
| 난수 | TIM4를 항상 자유 회전시키고 버튼 입력 순간의 `CNT` 값을 시드로 사용 (TIM2와 분리해 간섭 방지) |
| LED 8개 | **SPI1** 으로 SC16IS752 GPIO 확장 → Active-Low 반전 처리를 `LED_Show(on_mask)`로 추상화 |
| 부저 | TIM3 PWM 주파수 생성 (GO 2kHz, 신기록 3kHz/3.5kHz, False Start 300Hz) |
| 출력 | UART2 115200, `printf` |
| 기타 드라이버 | I2C, ADC, SysTick 드라이버 포함 |

## 📁 구조

| 파일 | 역할 |
|---|---|
| `main.c` | 게임 로직 (`Play_Round`, `Show_False_Start`, `Show_Reaction_Bar`) |
| `timer.c` | TIM2(딜레이/스톱워치), TIM3(부저 PWM) |
| `spi.c`, `i2c.c` | SC16IS752 LED 확장 드라이버 |
| `key.c`, `exception.c` | 버튼 EXTI 및 핸들러 |
| `uart.c`, `clock.c`, `adc.c`, `systick.c`, `led.c` | 주변장치 |
| `crt0.s`, `rom_0x08000000.lds`, CMSIS 헤더 | 시작 코드/링커/헤더 |

## 🚀 빌드

```bash
make        # arm-none-eabi-gcc (Makefile의 TOOL_DIR을 본인 환경에 맞게 수정)
make run    # STM32CubeProgrammer 다운로드
```

UART 터미널(115200)에서 `[Round N] Press KEY0 to start...` 메시지가 보이면 정상입니다.

## 📚 배운 점

- 인터럽트 플래그(`volatile`) 패턴과 타이머 역할 분리
- 하드웨어 난수원이 없는 MCU에서 사용자 입력 타이밍을 엔트로피로 쓰는 방법
- SPI 기반 GPIO 확장기 제어와 Active-Low 신호 처리
