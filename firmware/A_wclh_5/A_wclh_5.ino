// ========================= A (MASTER: SPI primary, UART-SS fallback) =========================
#include <Arduino.h>
#include <avr/interrupt.h>
#include <math.h>
#include <SPI.h>
#include <SoftwareSerial.h>

// ====== Audio/Feature Config ======
#define FS            8000
#define N_SAMPLES     160
#define MEAN_SHIFT    3
#define STX           0xAA
#define ETX           0x55
#define RMS_FS        300
#define GZ_SQRT_SCALE 220
#define SILENCE_SKIP_ENABLE 0     // 디버깅 안정 위해 끔
#define SILENCE_THRESH_U8   8
#define SILENCE_FORCE_RECALC_N 20
#define FLAT_RMS_U8   2
#define FLAT_VPP_U8   2
#define FLAT_LIMIT    200

// ====== SPI (Master) ======
#define SPI_SS_PIN        10
#define FRAME_LED_PIN     4
#define SPI_SYNC          0xF0
#define SPI_READY         0xA5
#define SPI_ACK           0x5A

// ====== UART (SoftwareSerial) fallback ======
#define UART_TX_PIN 3     // A → B (B측 RX: D2)
#define UART_RX_PIN 2     // RX는 미사용(dummy)
#define UART_BAUD   57600

// ====== Failover policy ======
#define SPI_MAX_FAIL          3
#define SPI_RETRY_BLOCK_MS 2000

// ====== Goertzel bins ======
static const uint16_t GZ_FREQS[] = {1100,1200,2200,800,2300};
#define NUM_BINS (sizeof(GZ_FREQS)/sizeof(GZ_FREQS[0]))

static int16_t  GZ_COEF_Q14[NUM_BINS];
volatile uint16_t bufA[N_SAMPLES], bufB[N_SAMPLES];
volatile uint16_t wr=0; volatile bool frame_ready=false; volatile bool useA=true;
static uint8_t seq=0;

static int32_t mean_state=0;
static uint8_t last_p[NUM_BINS]={0};
static uint16_t flat_cnt=0, silence_cnt=0;

// failover 상태/카운터
static bool     prefer_spi = true;
static uint8_t  spi_fail = 0;
static uint32_t spi_block_until_ms = 0;
static uint32_t cnt_frames=0, cnt_spi_ok=0, cnt_spi_fail=0, cnt_uart_tx=0;
static uint32_t last_stat_ms=0;

SoftwareSerial UartSS(UART_RX_PIN, UART_TX_PIN); // RX(dummy), TX

// ====== ISR ======
ISR(TIMER1_COMPA_vect){ if(!(ADCSRA&(1<<ADSC))) ADCSRA|=(1<<ADSC); }
ISR(ADC_vect){
  uint16_t s=ADC;
  if(useA) bufA[wr]=s; else bufB[wr]=s;
  if(++wr>=N_SAMPLES){ wr=0; useA=!useA; frame_ready=true; PIND=_BV(PD4); }
}

// ====== Setup ADC 8kHz ======
static void setup_timer_adc_8k(){
  cli();
  TCCR1A=0; TCCR1B=(1<<WGM12)|(1<<CS11); OCR1A=249;
  TIMSK1=(1<<OCIE1A); TIFR1=(1<<OCF1A);
  ADMUX=(1<<REFS0)|0; // ADC0
  ADCSRA=(1<<ADEN)|(1<<ADIE)|(1<<ADPS2)|(1<<ADPS1)|(1<<ADPS0);
  ADCSRB=0; DIDR0=(1<<ADC0D);
  sei(); ADCSRA|=(1<<ADSC);
}

// ====== Utils ======
static inline uint16_t isqrt_u32(uint32_t x){
  uint32_t r=0,bit=1UL<<30; while(bit>x) bit>>=2;
  while(bit){ if(x>=r+bit){x-=r+bit; r=(r>>1)+bit;} else r>>=1; bit>>=2; }
  return (uint16_t)r;
}
static inline uint8_t build_packet_u8(const uint8_t* feat,uint8_t n,uint8_t*out){
  uint8_t i=0,ck=0; out[i++]=STX; out[i++]=seq; ck^=seq; out[i++]=n; ck^=n;
  for(uint8_t k=0;k<n;k++){ out[i++]=feat[k]; ck^=feat[k]; }
  out[i++]=ck; out[i++]=ETX; return i;
}

