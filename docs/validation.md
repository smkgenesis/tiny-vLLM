# 검증과 재현

## 검증 범위

검증 환경은 Ubuntu 24.04.4 / WSL2, GCC 13.3, CMake 3.28.3, Ninja 1.11.1, yaml-cpp 0.8, Catch2 3.4, Verilator 5.020, Python 3.12.3이다. 다른 OS·toolchain 조합까지 검증했다는 뜻은 아니다.

| 검사 | 확인 내용 |
|---|---|
| Core / CLI | 독립 주소 golden, scheduler/KV 상태, 경계·재사용·완료 순서, 설정 오류 |
| 예제 RTL | FIFO·응답 정지·지연·pointer wrap·주소 범위, 완료 누락/중복 |
| 외부 AXI | 실제 RAM readback, strobe, ID, 4 KiB 분할, 256-beat, phase/수명 |
| Negative control | 관측값 변조·완료 전달 누락, 복사한 기록의 조기 반환·RDATA 변조 |
| 재현 | 새 build/output, resolved 입력 replay, 저장된 wire를 별도 Python으로 검사 |

현재 검사에는 기능을 확인하는 22개 CTest가 있다. 이전 26개에서 도구 설치 확인용 C++ smoke 1개와 전용 clock/reset probe 3개를 제외했다. 실제 core·예제 RTL·외부 AXI의 검사는 유지한다. Trace만 빌드하면 14개다.

공통 generator와 trace/RTL 결과의 일치는 전송 일관성 확인이다. Generator의 정답은 별도의 작은 주소 기준·byte 식으로 확인한다. 모든 입력에 대한 형식 검증이나 AXI4 전체 적합성 인증은 아니다.

## 대표 결과

| AXI workload | Write / read bytes | Descriptor | 정상 / 응답 정지 cycles |
|---|---:|---:|---:|
| small | 56 / 104 | 18 | 71 / 151 |
| mixed | 80 / 128 | 26 | 97 / 207 |
| pressure | 80 / 128 | 24 | 95 / 207 |

Small의 18 descriptor는 AW 12개와 AR 10개로 분할된다. Pressure에서는 두 번째 요청의 admission이 step 4로 늦춰지며, 정상 종료 후 모든 KV 자원은 반환된다. 다층·다중 head 조건은 720/1152 B, 4 KiB/256-beat 조건은 43036/79924 B를 검증한다. 홀수 주소·40-bit를 넘는 ID·부분 덮어쓰기·read/write 동시 진행은 직접 backend 검사에 포함된다.

전체 AXI suite는 기본 세 workload의 정상/정지 6회, 다층·큰 burst 2회, resolved replay 1회로 정상 9회다. Replay는 독립 실험 조건이 아니다. Adapter 오류 주입은 exit 3, 잘못된 설정은 exit 2이며 실패 기록도 남긴다.

## 전체 재현

빌드 의존성은 [README](../README.md)를 따른다. 그림에 필요한 Python 패키지만 별도 환경에 설치한다.

```bash
python3 -m venv "$HOME/.cache/tiny-vllm/plot-env"
"$HOME/.cache/tiny-vllm/plot-env/bin/python" -m pip install -r tools/experiments/requirements.txt
python3 tools/reproduce.py \
  --build-root "$HOME/.cache/tiny-vllm/reproduce-build" \
  --output results/reproduction \
  --plot-python "$HOME/.cache/tiny-vllm/plot-env/bin/python" --jobs 4
```

Build/output은 새 경로여야 한다. Trace 및 RTL/AXI 빌드, CTest, protocol/AXI 검사, 실험, 분석, 그림을 실행한다. `reproduction.json`은 명령·exit·전체 상태, `logs/`는 명령 출력, `protocol/`과 `external-axi/`는 정상·실패 원시 자료를 담는다. `experiments/`, `analysis/`, `figures/`에 실험 결과를 보존한다. 실패 시 nonzero exit로 종료한다.

## 실험과 패키징

```bash
python3 tools/experiments/run.py \
  --trace "$HOME/.cache/tiny-vllm/build/tiny-vllm-trace" \
  --rtl "$HOME/.cache/tiny-vllm/build/tiny-vllm-rtl" --output results/experiments
python3 tools/experiments/analyze.py results/experiments --output results/analysis
"$HOME/.cache/tiny-vllm/plot-env/bin/python" tools/experiments/plot.py results/analysis --output results/figures
python3 tools/package_source.py --output dist/tiny-vllm-source.zip
```

