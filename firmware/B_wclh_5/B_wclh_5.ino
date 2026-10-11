// ========================= B (SLAVE: SPI primary, UART0 fallback) + Ultrasonic Gate =========================
// AVR: ATmega328P (Arduino UNO)
#include <string.h>
#include <Arduino.h>
#include <limits.h>
#include <avr/interrupt.h>
#include "weights_wclh_k5.h"

// ====== Inference/Voting Params ======
#define T_silence 10
#define VOTE_N 5
#define TON 2
#define STX 0xAA
#define ETX 0x55
#define HOLD_MS 1000
#define N_FEATURES_PER_FRAME 8
#define COOL_BLOCK_EN 1
#define RMS_COOL_BLOCK 105

// ====== 3-Color LED (PWM; 레지스터 제어) ======
// D5=PD5=OC0B, D6=PD6=OC0A, D3=PD3=OC2B  (※ D9(OC1A) 대신 D3 사용: Timer1은 초음파 ICP 전용)
#define PIN_R_BIT _BV(PD5)
#define PIN_G_BIT _BV(PD6)
#define PIN_B_BIT _BV(PD3)

// COMMON_ANODE=1 이면 듀티 반전
#define COMMON_ANODE 1

static inline void pwm_init(){
  // --- DDR 설정 ---
  DDRD |= PIN_R_BIT | PIN_G_BIT | PIN_B_BIT;   // D5,D6,D3 출력

  // --- Timer0 (D5/D6) Fast PWM, non-inverting, prescale /64 (Arduino 기본과 동일) ---
  // WGM01:0=3(=Fast PWM 8-bit), WGM02=0
  TCCR0A = _BV(WGM00) | _BV(WGM01) | _BV(COM0A1) | _BV(COM0B1);  // OC0A(6), OC0B(5) enable non-inverting
  TCCR0B = (TCCR0B & ~(_BV(CS02)|_BV(CS01)|_BV(CS00))) | _BV(CS01) | _BV(CS00); // /64

  // --- Timer2 (D3) Fast PWM, non-inverting, prescale /64 ---
  TCCR2A = _BV(WGM20) | _BV(WGM21) | _BV(COM2B1);  // OC2B(3) enable
  TCCR2B = (TCCR2B & ~(_BV(CS22)|_BV(CS21)|_BV(CS20))) | _BV(CS22); // /64
}

static inline void pwm_write_r(uint8_t v){ OCR0B = COMMON_ANODE ? (255 - v) : v; } // D5
static inline void pwm_write_g(uint8_t v){ OCR0A = COMMON_ANODE ? (255 - v) : v; } // D6
static inline void pwm_write_b(uint8_t v){ OCR2B = COMMON_ANODE ? (255 - v) : v; } // D3

// 톤 프리셋 (0~255)
static const uint8_t WARM_RGB[3] = {255,  80,  10};
static const uint8_t COOL_RGB[3] = { 20, 170, 255};

static inline uint8_t scale_u8(uint8_t base, uint8_t step){
  uint16_t v = (uint16_t)base * step; v /= 3;
  return (v>255)?255:(uint8_t)v;
}

enum Tone { TONE_WARM = 0, TONE_COOL = 1 };
static Tone     g_tone  = TONE_WARM;
static uint8_t  g_level = 1; // 0~3

static inline void apply_led_state(){
  const uint8_t* rgb = (g_tone==TONE_WARM)? WARM_RGB : COOL_RGB;
  pwm_write_r(scale_u8(rgb[0], g_level));
  pwm_write_g(scale_u8(rgb[1], g_level));
  pwm_write_b(scale_u8(rgb[2], g_level));
}

// ====== UART0 (HW) — RX 전용 폴백 ======
#define UART_BAUD 57600
static inline void uart0_init_rx_57600(){
  // 16MHz / (16*(UBRR+1)) ≈ 57600 → UBRR≈34
  UBRR0H = 0; UBRR0L = 34;
  UCSR0A = 0;              // U2X0=0
  UCSR0B = _BV(RXEN0);     // 수신만
  UCSR0C = _BV(UCSZ01)|_BV(UCSZ00); // 8N1
}
static inline int uart0_read_byte_nonblock(){
  if (UCSR0A & _BV(RXC0)) return UDR0; // 0..255
  return -1;
}

// ====== Ultrasonic (HC-SR04) — Timer1 입력캡처(ICP1=D8, TRIG=D7) ======
#define ULTRA_TRIG_BIT   _BV(PD7)   // D7
#define ULTRA_THRESH_CM  8
#define ULTRA_READ_INTERVAL_MS 60
#define ULTRA_TIMEOUT_TICKS  (uint16_t)60000  // 약 30ms@prescale=8, tick=0.5us

