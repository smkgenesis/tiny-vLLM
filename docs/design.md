# 설계 계약

## Workload와 요청 진행

입력은 prompt/output 길이, 논리 도착 step, token/active 예산, prefill chunk 상한, KV geometry와 block pool이다. 필드 이름과 구조는 [예제](../examples/small.yaml)를 따른다. 알 수 없는 필드·중복 key/ID·잘못된 숫자·크기 overflow·개별 요청이 pool보다 큰 설정은 거부한다.

- 동일한 full causal attention layer의 비양자화 KV를 모델링한다. MHA/GQA/MQA는 KV head 수로 저장 형상을 표현한다.
- Running 요청을 먼저 배정하고 waiting 요청은 FCFS로 admission한다. 같은 도착 step은 입력 순서다. 선두가 예약할 수 없으면 뒤 요청도 우회하지 않는다.
- 최종 KV 길이 `P + O − 1`의 block 공간을 먼저 예약하고, 접근이 진행될 때 실제 block을 할당한다. 예약량은 할당량을 포함한다.
- 모든 요청의 현재 step 작업이 끝난 뒤 상태를 commit하고 종료 요청의 block을 반환한다. 반환 공간은 다음 step부터 재사용한다.
- `arrival_step`은 backend cycle과 독립적이다. 실제 도착률·TTFT·TPOT를 모델링하지 않는다.

Prefix cache, preemption, sliding window, MLA, speculative decoding, 분산 실행, tokenizer·가중치·수치 attention은 지원하지 않는다.

## 접근 정책과 주소

정책은 `prefill_store_decode_scan`이다. Prefill은 새 KV를 쓰며 attention read를 생략한다. Decode는 새 KV를 쓴 뒤 자기 token을 포함한 유효 context 전체를 읽는다. 해당 요청/layer의 write 완료를 기다린 후 read하고, layer 접근을 마친 뒤 다음 layer로 진행한다.

주소 배치는 `[physical block bundle][layer][K/V][KV head][token][dimension]`이다. vLLM GPU layout이나 GPU DRAM traffic과 동일하다는 뜻은 아니다. Transaction은 block/배치 경계와 `transaction_bytes` 상한에 맞춰 분할하며 padding을 전송하지 않는다.

`L`은 layer 수, `H`는 KV head 수, `D`는 head dimension, `S`는 dtype bytes일 때 token당 K+V는 `T=2LHDS` bytes다. 독립 byte 기준은 다음과 같다.

```text
Write = T × Σ(P + O − 1)
Read  = T × Σ((O − 1)P + O(O − 1)/2)
```

이 식은 정의한 synthetic 정책의 logical bytes다. 실제 모델 실행의 접근량이 아니다.

## Backend 인터페이스

[`Backend`](../include/tiny_vllm/core.hpp)는 `try_submit(MemoryRequest)`, `tick()`, `poll_completion()`을 제공한다. Request는 64-bit ID·byte address·byte count와 write 방향을 가진다.

- `try_submit=false`면 core가 같은 요청을 보존한다. `true`면 backend가 소유하며 core는 재전송하지 않는다.
- ID는 실행마다 증가하며 완료는 한 번만 반환한다. Core는 순서가 바뀐 완료도 처리한다.
- 실제 완료 전에는 다음 접근 phase, step commit, KV 반환을 진행하지 않는다.
- `max_outstanding`은 core의 상한이며 backend 자체의 수용 능력을 늘리지 않는다.
- `max_backend_cycles`를 초과하면 timeout이다. 실패 시 부분 기록과 남은 자원을 보고한다.

새 RTL을 연결할 때 backend 구현과 CMake의 Verilator source/top 설정을 추가한다. DUT의 수락·완료·reset 규칙을 adapter에 반영하고 대상 전용 wire checker를 작성한다. 기존 scheduler와 KV 주소 생성기를 수정할 필요는 없다. 지연을 임의로 추정해 완료를 대신하지 않는다.

## 외부 AXI RAM