// ====== SPI send (SYNC→READY, 본문, ACK 폴링) ======
static inline bool send_packet_spi(const uint8_t* feat, uint8_t n){
  uint8_t buf[3+NUM_BINS+4]; uint8_t len=build_packet_u8(feat,n,buf);

  digitalWrite(SPI_SS_PIN,LOW);
  delayMicroseconds(12);

  (void)SPI.transfer(0x00);            // 프리로드 소거
  delayMicroseconds(8);
  (void)SPI.transfer(SPI_SYNC);        // SYNC
  delayMicroseconds(8);
  uint8_t ready=SPI.transfer(0x00);    // READY 기대(0xA5)
  delayMicroseconds(8);

  if(ready!=SPI_READY){ digitalWrite(SPI_SS_PIN,HIGH); return false; }

  for(uint8_t i=0;i<len;i++){          // 본문 전송
    SPI.transfer(buf[i]);
    delayMicroseconds(18);
  }

  // ACK 폴링(여유)
  uint8_t ack = 0x00;
  for (uint8_t tries=0; tries<8; ++tries){
    delayMicroseconds(18);
    ack = SPI.transfer(0x00);
    if (ack == SPI_ACK) break;         // 0x5A
  }

  digitalWrite(SPI_SS_PIN,HIGH);
  if (ack != SPI_ACK) return false;

  seq++;
  return true;
}

// ====== UART(SS) send ======
static inline void send_packet_uart(const uint8_t* feat, uint8_t n){
  uint8_t buf[3+NUM_BINS+4]; uint8_t len=build_packet_u8(feat,n,buf);
  UartSS.write(buf, len);
  seq++; cnt_uart_tx++;
}

// ====== Link send (SPI 우선, 실패 누적→UART, 창 종료 후 SPI 재시도) ======
static inline void link_send_packet(const uint8_t* feat, uint8_t n){
  uint32_t now = millis();
  bool try_spi = prefer_spi && (now >= spi_block_until_ms);

  if (try_spi){
    if (send_packet_spi(feat, n)){
      spi_fail = 0; cnt_spi_ok++;
      return;
    } else {
      cnt_spi_fail++;
      if (++spi_fail >= SPI_MAX_FAIL){
        prefer_spi = false;
        spi_block_until_ms = now + SPI_RETRY_BLOCK_MS;
      }
    }
  }

  // fallback UART
  send_packet_uart(feat, n);

  // 블록창 종료 시 SPI 재시도 허용
  if (!prefer_spi && now >= spi_block_until_ms){
    prefer_spi = true; spi_fail = 0;
  }
}

