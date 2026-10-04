# Student onboarding check

**Status: protocol prepared; no participant sessions or integration-time measurements have been collected.** Automated build/tutorial checks are not a human usability study.

Target: a student with a Verilog/SystemVerilog block can run a serving-aware workload through a wrapper within 30 minutes after the toolchain is installed. Ten minutes is a longer-term goal, not a demonstrated result. Record setup time separately; complex DUT integration may exceed either target.

## Session

Recruit 2–3 willing students with small memory-facing RTL blocks. Give them the repository and ask: “Using only the README and linked documentation, connect your block and run a workload.” Do not explain the interface during the unassisted attempt. If help is requested, record its time and content and mark subsequent progress as assisted. Do not send invitations or report participants without their agreement.

Observe these milestones:

1. Explain what the tool does and does not model.
2. Build and run the basic reference example.
3. Copy the wrapper, map the DUT signals, and build the custom target.
4. Complete a workload and run the wire checker.
5. Find one accepted ID and its completion in CSV/VCD.

Successful integration requires the participant's actual block to execute the descriptor operation, not merely running the unchanged template. The DUT must have a meaningful completion signal and known payload/capacity behavior.

## Record a session

Use anonymous participant labels and record only what is needed:

| Field | Value to record |
|---|---|
| Participant / date / commit | Anonymous label, date, exact repository revision |
| Prior experience / DUT | HDL/toolchain familiarity and brief interface description |
| Environment ready | Time after dependencies and repository access work |
| First reference result | Elapsed time and errors encountered |
| Wrapper build / first DUT result | Elapsed times, descriptor count, checker result |
| First blocker / assistance | Error or confusing term, time, help provided |
| Outcome | Unassisted success, assisted success, incomplete, or target unsuitable |
| Follow-up change | Concrete README/template/build correction |

Keep every attempted session, including failures. Report actual counts and ranges, not an invented average or universal “30-minute integration” claim. Re-test documented blockers after changes and distinguish repeated participants from new users.
