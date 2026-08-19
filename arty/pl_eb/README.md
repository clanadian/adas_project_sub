# PL 인수인계 — Arty Z7-20 ROI 분류기 (arty_96_classifier)

**받는 사람: 다음 PL 엔지니어.** 이 폴더에는 HLS 소스 12개·tcl·게이트·검증
하네스·문서와, PS 인계 산출물(`bitstream/`·`golden/`·`weights/`·`HANDOFF.md`·
`PL_CONTRACT_DELTA.md`)이 **함께 있다** (2026-08-19 병합 — 예전에는
`handoff/` 서브폴더로 분리돼 있었다).

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

## PS 인계 산출물 — `bitstream/` `golden/` `weights/` `HANDOFF.md` `PL_CONTRACT_DELTA.md`

원래 `repo_python/build_arty_deliverable.py`가 이 트리에서 별도 `handoff/`
폴더로 **생성하던** 패키지였다. PS 쪽에서 소스 트리와 나란히 두고 쓰기
편하도록 2026-08-19에 이 최상위로 병합했다 — `HANDOFF.md`는 옛 `handoff/README.md`를
개명한 것이고(가중치 없던 구판 `HANDOFF.md`는 폐기), `golden/`·`bitstream/`·
`weights/`도 같은 자리로 옮겼다. `sw/`·`reports/`·`HW/`처럼 `SW/`·`system/`·`HW/`와
내용이 완전히 같던 중복은 지웠다.

**재생성 스크립트를 다시 돌릴 때는 이 최상위 파일들과 충돌하지 않게
`--out` 경로를 확인할 것** — 예전처럼 `handoff/`로 출력하면 이 병합과
다시 갈라진다.

```bash
python3 repo_python/build_arty_deliverable.py . --out <별도 경로>
```

`DELIVERABLE_CHECKSUMS.sha256`는 병합 전 `handoff/` 구조 기준이라 지금
경로와는 안 맞는다 (파일 안 주석 참고). 데이터 무결성은 `golden/SHA256SUMS`,
`weights/SOURCE.sha256`로 확인한다.

### `golden/` — 실가중치 golden 하나만 쓴다

**2026-08-19: `forps_golden/`(seed=42 의사난수, 엔진 배선 검증 전용)을
지웠다.** `golden/`(실제 학습 INT8 익스포트, `../models/roi_classifier_int8_eb/export/`
와 바이트 일치)만 남았고, PS 온보드 대조는 물론 로컬 게이트에도 이걸 쓴다.

⚠️ **`verif_host/build_and_run.sh`와 `python/gen_manifest.py`,
`check_shapes.py`, `verify_golden.py`는 아직 `forps_golden/`을 참조하거나
seed=42로 그 폴더를 재생성하는 코드를 그대로 갖고 있다.** 이 스크립트들을
그대로 돌리면 지운 폴더가 다시 생기거나 존재하지 않는 경로를 찾다 실패할
수 있다 — PL 팀이 직접 그 로직을 정리해야 한다(다음 PL 엔지니어에게
인계할 사항).

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