static uint16_t  g_last_distance_cm = 65535;
static uint32_t  g_last_ultra_ms = 0;

static inline void ultra_hw_init(){
  // TRIG(D7) 출력
  DDRD  |= ULTRA_TRIG_BIT;
  PORTD &= ~ULTRA_TRIG_BIT;

  // ICP1(D8=PB0) 입력(풀업 끔)
  DDRB  &= ~_BV(PB0);
  PORTB &= ~_BV(PB0);

  // Timer1: normal mode, prescale /8 (tick=0.5us), Input Capture 사용
  TCCR1A = 0;
  TCCR1B = _BV(CS11);  // /8
  TIFR1  = _BV(ICF1) | _BV(TOV1); // 플래그 클리어
}

// 초음파 1회 측정 (입력캡처)
static uint16_t ultrasonic_read_cm_once(){
  // 안정 트리거: HIGH 10us
  PORTD &= ~ULTRA_TRIG_BIT; delayMicroseconds(2);
  PORTD |=  ULTRA_TRIG_BIT; delayMicroseconds(10);
  PORTD &= ~ULTRA_TRIG_BIT;

  // 상승에지 캡처 대기
  TIFR1  = _BV(ICF1) | _BV(TOV1);       // 플래그 클리어
  TCCR1B = (TCCR1B & ~_BV(ICES1)) | _BV(CS11) | _BV(ICES1); // 상승에지
  uint16_t t_start=0, t_end=0;

  // 타임아웃 감시
  uint16_t guard = 0;
  while(!(TIFR1 & _BV(ICF1))){ if(++guard > ULTRA_TIMEOUT_TICKS) return 65535; }
  t_start = ICR1; TIFR1 = _BV(ICF1);

  // 하강에지 캡처 대기
  TCCR1B = (TCCR1B & ~_BV(ICES1)) | _BV(CS11); // 하강에지
  guard = 0;
  while(!(TIFR1 & _BV(ICF1))){ if(++guard > ULTRA_TIMEOUT_TICKS) return 65535; }
  t_end = ICR1; TIFR1 = _BV(ICF1);

  // 펄스폭 계산(0.5us tick)
  uint16_t dt = (uint16_t)(t_end - t_start);   // 오버플로우 범위 내 가정
  uint32_t us = (uint32_t)dt / 2;              // 0.5us*tick → us
  uint32_t cm = us / 58U;                      // HC-SR04 변환
  if (cm > 600) cm = 600;
  return (uint16_t)cm;
}

static inline uint16_t ultrasonic_get_distance_cm(){
  uint32_t now = millis();
  if (now - g_last_ultra_ms >= ULTRA_READ_INTERVAL_MS){
    g_last_distance_cm = ultrasonic_read_cm_once();
    g_last_ultra_ms = now;
  }
  return g_last_distance_cm;
}

// ====== Frame / Vote state ======
static uint8_t frame_buf[3][N_FEATURES_PER_FRAME];
static uint8_t frame_count=0, fb_idx=0;
static uint8_t current_features[N_FEATURES_PER_FRAME];

static uint8_t vote_ring[VOTE_N]; static uint8_t vote_idx=0, vote_filled=0;
static int current_decision=0; static uint8_t current_streak=0;

static bool     hold_active=false;
static uint32_t hold_started_ms=0;

// ====== Inference helpers ======
static inline void infer_scores_u8(const uint8_t* f, int32_t s[5]){
  for(int c=0;c<N_CLASS;c++){
    int32_t acc = B[c];
    for(int i=0;i<N_FEAT;i++){
      int32_t xi = (int32_t)f[i] - (int32_t)MEAN[i];
      xi = (xi * (int32_t)SCALE[i]) >> Sx;
      acc += (int32_t)W[c][i] * xi;
    }
    s[c] = acc;
  }
}
static inline int argmax5_except2(const int32_t s[5], bool ex2){
  int bi=-1; int32_t bv=INT32_MIN;
  for(int i=0;i<5;i++){ if(ex2 && i==2) continue; if(s[i]>bv){bv=s[i];bi=i;} }
  return (bi<0)?0:bi;
}
static inline int vote_class(int np, uint8_t rms){
  if(rms<=T_silence){ current_decision=0; current_streak=0; return current_decision; }
  vote_ring[vote_idx]=(uint8_t)np; vote_idx=(vote_idx+1)%VOTE_N;
  if(vote_filled<VOTE_N) vote_filled++; if(vote_filled<VOTE_N) return current_decision;
  uint8_t cnt[8]={0}; for(uint8_t i=0;i<VOTE_N;i++){ uint8_t c=vote_ring[i]; if(c<N_CLASS) cnt[c]++; }
  int maj=0; for(int c=1;c<N_CLASS;c++) if(cnt[c]>cnt[maj]) maj=c;
  if(maj!=current_decision){ current_streak++; if(current_streak>=TON){ current_decision=maj; current_streak=0; } }
  else current_streak=0;
  return current_decision;
}

