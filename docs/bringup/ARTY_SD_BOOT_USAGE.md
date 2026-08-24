# Arty Z7-20 SD 부팅 사용법 (DB)

## 1. DB 사용법

### 1.0 부팅

1. DB SD 카드를 보드에 꽂는다 (`arty/deploy/burn_sd.sh` 참고).
2. 점프퍼를 SD 부팅으로 맞춘다.
3. UART 콘솔을 먼저 연다.

   ```bash
   sudo picocom -b 115200 /dev/ttyUSB1
   ```

4. 보드 전원을 켜거나 RESET을 누른다.

**초록색 DONE LED가 켜지는지 확인한다.** 이게 켜져야 비트스트림이 정상
로드된 것이고, 부팅이 제대로 진행되고 있다는 뜻이다. 안 켜지면 그 뒤로
아무리 기다려도 UART에 아무것도 안 나온다.

**부팅 중간에 화면이 한동안 멈춘 것처럼 안 움직이는 구간이 있다. 정상이다 —
그냥 기다리면 로그인 프롬프트까지 올라온다.**

5. 로그인한다.

   ```text
   login: petalinux
   Password: (Enter — 빈 비밀번호)
   ```

   첫 로그인이면 새 비밀번호를 설정하라고 나온다.

**네트워크는 따로 잡을 필요가 없다.** DB는 rootfs가 영속 ext4
(`root=/dev/mmcblk0p2`)로 바뀌면서 `enx020000000020` → `10.10.16.61` 고정
IP가 이미지에 recipe로 박혀 있다 — 재부팅해도 유지된다. 바로
`ping 10.10.16.61`이 된다. (예전엔 initramfs라 매번 IP를 다시 잡아야
했다 — 그 절차와 EB용 안내는
[`ARTY_NETWORK_SETUP.md`](ARTY_NETWORK_SETUP.md) 참고.)

### 1.1 모델 업로드 (PC에서)

```bash
scp -r arty/models/roi_classifier_int8_db/export petalinux@10.10.16.61:/home/petalinux/model
```

### 1.2 드라이버 확인

```sh
dmesg | grep -i adas
```

```text
adas_classifier: loading out-of-tree module taints kernel.
adas_classifier 40000000.classifier: DMA buffer at 0x1f060000, size 0x13000
```

### 1.3 서버 바이너리 (더 이상 직접 안 올려도 된다)

`ps_classifier_server`는 2026-08-20부터 `ps-classifier-server` 레시피로
이미지에 편입돼서 `/usr/bin/ps_classifier_server`에 이미 있다 — SD 이미지를
새로 굽기만 하면 따라온다. `ps_db_golden_test`도 마찬가지다.

**레시피 편입 전의 소스를 급하게 테스트하고 싶을 때만** 아래처럼 PC에서
크로스컴파일해서 임시로 덮어쓴다(재부팅하면 이미지에 든 버전으로 되돌아간다).

```bash
cmake -S arty/ps_db -B arty/ps_db/build_arm \
    -DCMAKE_TOOLCHAIN_FILE="$(pwd)/arty/tools/toolchain_arm_cortexa9.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build arty/ps_db/build_arm -j2 --target ps_classifier_server

scp arty/ps_db/build_arm/ps_classifier_server petalinux@10.10.16.61:/tmp/
ssh petalinux@10.10.16.61 'sudo cp /tmp/ps_classifier_server /usr/bin/ps_classifier_server'
```

`-DCMAKE_TOOLCHAIN_FILE`은 절대경로(`$(pwd)/...`)로 준다 — 상대경로를 주면
처음 `-B`로 빌드 디렉터리를 새로 만드는 configure에서 "toolchain file을 못
찾는다"로 실패한다.

소스를 고쳤다면 이 임시 방편 대신 SD 이미지 자체를 다시 굽는 쪽이 맞다 —
`petalinux-build` → `petalinux-package boot --fsbl --fpga --u-boot --force`
→ `images/linux/`를 `arty/deploy/db_sd_boot/`로 복사 → `burn_sd.sh db`.
다만 **DB는 이제 rootfs가 SD카드 두 번째 파티션의 영속 ext4라, `burn_sd.sh`가
다루는 BOOT.BIN/boot.scr/image.ub(첫 파티션) 갱신만으로는 그 파티션이
안 바뀐다** — rootfs를 다시 쓰는 절차는 아직 스크립트로 안 만들어져 있다.