// ====== Frame processing ======
static void process_frame(const uint16_t* r){
  cnt_frames++;

  uint32_t sumsq=0; int16_t ymin=32767,ymax=-32768; uint16_t zcr=0;
  int16_t y_prev=0; bool have_prev=false;
  int32_t s1[NUM_BINS]={0}, s2[NUM_BINS]={0};

  for(int i=0;i<N_SAMPLES;i++){
    int16_t x=(int16_t)r[i];
    mean_state += ((int32_t)x - mean_state) >> MEAN_SHIFT;
    int16_t y=(int16_t)((int32_t)x - mean_state);

    sumsq += (uint32_t)((int32_t)y*(int32_t)y);
    if(y<ymin) ymin=y; if(y>ymax) ymax=y;
    if(have_prev){ if((y_prev<=0 && y>0)||(y_prev>=0 && y<0)) zcr++; } else have_prev=true;
    y_prev=y;

    for(uint8_t k=0;k<NUM_BINS;k++){
      int32_t t=((int32_t)GZ_COEF_Q14[k]*s1[k])>>14;
      int32_t s0=(int32_t)y + t - s2[k]; s2[k]=s1[k]; s1[k]=s0;
    }
  }

  uint16_t rms=isqrt_u32(sumsq/N_SAMPLES);
  uint32_t rms_u32=((uint32_t)rms*255U)/RMS_FS; if(rms_u32>255U) rms_u32=255U;
  uint8_t rms_u=(uint8_t)rms_u32;

  int32_t vpp=(int32_t)ymax-(int32_t)ymin; if(vpp<0)vpp=0; if(vpp>1023)vpp=1023;
  uint8_t vpp_u=(uint8_t)((vpp*255L)/1023L);
  uint8_t zcr_u=(uint8_t)(((uint32_t)zcr*255U)/(uint32_t)(N_SAMPLES-1));

  uint8_t p_u[NUM_BINS];
  for(uint8_t k=0;k<NUM_BINS;k++){
    int64_t a=(int64_t)s1[k]*s1[k], b=(int64_t)s2[k]*s2[k];
    int64_t c=((int64_t)GZ_COEF_Q14[k]*s1[k]*s2[k])>>14;
    int64_t p=a+b-c; if(p<0)p=0;
    uint16_t mag=isqrt_u32((uint32_t)(p>>8));
    uint32_t pu=((uint32_t)mag*255U)/GZ_SQRT_SCALE; if(pu>255U) pu=255U;
    p_u[k]=(uint8_t)pu;
  }

  uint8_t feat[3+NUM_BINS]; feat[0]=rms_u; feat[1]=vpp_u; feat[2]=zcr_u;
  for(uint8_t k=0;k<NUM_BINS;k++) feat[3+k]=p_u[k];

  link_send_packet(feat,(uint8_t)(3+NUM_BINS));

  // 1초마다 상태 요약
  uint32_t now=millis();
  if (now - last_stat_ms >= 1000){
    last_stat_ms = now;
    Serial.print("[A][link] frames="); Serial.print(cnt_frames);
    Serial.print("  SPI ok/fail="); Serial.print(cnt_spi_ok); Serial.print("/"); Serial.print(cnt_spi_fail);
    Serial.print("  UART tx="); Serial.print(cnt_uart_tx);
    Serial.print("  mode="); Serial.println(prefer_spi ? "SPI-first" : "UART-fallback");
  }
}

void setup(){
  pinMode(FRAME_LED_PIN,OUTPUT); digitalWrite(FRAME_LED_PIN,LOW);

  Serial.begin(115200);
  Serial.println("[A] SPI+UART(SS) failover");

  // SPI Master (31.25 kHz)
  pinMode(SPI_SS_PIN,OUTPUT); digitalWrite(SPI_SS_PIN,HIGH);
  SPI.begin();
  SPI.beginTransaction(SPISettings(31250,MSBFIRST,SPI_MODE0));
  // SS 라인 프라임
  digitalWrite(SPI_SS_PIN,LOW); SPI.transfer(0x00); digitalWrite(SPI_SS_PIN,HIGH); delay(2);

  // UART SoftwareSerial
  UartSS.begin(UART_BAUD);

  // Goertzel coef
  for(uint8_t i=0;i<NUM_BINS;i++){
    int k=(int)roundf((float)N_SAMPLES*(float)GZ_FREQS[i]/(float)FS);
    if(k<1)k=1; if(k>(N_SAMPLES/2-1)) k=(N_SAMPLES/2-1);
    float w=2.0f*PI*(float)k/(float)N_SAMPLES;
    int coef=(int)lroundf(2.0f*cosf(w)*(float)(1<<14));
    if(coef<-32768)coef=-32768; if(coef>32767)coef=32767; GZ_COEF_Q14[i]=(int16_t)coef;
  }
  setup_timer_adc_8k();
}

void loop(){
  if(frame_ready){
    noInterrupts(); frame_ready=false; bool procA=!useA; interrupts();
    if(procA) process_frame((const uint16_t*)bufA);
    else      process_frame((const uint16_t*)bufB);
  }
}