// ====== SPI Slave (레지스터) ======
#define SPI_SYNC  0xF0
#define SPI_READY 0xA5
#define SPI_ACK   0x5A
#define N_PAYLOAD N_FEATURES_PER_FRAME

enum SpiRxState { S_IDLE, S_WAIT_DUMMY, S_WAIT_STX, S_WAIT_SEQ, S_WAIT_LEN, S_WAIT_PAYLOAD, S_WAIT_XOR, S_WAIT_ETX };
volatile SpiRxState spi_state=S_IDLE;

volatile uint8_t  spi_len=0, spi_xor=0, spi_buf[2+N_PAYLOAD+1], spi_idx=0;
volatile bool     spi_packet_ready=false;
volatile uint8_t  spi_payload[N_FEATURES_PER_FRAME];
volatile bool     spi_ack_armed=false;

ISR(SPI_STC_vect){
  uint8_t in=SPDR, out=SPI_READY;

  if(spi_state==S_IDLE){
    out = spi_ack_armed ? SPI_ACK : SPI_READY;
    SPDR = out; spi_ack_armed=false;
    if(in==SPI_SYNC){ spi_state=S_WAIT_DUMMY; spi_idx=0; spi_xor=0; spi_len=0; }
    return;
  }
  if(spi_state==S_WAIT_DUMMY){ spi_state=S_WAIT_STX; SPDR=SPI_READY; return; }

  switch(spi_state){
    case S_WAIT_STX:     spi_state = (in==STX)?S_WAIT_SEQ:S_IDLE; break;
    case S_WAIT_SEQ:     spi_buf[spi_idx++]=in; spi_xor^=in; spi_state=S_WAIT_LEN; break;
    case S_WAIT_LEN:     spi_buf[spi_idx++]=in; spi_len=in; spi_xor^=in; spi_state=(spi_len==N_PAYLOAD)?S_WAIT_PAYLOAD:S_IDLE; break;
    case S_WAIT_PAYLOAD: spi_buf[spi_idx++]=in; spi_xor^=in; if(spi_idx>=(2+spi_len)) spi_state=S_WAIT_XOR; break;
    case S_WAIT_XOR:     spi_buf[spi_idx++]=in; spi_state=S_WAIT_ETX; break;
    case S_WAIT_ETX:
      if(in==ETX){
        uint8_t rx_ck=spi_buf[2+spi_len];
        if(rx_ck==spi_xor){
          for(uint8_t i=0;i<spi_len;i++) spi_payload[i]=spi_buf[2+i];
          spi_packet_ready=true;
          spi_ack_armed=true; // 다음 바이트 ACK
        }
      }
      spi_state=S_IDLE; break;
  }
  SPDR=SPI_READY;
}

static inline void spi_slave_init(){
  // SS(D10=PB2), MOSI(D11=PB3), SCK(D13=PB5) 입력, MISO(D12=PB4) 출력
  DDRB &= ~(_BV(PB2)|_BV(PB3)|_BV(PB5));
  DDRB |=  _BV(PB4);
  PORTB |= _BV(PB2);  // SS 풀업
  // SPI enable + SPI interrupt enable (Slave)
  SPCR = _BV(SPE) | _BV(SPIE);
  SPSR = 0;
  SPDR = SPI_READY;
}

