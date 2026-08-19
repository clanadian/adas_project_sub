# PL 인수인계 — Arty Z7-20 ROI 분류기 (arty_96_classifier)

**받는 사람: 다음 PL 엔지니어.** PS 인계본(`arty96_pl_handoff/`)과 **내용이 다릅니다** —
그쪽은 비트스트림·드라이버·golden 만 있고 **소스가 없습니다.** 이 폴더에는
HLS 소스 12개·tcl·게이트·검증 하네스·문서가 들어 있습니다.

## 이 폴더에 없는 것 (일부러 뺐습니다)

`*_prj/` (Vitis HLS 프로젝트)와 `system/s1/` (Vivado 프로젝트)는 **합쳐 700MB 이상이고
전부 재생성물**이라 뺐습니다. `bash run.sh package <엔진>` 과 `bash run.sh build` 가
다시 만듭니다.

## 처음 할 일

```bash
bash run.sh check          # 게이트. 툴 없이 돕니다
```

⚠️ **처음에는 "export 된 IP 가 모두 존재" 하나가 FAIL 합니다.** `*_prj/` 를 안 실었으니
당연하고, 아래를 돌리면 사라집니다.

```bash
bash run.sh package tr8 && bash run.sh package conv0 && bash run.sh package pool
bash run.sh build          # 합성+구현+XSA (약 25분)
bash run.sh check          # 이제 전건 통과해야 합니다
```

`system/*_arty*_v1.rpt` 가 **재현 목표**입니다. 빌드 후 WNS 와 면적이 그 리포트와
크게 다르면 무언가 바뀐 것입니다.

## 반드시 먼저 읽을 문서

| 파일 | 왜 |
|---|---|
| `doc/2026-08-18_arty96-perf-campaign.md` | **채택 3건 / 기각 5건.** 기각 목록이 더 값집니다 — 같은 벽에 다시 부딪히지 않게 합니다 |
| `doc/arty-classifier-log.md` | 트러블슈팅. 증상 → 원인 → 조치 |
| `doc/CLAUDE.md` | 이 저장소의 Gotchas (하드웨어·검사 양쪽) |
| `doc/2026-08-19_ps-request-conv0-row-stride.md` | PS 에 넘긴 요청 (프레임 약 4%) |

## 절대 재시도하지 말 것 (전부 cosim 으로 기각됨)

- `FINISH_WR` 을 행 버퍼(BRAM)로 모으기 — **+2.28%**
- `FINISH_WR` 을 oc_tile 루프 밖으로 병합 — **+16.72%**
- 출력 쓰기 4레인 UNROLL — **+33.7%** (`ofmap` m_axi 쓰기 포트가 하나라 II 1→4)
- `window` 완전 분할로 II 고치기 — II 불변, **LUT 바이트 동일**
- 꼬리 쓰기를 컴파일타임 인덱스로 — II 불변

**출력 쓰기 경로는 워드 패킹·레인 언롤 양방향이 다 닫혔습니다.**

## 남은 레버 (측정된 근거 있음)

`FUSED_SHIFT_STEP` 의 achieved II 가 2 입니다. 창 시프트가 위치당 창 전체를
물리적으로 옮기는 구조 때문이고, 순환 창(인덱스 회전)으로 바꾸면 II=1 이 가능하지만
`MAC_REDUCE` 의 읽기 인덱싱까지 재구조화해야 하고 **이득은 약 1%** 입니다.

## 이 폴더를 다시 만들려면

```bash
python3 repo_python/build_pl_source_handoff.py <트리경로>
```
