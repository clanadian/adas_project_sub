# Arty Z7-20

Arty Z7-20 한 보드에서 동작하는 PS 소프트웨어와 PL 가속기 자료를 함께 둔다.

```text
arty/
  ps_db/   DB PL용 Cortex-A9 Linux 애플리케이션과 테스트
  pl_db/   DB PL 가속기 HLS 소스와 구현 보고서
  classifier_linux_db/   DB XSA 기반 PetaLinux 프로젝트
  deploy/  SD 카드 굽기·검사 스크립트 (burn_sd.sh, inspect_sd.sh)
  tools/   XSA 하드웨어 설정 검증 (check_xsa.sh)
```

최종 지원 대상은 DB 구현이다. 비교 실험에 사용한 EB 소스·모델·
PetaLinux 프로젝트는 Git 태그 `eb-comparison-final`에 보존하고, 실측
결과는 [`../docs/DB_EB_VERIFICATION_SUMMARY.md`](../docs/DB_EB_VERIFICATION_SUMMARY.md)에 남겨 둔다.

PS는 Jetson에서 원본 bbox 메타데이터와 96x96 RGB UINT8 ROI를 TCP로 받고, INT8 양자화와
98x98 pre-padding을 수행한 뒤 PL을 실행한다. PL의 12x12x64 feature map은
PS가 GAP/FC/argmax로 후처리한다. 분류 결과는 Jetson에 반환해 화면에 표시하고,
PS 내부에서는 bbox와 결합해 안전 상태를 판단한 뒤 UART1으로 TurtleBot
Raspberry Pi에 직접 전송한다.

최종 DB XSA의 UART 배치는 UART0=MIO 14..15(콘솔), UART1=EMIO다. UART1 외부
핀은 PMOD JA1(`Y18`, TXD), JA2(`Y19`, RXD)이며 두 보드의 GND를 함께 연결한다.

컴포넌트 사이의 정확한 데이터 형식과 처리 규칙은
[`../docs/contracts/ROI_CLASSIFIER_CONTRACT.md`](../docs/contracts/ROI_CLASSIFIER_CONTRACT.md)를
정본으로 사용한다.