// ====== 공통 소비 파이프라인 + LED 구동 ======
static void consume_features_and_drive(const uint8_t* feat){
  memcpy(current_features,feat,N_FEATURES_PER_FRAME);
  memcpy(frame_buf[fb_idx],current_features,N_FEATURES_PER_FRAME);
  fb_idx=(fb_idx+1)%3; if(frame_count<3) frame_count++;

  int decision = 0;
  if(frame_count>=3){
    const int STRIDE=N_FEATURES_PER_FRAME; uint8_t flat[N_FEAT];
    uint8_t i_prev=(fb_idx+1)%3, i_curr=(fb_idx+2)%3, i_next=fb_idx;
    memcpy(&flat[0*STRIDE],frame_buf[i_prev],STRIDE);
    memcpy(&flat[1*STRIDE],frame_buf[i_curr],STRIDE);
    memcpy(&flat[2*STRIDE],frame_buf[i_next],STRIDE);

    int32_t scores[5]; infer_scores_u8(flat,scores);
    int argmax_all=argmax5_except2(scores,false);
    uint8_t rms_now=current_features[0];
    int pred=(COOL_BLOCK_EN && rms_now>=RMS_COOL_BLOCK && argmax_all==2) ? argmax5_except2(scores,true) : argmax_all;
    decision=vote_class(pred,rms_now);
  } else {
    decision = 0;
  }

  // 리프랙토리 1초
  uint32_t now = millis();
  bool is_action = (decision==1 || decision==2 || decision==3 || decision==4);
  if (hold_active){
    if ((now - hold_started_ms) < HOLD_MS) return;
    else hold_active = false;
  }
  if (!is_action) return;

  // 초음파 게이트
  uint16_t dcm = ultrasonic_get_distance_cm();
  bool gate_ok = (dcm != 65535) && (dcm <= ULTRA_THRESH_CM);
  if (!gate_ok) return;

  switch (decision){
    case 1: g_tone = TONE_WARM; break;   // warm
    case 2: g_tone = TONE_COOL; break;   // cool
    case 3: if (g_level>0) g_level--; break; // low
    case 4: if (g_level<3) g_level++; break; // high
  }
  apply_led_state();
  hold_active = true;
  hold_started_ms = now;
}

// ====== UART0 패킷 파서 (SPI가 없을 때만 호출) ======
enum RxState { WAIT_STX, WAIT_SEQ, WAIT_LEN, WAIT_PAYLOAD, WAIT_XOR, WAIT_ETX };
static RxState rx_state = WAIT_STX;
static uint8_t rx_buffer[1 + 1 + N_FEATURES_PER_FRAME + 1];
static uint8_t rx_idx=0, rx_payload_len=0;

static bool try_parse_uart0_once(){
  int rb;
  while ((rb = uart0_read_byte_nonblock()) >= 0){
    uint8_t b = (uint8_t)rb;
    switch(rx_state){
      case WAIT_STX: if(b==STX){ rx_state=WAIT_SEQ; rx_idx=0; } break;
      case WAIT_SEQ: rx_buffer[rx_idx++]=b; rx_state=WAIT_LEN; break;
      case WAIT_LEN: rx_buffer[rx_idx++]=b;
        if(b==N_FEATURES_PER_FRAME){ rx_payload_len=b; rx_state=WAIT_PAYLOAD; }
        else rx_state=WAIT_STX;
        break;
      case WAIT_PAYLOAD: rx_buffer[rx_idx++]=b;
        if(rx_idx>=(2+rx_payload_len)) rx_state=WAIT_XOR;
        break;
      case WAIT_XOR: rx_buffer[rx_idx++]=b; rx_state=WAIT_ETX; break;
      case WAIT_ETX:
        if(b==ETX){
          uint8_t ck=0; for(int i=0;i<(2+rx_payload_len);i++) ck^=rx_buffer[i];
          uint8_t rx_ck=rx_buffer[2+rx_payload_len];
          if(ck==rx_ck){
            memcpy(current_features,&rx_buffer[2],N_FEATURES_PER_FRAME);
            consume_features_and_drive(current_features);
          }
        }
        rx_state=WAIT_STX; return true;
    }
  }
  return false;
}

// ====== Setup / Loop ======
void setup(){
  cli();
  pwm_init();
  ultra_hw_init();
  spi_slave_init();
  uart0_init_rx_57600();
  sei();

  // 기본 LED 상태
  g_tone = TONE_WARM; g_level = 1; apply_led_state();

  // 모델 입력 크기 확인(안 맞으면 점멸)
  if(N_FEAT != (3*N_FEATURES_PER_FRAME)){
    for(int i=0;i<8;i++){
      g_tone = (i&1)?TONE_COOL:TONE_WARM;
      g_level = (i%4);
      apply_led_state();
      delay(120);
    }
    g_tone = TONE_WARM; g_level = 1; apply_led_state();
  }
}

void loop(){
  // 1) SPI 우선 소비
  if(spi_packet_ready){
    noInterrupts(); uint8_t local[N_FEATURES_PER_FRAME];
    for(uint8_t i=0;i<N_FEATURES_PER_FRAME;i++) local[i]=spi_payload[i];
    spi_packet_ready=false; interrupts();
    consume_features_and_drive(local);
    return;
  }
  // 2) SPI 없을 때만 UART0 파서
  (void)try_parse_uart0_once();

  // 3) 초음파 주기 갱신(캐시)
  (void)ultrasonic_get_distance_cm();
}