[`axi_backend.cpp`](../src/axi_backend.cpp)는 원본 [`axi_ram.v`](../rtl/vendor/verilog-axi/axi_ram.v)에 직접 AXI 신호를 구동한다. 고정 SHA는 `516bd5dadc3365b7f9e225d2af8fe0b8d804fe53`이며 빌드 시 원본 hash를 확인한다.

| 항목 | 계약 |
|---|---|
| RTL 구성 | 64-bit data/ID, 20-bit byte address, 1 MiB, `PIPELINE_OUTPUT=0` |
| 동시성 | read descriptor 하나 + write descriptor 하나. 같은 방향 lane이 차면 거절 |
| 분할 | 정렬된 1/2/4/8-byte 전송, INCR burst, 최대 256 beats, 4 KiB 경계 준수 |
| Write 완료 | 모든 sub-burst의 B handshake 이후 원래 ID 완료 |
| Read 완료 | 모든 sub-burst의 마지막 R handshake 이후 원래 ID 완료 |
| Reset | 두 reset rising edge 후 기록 시작. 실행 중 reset 제외 |
| 용량 검사 | Arena가 1 MiB보다 크면 config error. Descriptor 범위는 adapter에서 검사 |

WSTRB는 해당 byte만 선택한다. 검증 패턴은 `(id*131 + address*17 + (address>>8)) mod 256`이다. Adapter shadow memory와 별도 Python wire replay가 저장·읽기를 확인한다. 초기화되지 않은 byte 읽기는 오류다. 이 패턴은 실제 KV 수치가 아니다.

`max_burst_beats`는 1..256이다. `response_pause_period`와 `response_pause_cycles`는 BREADY/RREADY를 주기적으로 내려 정지를 유발한다. 정지는 testbench 조건이며 DRAM 지연이 아니다. VALID가 정지된 동안 payload는 유지되어야 한다. 종료 시 lane·완료 queue·DUT B/R valid가 비고 수락/완료 수가 일치해야 한다.

원본 RAM의 B/R response는 OKAY 고정이고 WLAST 대신 AWLEN으로 write 길이를 처리한다. 따라서 주소 범위와 master WLAST는 별도로 검사한다. 원본의 검토된 width·unused signal·incomplete case 경고만 해당 target에서 제외한다. 원본 RTL은 수정하지 않는다.

`--inject-fault read-data|drop-completion`은 adapter 관측/완료 전달의 negative control이다. 외부 RTL 내부 오류 주입이나 실제 설계 결함 발견이라고 표현하지 않는다.

## 예제 RTL과 관측

[`kv_service.sv`](../rtl/kv_service.sv)는 descriptor FIFO와 일정 처리 지연의 단일 engine이다. 실제 데이터 저장·DRAM timing은 없다. FIFO 깊이는 1..64의 runtime 수락 제한이며 물리 배열 면적 변경이 아니다. `fifo_depth`, `service_cycles`, `initial_request_pause_cycles`를 설정한다.

공통 출력은 `input.yaml`, `resolved_config.yaml`, `metadata.yaml`, `summary.csv`, `events.csv`, `transactions.csv`, `driver_waits.csv`다. 같은 transaction ID의 generated/accepted/completed 행을 중복 합산하지 않는다. Byte 합계는 completed 행 기준이다. 이벤트의 token/output 수는 요청별, 점유량·outstanding은 전체 상태다.

AXI의 `axi_cycles.csv`는 edge 직전 AW/W/B/AR/R 신호를 기록한다. `axi_summary.csv`는 descriptor, burst, beat, byte와 B/R stall을 구분한다. 예제 RTL은 `rtl_cycles.csv`, `rtl_handshakes.csv`, `rtl_summary.csv`를 사용한다. Frontend acceptance와 실제 RTL handshake 시각은 다를 수 있다.

AXI `backend_cycles`는 reset 이후 전체 edge 수다. 예제 RTL의 `completion_span_cycles`는 첫 request부터 마지막 completion까지의 차다. 서로 다른 측정 경계를 speedup으로 비교하지 않는다. Host wall time도 모델링한 cycle과 구분한다.

Exit code는 성공 0, 사용법/config 오류 2, 실행 실패/timeout 3, 출력 경로/I/O 오류 4다. 기존 결과 폴더는 덮어쓰지 않는다.
