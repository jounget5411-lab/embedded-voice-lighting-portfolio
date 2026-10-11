# 코드 안내와 원본 선택

원본 보존 저장소의 기준 커밋은 `c8618b1caef9cdf128ed20f62692d63818d9dd61`입니다. 여러 시도·중복본 가운데 장치 흐름을 설명하는 소스만 선별했습니다. 공개 파일의 원래 경로·hash는 [source-manifest.json](source-manifest.json)에 기록했습니다.

원본 공통 경로: `archive/Desktop/대학교오오/3학뇬/2학기/임시설/플젝1/`.

| 공개 파일 | 원본 아래 경로 | 읽을 부분 |
|---|---|---|
| [A_wclh_5.ino](../firmware/A_wclh_5/A_wclh_5.ino) | `시도7-spi+uart+3색 led+초음파/A_wclh_5/A_wclh_5.ino` | ADC ISR, 버퍼 교대, 정수 특징, 패킷·링크 상태 |
| [B_wclh_5.ino](../firmware/B_wclh_5/B_wclh_5.ino) | `시도7-spi+uart+3색 led+초음파/B_wclh_5 - cool크기good/B_wclh_5/B_wclh_5.ino` | SPI/UART 수신, 24차원 추론, 후처리, Timer1 입력 캡처, PWM |
| [weights_wclh_k5.h](../firmware/B_wclh_5/weights_wclh_k5.h) | 위 B 폴더 | 평균·스케일·가중치·편향의 데이터 표현 |
| [log_features_serial_5bin.py](../tools/log_features_serial_5bin.py) | `log_features_serial_5bin.py` | 바이너리 특징 패킷 읽기, seq/체크섬 검증, CSV 저장 |
| [prep_train_export_wclh_k5.py](../tools/prep_train_export_wclh_k5.py) | `파이썬 정리/prep_train_export_wclh_k5.py` | 학습 입력, VAD·균형화, 정수 헤더 생성 |

## 버전을 선택한 이유

시도7의 B코드는 Timer1 입력 캡처를 사용하는 버전입니다. `legend_spi_uart`의 동명 폴더에는 `pulseIn` 기반 이전 버전도 있으므로 한 쌍의 A/B와 같은 폴더의 헤더를 선택했습니다. `real_final`이나 `final`이라는 이름 자체를 최종 구현의 근거로 쓰지 않았습니다. A sketch의 SPI 설정은 31,250 Hz이며 보고서의 125 kHz와 구분합니다.

`record_k5_keyboard_label.py`라는 보존 파일은 실제 내용이 학습·헤더 내보내기 코드여서 수집 도구로 선택하지 않았습니다. `log_features_serial_5bin.py`는 `read_packet`에서 실제 시리얼 패킷을 읽는 도구입니다. 기본 bins는 PC 순서이므로 선택 A의 배열에 맞춰 `--bins`를 지정해야 하며, 선택 A의 USB 출력은 디버그 텍스트이므로 바이너리 수집용 출력/배선도 별도 대조해야 합니다.

## 모델 조합의 남은 확인

펌웨어와 PC 자료에서 주파수 특징의 마지막 두 열 순서가 다른 기록이 있습니다. 같은 폴더의 헤더를 보존한 사실과 그 헤더가 어떤 CSV·열 순서로 만들어졌는지를 완전히 검증한 사실은 구분합니다. [실행 준비](reproduction.md)의 대조 항목을 먼저 확인해야 합니다.

## 공개 구성

핵심 sketch·모델·PC 도구와 정수 특징 CSV, 모델 메타데이터 참조를 제공합니다. [reference_features.csv](../data/reference_features.csv)의 마지막 두 주파수 열은 p2300/p800 순서이며, [reference_model_meta.json](../data/reference_model_meta.json)은 모델 입력의 열 순서를 보여주는 보존 메타데이터입니다. 이것만으로 같은 폴더의 공개 모델 헤더 생성 과정을 재검증했다는 뜻은 아닙니다. 원시 음성·과거 시도 전체는 포함하지 않았습니다.
