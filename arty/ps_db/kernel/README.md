# Classifier Linux driver

`adas_classifier_drv`가 AXI-Lite 레지스터와 coherent DMA 버퍼를 소유한다.
사용자 프로그램은 `/dev/adas_classifier`만 사용한다.

## Device Tree

[`adas-classifier.dtsi`](adas-classifier.dtsi)의 노드를 시스템 Device Tree의
`amba` 아래에 병합한다. PL의 HP0는 비일관성 포트이므로 `dma-coherent` 속성을
추가하지 않는다.

## Kernel module

타깃에서 실행 중인 커널과 같은 소스·설정으로 빌드한다.

```bash
make KDIR=/path/to/target/kernel/build
sudo insmod adas_classifier_drv.ko
ls -l /dev/adas_classifier
dmesg | tail
```

## Userspace ABI

- `mmap`: 입력·가중치·출력이 들어 있는 coherent DMA 영역
- `SET_PARAMETERS`: requant와 Conv bias 기록
- `RUN`: DMA 주소 설정, `ap_start`, `ap_done` 대기
- `GET_INFO`: ABI 버전과 mmap 레이아웃 확인

DMA 영역의 물리주소는 드라이버 내부에서만 사용한다. 사용자 공간에서
`/dev/mem`, 물리주소 계산 또는 cache flush를 수행하지 않는다.

## 소스 위치

`adas_classifier_drv.c`는 PetaLinux 레시피의 파일을 가리키는 심볼릭 링크다.

```text
arty/classifier_linux_db/project-spec/meta-user/recipes-modules/adas-classifier/files/adas-classifier.c
```

보드 이미지에 들어가는 것이 그 파일이므로 **수정은 레시피 쪽에서 한다.**
예전에는 같은 내용이 양쪽에 복사돼 있어 한쪽만 고치면 조용히 어긋났다.

`adas_classifier_uapi.h`는 여전히 두 벌이다. `../include/driver/`의 것은
사용자 공간 빌드가 쓰고, 레시피 `files/`의 것은 bitbake 가 쓴다. ABI 를
바꾸면 **두 파일을 함께 고친다.**
