# AI 음성인식 기반 비접촉 조명 제어

**8비트 MCU의 실시간 DSP · 정수 추론 · SPI/UART 장애 대응**

음성으로 조명의 색조와 밝기를 조절하고, 사용자가 가까이 있을 때만 명령을 반영하는 시제품입니다. 두 대의 Arduino UNO에서 신호 처리와 추론·출력을 나누고, 통신 경로가 바뀌어도 같은 특징 패킷을 사용하도록 구성했습니다.

![프로젝트 시연 미리보기](assets/demo-preview.gif)

[시연영상](media/demo.mp4) · [시연 구간 안내](docs/demo.md) · [시스템 구조](docs/architecture.md) · [개발 과정](docs/development.md) · [결과와 시험 조건](docs/results.md) · [코드 안내](docs/code-map.md) · [실행 준비](docs/reproduction.md)

| 구분 | 내용 |
|---|---|
| 과목 | 임베디드시스템설계 프로젝트1 · 2025학년도 2학기 |
| 하드웨어 | Arduino UNO/ATmega328P × 2, MAX4466 마이크, HC-SR04, RGB LED |
| A보드 | 8 kHz ADC, 160샘플 이중 버퍼, 정수 DSP, 특징 패킷 전송 |
| B보드 | 3프레임 특징 누적, 정수 선형 분류, 후처리, 거리 조건, RGB PWM |
| 인터페이스 | SPI 주 링크, UART 폴백, 동일 패킷·체크섬 |

## 음성 입력에서 실제 출력까지

```text
마이크 → A보드 ADC/이중 버퍼 → RMS·Vpp·ZCR·Goertzel 특징
                                  │ SPI / UART
                         B보드 정수 분류·후처리
                                  │ 초음파 근접 조건
                       RGB 색조 / 4단계 밝기 제어
```

Warm·Cool은 RGB 혼합비를 바꾸고, Low·High는 밝기를 한 단계씩 조절합니다. Silence는 명령으로 실행하지 않습니다. 수술실 조명의 접촉 조작을 줄이는 문제에서 출발했으며, 시연 대상은 RGB LED를 사용하는 교육용 시제품입니다.

## 구현의 핵심

### 샘플 취득과 계산을 분리

125 µs마다 샘플을 확보하고 160개를 모아 20 ms 프레임으로 처리합니다. ADC ISR은 샘플 저장과 버퍼 교대를 수행하고, 메인 루프에서 필터·특징 계산을 진행해 다음 프레임 취득과 겹치도록 했습니다.

### 작은 MCU에 맞춘 정수 연산

RMS·Vpp·ZCR와 다섯 Goertzel 주파수 특징을 계산하고, 최근 세 프레임을 24차원 입력으로 구성했습니다. 학습은 PC에서 수행하고 MCU에서는 정수 표준화와 가중치·편향으로 클래스 점수를 계산합니다.

### 링크 변경과 실제 출력 조건을 함께 설계

13바이트 패킷의 길이·체크섬을 확인하고, SPI 실패가 연속되면 UART로 전환합니다. 원시 분류에는 무음 gate·투표·연속 확인·불응기를 적용하고, Timer1 입력 캡처로 측정한 근접 조건을 통과할 때 RGB PWM을 갱신합니다.

## 확인한 결과

보고서에는 LED 색조·밝기 제어 시연과 의도적인 통신선 단선/복구 시험이 기록되어 있습니다. A의 프레임 처리와 B의 추론 시간은 오실로스코프 기반 구간 측정값으로 보고되며, 시스템 전체의 명령 반영 지연과 구분합니다. [시험 조건과 결과](docs/results.md)에 정리했습니다.

## 코드를 읽는 순서

1. [A보드](firmware/A_wclh_5/A_wclh_5.ino)의 ADC ISR, 이중 버퍼, 특징 계산과 전송을 봅니다.
2. [B보드](firmware/B_wclh_5/B_wclh_5.ino)의 패킷 수신, 정수 추론, 후처리와 Timer1 입력 캡처를 봅니다.
3. [학습·헤더 내보내기](tools/prep_train_export_wclh_k5.py)에서 특징 구성과 양자화 과정을 확인합니다.

## 코드와 개발 환경

초음파 Timer1 입력 캡처가 포함된 A/B sketch, 정수 모델 헤더, PC 특징 수집·학습 도구를 함께 제공합니다. 파일별 원본과 입력 형식은 [코드 안내](docs/code-map.md), 보드·배선·PC/MCU 대조 조건은 [실행 준비](docs/reproduction.md)에 정리했습니다.

---

[임베디드2: 음원 방향·수어 시스템](https://github.com/jounget5411-lab/embedded-sensor-fusion-portfolio) · [임베디드3: 사진 판별·선별 저장](https://github.com/jounget5411-lab/embedded-intrusion-detection-portfolio)
