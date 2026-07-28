# PLAN3 Step 6 Yinglong start serialization

Date: 2026-07-28

## Scope

This checkpoint answers only the software-visible Yinglong integration
question: can firmware deliver another SAU start while the current command is
busy? It does not claim behavior for a start forced directly onto `SA_CORE`,
and it does not use the non-authoritative `sim/testbench/sau_uvm` environment.

## Method

A temporary K768 diagnostic firmware omitted the completion poll after its
first retain command and immediately proceeded toward the next CSR sequence.
The normal Yinglong RTL and `SAU_REGRESS` testbench were otherwise unchanged.
The diagnostic source was removed after the experiment; the generated
firmware and waveform hashes below identify the executed artifacts.

Relevant RTL control is in
`hardware/src/crossbar/crossbar_mi.sv`. A master start moves the crossbar to
`ACTIVE`; CPU `dbus_req` service is only performed in `RVACTIVE`, and the
crossbar returns to `IDLE` after a master done.

## Evidence

- RTL simulation passed at `53042000 ps`; the result comparator intentionally
  used `elems=0` because this was a control-only probe.
- Attempted SAU starts appeared at `49137742`, `50269635`, and `51393193 ps`.
- Corresponding done edges appeared at `50086265`, `51218158`, and
  `52535088 ps`.
- At the 1667 ps SAU period, command extents were 569, 569, and 685 cycles.
- The next starts arrived 110 and 105 cycles after the preceding done edges.
- Every start therefore arrived with `csr_inst.busy=0` and
  `core_state_s=IDLE`; no start reached SAU while busy.

Artifacts:

- FSDB SHA-256:
  `3283d5006da469b97007ad3ea9825dbcbe15c05e15c5d45e77300df083d5256d`
- Control CSV:
  `/tmp/plan3_step6_busy_start_control.csv`
- Control CSV SHA-256:
  `a22bcbaaebd7d00dc2f41983635ec6afbb3b6124c5ad4a14abcb074e3452c66a`
- `instruction.hex` SHA-256:
  `603a41887b99dcca4c3e281339219289648565a949a852b9236b359aaece1e7a`
- `simv` SHA-256:
  `adce69439f65e5bab8acbd76257b58e5c0a09634035c255f7fefd36c6846ce08`
- NPU repository HEAD:
  `ad97aaa7fa6c70e40c8bf8cce07486628eea9b81`

## Contract

- Normal Yinglong software observes serialized SAU command admission.
- gem5 `sequential` fixture replay represents this host-visible policy.
- A raw fixture whose start overlaps an active command is invalid for this
  integrated software path and remains fail-fast; it is not an oracle for
  forced internal `SA_CORE` behavior.
- No claim is made about accepted/ignored semantics below the Yinglong
  crossbar boundary without a separate authoritative RTL environment.