`tools/experiments/plan.py`가 실행 전에 입력·예측·byte 기대값을 선언한다. 23조건은 19개 고유 설정이며 4쌍은 반복이다. Trace/예제 RTL에서 46회 실행한다. FIFO 1/2/4에서 stall 100/64/10, span 189/189/189 cycles라는 결과는 대기 위치 변화와 단일 service 병목을 보여 준다. Chunk 1/2/3은 prefill step 최대량 8/16/24 B를 바꾸지만 전체 step 최대량은 모두 64 B다. 변화 없는 결과를 개선으로 재해석하지 않는다.

패키지는 소스·문서·예제·검사·외부 RTL 라이선스를 포함하고 Git, build, results, 환경을 제외한다. 기존 ZIP과 결과는 덮어쓰지 않는다. 원시 실행 자료·실패·개발 기록은 로컬 `results/`와 `build/`에 보존하며 저장소에는 재현 도구와 이 요약을 둔다.

## 선택적 vLLM 비교

기준은 [고정 vLLM commit](https://github.com/vllm-project/vllm/tree/b558f160a2c0abcb5902acc3c91a14c38a4af173)이다. 수정하지 않은 scheduler/allocator에 synthetic `ModelRunnerOutput`을 주며 가중치·GPU kernel은 실행하지 않는다. 이 Python 환경은 C++/RTL 실행에 필요하지 않다.

```bash
python3 -m venv "$HOME/.cache/tiny-vllm/reference-env"
"$HOME/.cache/tiny-vllm/reference-env/bin/python" -m pip install -r tools/reference/requirements.txt
git clone --filter=blob:none --no-checkout https://github.com/vllm-project/vllm.git "$HOME/.cache/tiny-vllm/vllm-reference"
git -C "$HOME/.cache/tiny-vllm/vllm-reference" fetch --depth 1 origin b558f160a2c0abcb5902acc3c91a14c38a4af173
git -C "$HOME/.cache/tiny-vllm/vllm-reference" checkout --detach b558f160a2c0abcb5902acc3c91a14c38a4af173
VLLM_TARGET_DEVICE=empty "$HOME/.cache/tiny-vllm/reference-env/bin/python" -m pip install \
  -e "$HOME/.cache/tiny-vllm/vllm-reference" --no-build-isolation --no-deps
"$HOME/.cache/tiny-vllm/reference-env/bin/python" tools/reference/run.py \
  --vllm-source "$HOME/.cache/tiny-vllm/vllm-reference" \
  --tiny-executable "$HOME/.cache/tiny-vllm/build/tiny-vllm-trace" --output results/reference
```

기존 환경은 재사용한다. 정상 9조건 중 7개·104행 일치, 2개(`chunk-one`, `longer-mixed`) 불일치다. Upstream은 [eligible 요청이 하나일 때 chunk threshold를 해제](https://github.com/vllm-project/vllm/blob/b558f160a2c0abcb5902acc3c91a14c38a4af173/vllm/v1/core/sched/scheduler.py#L623-L640)하고 tiny-vLLM은 항상 상한을 적용한다. 전체 suite는 **exit 1**을 유지한다. Memory pressure 정책·물리 주소·GPU traffic 동일성은 이 비교에서 제외한다.

## 연구에서 주장하는 범위

이 결과는 명시된 synthetic 접근 정책을 외부 RTL에 공급하고 수락·완료·KV 수명을 검증하는 구현 사례다. [LLMServingSim](https://arxiv.org/html/2408.05499v1), [Vidur](https://github.com/microsoft/vidur), [FlowSim](https://github.com/microsoft/FlowSim), [Ramulator](https://github.com/CMU-SAFARI/ramulator2) 등 선행 도구가 있으므로 HW/SW 연결 자체의 최초성이나 비교 우위를 주장하지 않는다. 외부 RTL 하나의 적용성을 확인한 것이며 실제 LLM 속도·DRAM 성능·넓은 버그 검출 우위를 측정한 것은 아니다.
