# Arty Z7-20

Arty Z7-20 한 보드에서 동작하는 PS 소프트웨어와 PL 가속기 자료를 함께 둔다.

```text
arty/
  ps_db/   DB PL용 Cortex-A9 Linux 애플리케이션과 테스트
  ps_eb/   EB PL용 Cortex-A9 Linux 애플리케이션과 테스트
  pl/   ROI 분류 가속기 HLS 소스와 구현 보고서
  classifier_linux_db/   DB XSA 기반 PetaLinux 프로젝트
  classifier_linux_eb/   EB XSA 기반 PetaLinux 프로젝트
```

PS는 Jetson에서 96x96 RGB UINT8 ROI를 TCP로 받고, INT8 양자화와
98x98 pre-padding을 수행한 뒤 PL을 실행한다. PL의 12x12x64 feature map은
PS가 GAP/FC/argmax로 후처리하여 분류 결과를 Jetson에 반환한다.

컴포넌트 사이의 정확한 데이터 형식과 처리 규칙은
[`../docs/contracts/ROI_CLASSIFIER_CONTRACT.md`](../docs/contracts/ROI_CLASSIFIER_CONTRACT.md)를
정본으로 사용한다.