### 1.4 PL 골든 테스트 (선택)

이미 검증된 SD 카드·가중치 그대로면 매번 안 돌려도 된다. 이미지를 새로
굽거나 가중치를 바꿨을 때, 또는 뭔가 이상할 때 원인 확인용으로 돌린다.

```sh
sudo ps_db_golden_test /home/petalinux/model
```

```text
PASS: 9216 bytes bit-exact, accelerator time=6570 us
report: golden_report
```

### 1.5 서버 실행

```sh
sudo ps_classifier_server "*" 5000 /home/petalinux/model 6 1 \
    1467099144 38 1160501223 35 1422046702 38 8.540366656652573e-06
```

```text
classifier server listening on port 5000
```

마지막 인자(`2.919...e-05`)는 `model/manifest.json`의 `logits_scale`이다 —
confidence 계산에만 쓰이고, 없어도 분류(class_id)는 그대로 나오지만
confidence가 항상 0으로 나간다.

### 1.6 동작 확인 (PC에서, golden 벡터로 검증된 요청)

```bash
python3 /path/to/roi_client.py 10.10.16.61 5000 arty/models/roi_classifier_int8_db/export
```

```text
status=0 class_id=2 confidence_ppm=992666
```

---

## 2. Jetson 사용법

### 2.0 켜기

Jetson은 자체 JetPack SD 이미지로 부팅하며 RPi 뒤의 전용 Ethernet 구간에서
`192.168.100.2`를 사용한다.

1. 전원을 켠다.
2. 부팅될 때까지 기다린다 (SD 보드들과 달리 화면이 안 보여도 정상 — HDMI를
   안 꽂았으면 그냥 헤드리스로 올라온다).
3. SSH로 접속한다.

   ```bash
   ssh -J ubuntu@10.10.16.200 jetson@192.168.100.2
   ```

카메라(`/dev/video0`)와 저장소(`/home/jetson/adas_project_sub`)는 이미
올라와 있다. 빌드가 안 돼 있으면 `jetson/README.md`의 Build and test 참고.

### 2.1 실행 — DB에 붙이기

```bash
cd /home/jetson/adas_project_sub/jetson
./build/jetson_roi_client /dev/video0 10.10.16.61 5000 \
    models/proposal/export/proposal_yolov8n_fp16.engine 8080
```

### 2.2 결과

```text
V4L2Capture: /dev/video0  YUYV 640x360 @30fps  bytesperline=1280 sizeimage=460800
camera: YUYV 640x360 @30fps  bytesperline=1280 sizeimage=460800
MJPEG stream: http://<jetson-ip>:8080/
frame=0 roi=0 status=0 class=1 confidence_ppm=976539
frame=0 roi=1 status=0 class=2 confidence_ppm=658523
...
```

마지막 인자(`8080`)는 선택이다. 주면 브라우저로
직접 확인할 때는 Jetson의 `http://192.168.100.2:8080/`을 사용한다. 외부
브라우저에서는 RPi가 포트 8080을 Jetson으로 DNAT하므로
`http://10.10.16.200:8080/`으로 접근한다. 통합 화면은
`http://10.10.16.200:8090/`이다. 마지막 인자를 생략하면 스트리밍 없이
터미널 로그만 출력한다.

Arty DB 서버가 먼저 떠 있어야 한다 — 그렇지 않으면 연결부터 실패한다.

---

## 3. 종료

### Arty (DB)

```sh
# 서버가 떠 있는 터미널에서
Ctrl+C
```

또는 원격에서 종료한다. **`pkill`이 이 rootfs에는 없다** (`command not
found`로 실패한 적 있음) — PID를 찾아서 직접 죽인다.

```sh
pgrep -af ps_classifier_server
sudo kill -9 <PID>
```

### Jetson

```sh
# jetson_roi_client 가 떠 있는 터미널에서
Ctrl+C
```

원격에서 종료할 때는 Jetson은 일반 Ubuntu 기반이라 `pkill`이 있다.

```bash
pkill -f jetson_roi_client
```
