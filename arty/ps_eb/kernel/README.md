# Classifier Linux driver

`adas_classifier_drv`가 AXI-Lite 레지스터와 coherent DMA 버퍼를 소유한다.
사용자 프로그램은 `/dev/adas_classifier`만 사용한다.

## Device Tree

[`adas-classifier.dtsi`](adas-classifier.dtsi)의 노드를 시스템 Device Tree의
`amba` 아래에 병합한다. PL의 HP0는 비일관성 포트이므로 `dma-coherent` 속성을
추가하지 않는다.

⚠️ **EB 노드는 `reg` 가 3개**(conv / conv0 / maxpool)이고 `compatible` 이
`adas,classifier-eb-1.0` 이다. DB 판(창 2개, `adas,classifier-1.0`)과 이름을
공유하면 반대쪽 드라이버가 붙어 존재하지 않는 레지스터를 건드린다.
`reg` 의 **순서가 곧 드라이버의 ioremap 인덱스**다.

## Kernel module

타깃에서 실행 중인 커널과 같은 소스·설정으로 빌드한다.

```bash
make KDIR=/path/to/target/kernel/build
sudo insmod adas_classifier_drv.ko
ls -l /dev/adas_classifier
dmesg | tail
```

## Userspace ABI

- `mmap`: 입력·가중치·bias·ping-pong 활성 버퍼가 들어 있는 coherent DMA 영역
- `SET_PARAMETERS`: conv 3단의 requant `(multiplier, shift, leaky)` 기록.
  **bias 는 여기 없다** — DDR 버퍼에 사용자 공간이 직접 쓰고 드라이버는
  주소만 넘긴다 (EB 계약)
- `RUN`: 6-op 을 순서대로 기동. op 마다 주소·형상·requant 를 해당 엔진에
  다시 쓰고 `ap_idle` 확인 → `ap_start` → `ap_done` 대기
- `RUN_OP`: op 하나만 기동. 단계별 golden 대조와 첫 점등에 쓴다
- `GET_STATUS`: 마지막 op 번호, 완료한 op 수, 소요 시간
- `GET_INFO`: ABI 버전(EB 는 **2**)과 mmap 레이아웃 확인

`RUN` 이 6-op 을 mutex 아래에서 한 번에 끝내는 이유는, 사용자 공간이 op 마다
ioctl 을 돌면 ROI 한 건에 왕복이 6번 생기고 그 사이에 다른 프로세스가 같은
엔진을 건드릴 여지가 열리기 때문이다.

DMA 영역의 물리주소는 드라이버 내부에서만 사용한다. 사용자 공간에서
`/dev/mem`, 물리주소 계산 또는 cache flush를 수행하지 않는다.
